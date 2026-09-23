#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(_WIN32) || defined(_WIN64)
/* MinGW-w64 already maps these to their 64-bit forms; only MSVC-style CRTs lack them */
#ifndef fseeko
#define fseeko _fseeki64
#endif
#ifndef ftello
#define ftello _ftelli64
#endif
#define off_t long long
#endif

#include "eccedc.h"
#include "version.h"

static void banner(void) {
    fprintf(stderr, "UNECM - Decoder for Error Code Modeler format v" ECM_VERSION "\n\n");
}

/*
 * Progress tracking
 */
typedef struct {
    int64_t current;
    int64_t total;
    bool show; /* only a terminal gets the self-overwriting progress line */
} progress_t;

static void progress_reset(progress_t *p, int64_t total) {
    p->current = 0;
    p->total = total;
    p->show = stream_is_terminal(stderr);
}

static void progress_update(progress_t *p, int64_t n) {
    if (p->show && (n >> 20) != (p->current >> 20)) {
        int64_t d = (p->total + 64) / 128;
        if (!d)
            d = 1;
        fprintf(stderr, "Decoding (%02u%%)\r", (unsigned)((100 * ((n + 64) / 128)) / d));
    }
    p->current = n;
}

/*
 * Decode statistics for CUE file generation. A Mode 2 record completes a raw 2352-byte sector
 * when the 16 bytes written just before it are a sync pattern and header, or when
 * --mode2-2352 regenerates them; otherwise it leaves a bare 2336-byte body.
 */
typedef struct {
    bool saw_mode1;
    bool saw_mode2_raw;
    bool saw_mode2_bare;
} decode_stats_t;

/*
 * The last SECTOR_SYNC_HEADER_SIZE bytes written, used to tell whether a Mode 2 record
 * follows its own sync and header.
 */
typedef struct {
    uint8_t bytes[SECTOR_SYNC_HEADER_SIZE];
    int64_t written;
} output_tail_t;

static void tail_push(output_tail_t *t, const uint8_t *data, size_t len) {
    if (len >= SECTOR_SYNC_HEADER_SIZE) {
        memcpy(t->bytes, data + len - SECTOR_SYNC_HEADER_SIZE, SECTOR_SYNC_HEADER_SIZE);
    } else {
        memmove(t->bytes, t->bytes + len, SECTOR_SYNC_HEADER_SIZE - len);
        memcpy(t->bytes + SECTOR_SYNC_HEADER_SIZE - len, data, len);
    }
    t->written += (int64_t)len;
}

static bool tail_is_mode2_header(const output_tail_t *t) {
    static const uint8_t sync[OFFSET_HEADER] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    return t->written >= SECTOR_SYNC_HEADER_SIZE && memcmp(t->bytes, sync, sizeof(sync)) == 0 &&
           t->bytes[OFFSET_MODE] == 0x02;
}

/*
 * Read and verify magic header
 */
static bool read_magic_header(FILE *in) {
    int magic[4];
    if ((magic[0] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }
    if ((magic[1] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }
    if ((magic[2] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }
    if ((magic[3] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }

    if (magic[0] != ECM_MAGIC_E || magic[1] != ECM_MAGIC_C || magic[2] != ECM_MAGIC_M ||
        magic[3] != ECM_MAGIC_NULL) {
        fprintf(stderr, "Header not found!\n");
        return false;
    }
    return true;
}

/*
 * Read type/count from input
 */
static bool read_type_count(FILE *in, unsigned *type, unsigned *num) {
    int c = fgetc(in);
    int bits = 5;

    if (c == EOF)
        return false;

    *type = (unsigned)(c & 3);
    *num = (unsigned)((c >> 2) & 0x1F);

    while (c & 0x80) {
        c = fgetc(in);
        if (c == EOF)
            return false;
        if (bits >= TYPE_COUNT_MAX_BITS) {
            fprintf(stderr, "Error: invalid type/count encoding (overflow)\n");
            return false;
        }
        *num |= ((unsigned)(c & 0x7F)) << bits;
        bits += 7;
    }
    return true;
}

/*
 * Decode a Mode 1 sector into a full 2352-byte raw sector.
 */
static int decode_mode1_sector(FILE *in, FILE *out, uint8_t *sector, uint32_t *checkedc) {
    memset(sector, 0, SECTOR_SIZE_RAW);
    sector_init_sync(sector);
    sector[OFFSET_MODE] = 0x01;

    if (fread(sector + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, in) != MODE1_ADDRESS_SIZE) {
        return -1;
    }
    if (fread(sector + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, in) != SECTOR_USER_DATA) {
        return -1;
    }
    eccedc_generate(sector, SECTOR_TYPE_MODE1);
    *checkedc = edc_compute(*checkedc, sector, SECTOR_SIZE_RAW);
    if (fwrite(sector, SECTOR_SIZE_RAW, 1, out) != 1) {
        fprintf(stderr, "Error: failed to write output\n");
        return -2;
    }
    return 0;
}

/* Convert a binary value (0-99) to packed BCD */
static uint8_t to_bcd(uint8_t value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/*
 * Write the 16-byte sync + header that versions 1.2.0 to 1.3.1 regenerated for every Mode 2
 * record: sync pattern, the MSF address of the sector number that the output position
 * implies (counting from 00:02:00, the standard 150-frame pregap), and mode byte 2.
 * The sector number counts every output byte, literal runs included, exactly as those
 * versions did, so their archives decode back to the same image.
 */
static void regenerate_mode2_header(uint8_t *sector, int64_t outbytes) {
    uint32_t frame = (uint32_t)(outbytes / SECTOR_SIZE_RAW) + 150;

    sector_init_sync(sector);
    sector[OFFSET_HEADER + 2] = to_bcd((uint8_t)(frame % 75));
    frame /= 75;
    sector[OFFSET_HEADER + 1] = to_bcd((uint8_t)(frame % 60));
    sector[OFFSET_HEADER + 0] = to_bcd((uint8_t)(frame / 60));
    sector[OFFSET_MODE] = 0x02;
}

/*
 * Decode a Mode 2 Form 1 or Form 2 record into the 2336-byte body that follows sync +
 * header in a raw sector.
 *
 * The record carries no sync, address, or mode: the encoder stores those 16 bytes as a
 * literal run in front of the record, so emitting them here would duplicate them and
 * corrupt spec-conformant streams. The address is zeroed for Mode 2 ECC anyway, so the
 * header area of the scratch buffer only needs to be deterministic, not meaningful.
 *
 * With mode2_2352 the record instead expands to a full 2352-byte sector with a regenerated
 * header, which is what versions 1.2.0 to 1.3.1 wrote. Only archives made by those versions
 * from raw Mode 2 images need this; their encoder dropped the header instead of storing it.
 * The trailing EDC covers the 2336-byte body either way, as those versions computed it.
 */
static int decode_mode2_sector(FILE *in, FILE *out, uint8_t *sector, uint32_t *checkedc,
                               sector_type_t type, bool mode2_2352, int64_t outbytes) {
    size_t payload =
        type == SECTOR_TYPE_MODE2_FORM1 ? MODE2_FORM1_DATA_SIZE : MODE2_FORM2_DATA_SIZE;
    const uint8_t *emit = mode2_2352 ? sector : sector + OFFSET_MODE2_SUBHEADER;
    size_t emit_len = mode2_2352 ? SECTOR_SIZE_RAW : SECTOR_SIZE_MODE2;

    memset(sector, 0, SECTOR_SIZE_RAW);
    if (fread(sector + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, payload, in) != payload) {
        return -1;
    }
    sector_copy_subheader(sector);
    eccedc_generate(sector, type);
    if (mode2_2352) {
        regenerate_mode2_header(sector, outbytes);
    }
    *checkedc = edc_compute(*checkedc, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
    if (fwrite(emit, emit_len, 1, out) != 1) {
        fprintf(stderr, "Error: failed to write output\n");
        return -2;
    }
    return 0;
}

/*
 * Main decoding function
 */
static int unecmify(FILE *in, FILE *out, decode_stats_t *stats, bool is_stdin, bool verbose,
                    bool mode2_2352) {
    static const char *type_names[] = {"literal", "mode1", "mode2f1", "mode2f2"};
    uint32_t checkedc = 0;
    uint8_t sector[SECTOR_SIZE_RAW];
    progress_t progress;
    output_tail_t tail = {{0}, 0};

    /* For regular files, get size for progress tracking; for stdin, skip */
    if (!is_stdin) {
        if (fseeko(in, 0, SEEK_END) != 0) {
            fprintf(stderr, "Error: failed to seek input file\n");
            return 1;
        }
        off_t endpos = ftello(in);
        if (endpos < 0) {
            fprintf(stderr, "Error: failed to determine input file size\n");
            return 1;
        }
        progress_reset(&progress, endpos);
        if (fseeko(in, 0, SEEK_SET) != 0) {
            fprintf(stderr, "Error: failed to rewind input file\n");
            return 1;
        }
    } else {
        progress_reset(&progress, 0);
    }

    if (!read_magic_header(in)) {
        goto corrupt;
    }

    if (mode2_2352) {
        ECM_VERBOSE(verbose, "Regenerating sync + header for Mode 2 records (1.2.0-1.3.1 layout)");
    }

    for (;;) {
        unsigned type, num;
        if (!read_type_count(in, &type, &num)) {
            goto uneof;
        }

        if (num == 0xFFFFFFFF)
            break;
        num++;

        ECM_VERBOSE(verbose, "Record: type=%s, count=%u", type_names[type], num);

        if (type == SECTOR_TYPE_LITERAL) {
            while (num) {
                unsigned b = (num > SECTOR_SIZE_RAW) ? SECTOR_SIZE_RAW : num;
                if (fread(sector, 1, b, in) != b) {
                    goto uneof;
                }
                checkedc = edc_compute(checkedc, sector, b);
                if (fwrite(sector, 1, b, out) != b) {
                    fprintf(stderr, "Error: failed to write output\n");
                    goto writeerr;
                }
                tail_push(&tail, sector, b);
                num -= b;
                off_t pos = ftello(in);
                if (pos >= 0)
                    progress_update(&progress, pos);
            }
            continue;
        }

        while (num--) {
            int ret;
            switch ((sector_type_t)type) {
                case SECTOR_TYPE_MODE1:
                    if (stats)
                        stats->saw_mode1 = true;
                    ret = decode_mode1_sector(in, out, sector, &checkedc);
                    if (ret == 0)
                        tail_push(&tail, sector, SECTOR_SIZE_RAW);
                    break;
                case SECTOR_TYPE_MODE2_FORM1:
                case SECTOR_TYPE_MODE2_FORM2:
                    if (stats) {
                        if (mode2_2352 || tail_is_mode2_header(&tail))
                            stats->saw_mode2_raw = true;
                        else
                            stats->saw_mode2_bare = true;
                    }
                    ret = decode_mode2_sector(in, out, sector, &checkedc, (sector_type_t)type,
                                              mode2_2352, tail.written);
                    if (ret == 0 && mode2_2352)
                        tail_push(&tail, sector, SECTOR_SIZE_RAW);
                    else if (ret == 0)
                        tail_push(&tail, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
                    break;
                default:
                    fprintf(stderr, "Error: invalid sector type %u\n", type);
                    goto corrupt;
            }
            if (ret == -1)
                goto uneof;
            if (ret == -2)
                goto writeerr;

            off_t pos = ftello(in);
            if (pos >= 0)
                progress_update(&progress, pos);
        }
    }

    /* Read and verify final EDC */
    if (fread(sector, 1, EDC_SIZE, in) != EDC_SIZE) {
        goto uneof;
    }

    /* A pipe has no meaningful position; the output size is counted as it is written */
    off_t inpos = stream_is_regular_file(in) ? ftello(in) : -1;
    if (inpos >= 0) {
        fprintf(stderr, "Decoded %lld bytes -> %lld bytes\n", (long long)inpos,
                (long long)tail.written);
    } else {
        fprintf(stderr, "Decoded to %lld bytes\n", (long long)tail.written);
    }

    if (sector[0] != ((checkedc >> 0) & 0xFF) || sector[1] != ((checkedc >> 8) & 0xFF) ||
        sector[2] != ((checkedc >> 16) & 0xFF) || sector[3] != ((checkedc >> 24) & 0xFF)) {
        fprintf(stderr, "EDC error (%08X, should be %02X%02X%02X%02X)\n", checkedc, sector[3],
                sector[2], sector[1], sector[0]);
        goto corrupt;
    }

    fprintf(stderr, "Done; file is OK\n");
    return 0;

uneof:
    fprintf(stderr, "Unexpected EOF!\n");
corrupt:
    fprintf(stderr, "Corrupt ECM file!\n");
    return 1;

writeerr:
    return 1;
}

/*
 * Track mode for the CUE sheet. Raw Mode 2 sectors make a MODE2/2352 image, while bare
 * 2336-byte bodies with nothing else make MODE2/2336, as in archives of MODE2/2336 images or
 * 1.2.0-1.3.1 archives decoded without --mode2-2352. An image mixing both sizes has no single
 * sector size; it keeps MODE2/2352 and write_cue_file() warns.
 */
static const char *cue_track_mode(const decode_stats_t *stats) {
    if (stats == nullptr) {
        return "MODE2/2352";
    }
    if (stats->saw_mode2_bare && !stats->saw_mode2_raw && !stats->saw_mode1) {
        return "MODE2/2336";
    }
    if (stats->saw_mode2_raw || stats->saw_mode2_bare) {
        return "MODE2/2352";
    }
    return "MODE1/2352";
}

/*
 * Write a CUE file for the decoded BIN.
 *
 * The sheet is created next to the image as <output>.cue, and players resolve its FILE entry
 * relative to the sheet, so the entry names the image by its file name alone. The output
 * path as typed would point nowhere once it contains a directory.
 */
static int write_cue_file(const char *outfilename, const decode_stats_t *stats) {
    size_t outlen = strlen(outfilename);
    char *cuefilename = nullptr;
    FILE *cuefile = nullptr;
    int result = 0;

    cuefilename = malloc(outlen + 5);
    if (!cuefilename) {
        fprintf(stderr, "Error: failed to allocate cue filename\n");
        return 1;
    }

    memcpy(cuefilename, outfilename, outlen);
    memcpy(cuefilename + outlen, ".cue", 5);

    cuefile = fopen(cuefilename, "wt");
    if (!cuefile) {
        perror(cuefilename);
        result = 1;
        goto cleanup;
    }

    fprintf(cuefile, "FILE \"%s\" BINARY\n", path_basename(outfilename));
    fprintf(cuefile, "  TRACK 01 %s\n", cue_track_mode(stats));
    fprintf(cuefile, "    INDEX 01 00:00:00\n");

    if (output_finish(cuefile, cuefilename) != 0) {
        remove(cuefilename);
        result = 1;
        goto cleanup;
    }
    if (stats && stats->saw_mode2_bare && (stats->saw_mode2_raw || stats->saw_mode1)) {
        fprintf(stderr, "Warning: the image mixes 2336-byte Mode 2 bodies with 2352-byte "
                        "sectors; no single CUE track mode describes it\n");
    }

    fprintf(stderr, "Created CUE file: %s\n", cuefilename);

cleanup:
    free(cuefilename);
    return result;
}

/*
 * Check if filename is "-" for stdin/stdout
 */
static bool is_stdio(const char *filename) {
    return filename[0] == '-' && filename[1] == '\0';
}

static void usage(FILE *f, const char *prog) {
    fprintf(f, "usage: %s [-v|--verbose] [--cue] [--mode2-2352] ecmfile [outputfile]\n", prog);
    fprintf(f, "       %s -h|--help | -V|--version\n", prog);
    fprintf(f, "       use '-' for stdin/stdout, and '--' before file names starting with '-'\n");
    fprintf(f, "       --mode2-2352  expand Mode 2 records to 2352-byte sectors with\n");
    fprintf(f, "                     regenerated headers (archives from versions 1.2.0-1.3.1)\n");
}

int main(int argc, char **argv) {
    FILE *fin = nullptr;
    FILE *fout = nullptr;
    char *files[2] = {nullptr, nullptr};
    int nfiles = 0;
    bool options_done = false;
    const char *infilename;
    char *outfilename = nullptr;
    bool outfilename_allocated = false;
    bool out_is_file = false;
    bool createcue = false;
    bool verbose = false;
    bool mode2_2352 = false;
    int result = 0;
    decode_stats_t stats = {false, false, false};

    /* Options may appear anywhere; '--' ends them, and a lone '-' is a file name */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!options_done && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "--") == 0) {
                options_done = true;
            } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
                verbose = true;
            } else if (strcasecmp(arg, "--cue") == 0) {
                createcue = true;
            } else if (strcasecmp(arg, "--mode2-2352") == 0) {
                mode2_2352 = true;
            } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
                usage(stdout, argv[0]);
                return 0;
            } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
                printf("unecm " ECM_VERSION "\n");
                return 0;
            } else {
                banner();
                fprintf(stderr, "unknown option: %s\n", arg);
                usage(stderr, argv[0]);
                return 1;
            }
            continue;
        }
        if (nfiles == 2) {
            banner();
            fprintf(stderr, "too many file names: %s\n", arg);
            usage(stderr, argv[0]);
            return 1;
        }
        files[nfiles++] = argv[i];
    }

    banner();
    if (nfiles == 0) {
        usage(stderr, argv[0]);
        return 1;
    }
    eccedc_init();

    infilename = files[0];

    /* Verify input filename ends with .ecm (unless stdin) */
    if (!is_stdio(infilename)) {
        size_t inlen = strlen(infilename);
        if (inlen < 5) {
            fprintf(stderr, "filename '%s' is too short\n", infilename);
            return 1;
        }
        if (strcasecmp(infilename + inlen - 4, ".ecm") != 0) {
            fprintf(stderr, "filename must end in .ecm\n");
            return 1;
        }
    }

    /* Figure out output filename */
    if (nfiles == 2) {
        outfilename = files[1];
    } else if (is_stdio(infilename)) {
        outfilename = "-";
    } else {
        size_t inlen = strlen(infilename);
        outfilename = malloc(inlen - 3);
        if (!outfilename) {
            fprintf(stderr, "Error: failed to allocate output filename\n");
            return 1;
        }
        outfilename_allocated = true;
        memcpy(outfilename, infilename, inlen - 4);
        outfilename[inlen - 4] = '\0';
    }

    fprintf(stderr, "Decoding %s to %s.\n", infilename, outfilename);

    /* Open input */
    if (is_stdio(infilename)) {
        fin = stdin;
        if (!stream_set_binary(fin)) {
            fprintf(stderr, "Error: failed to set stdin to binary mode\n");
            result = 1;
            goto cleanup;
        }
    } else {
        fin = fopen(infilename, "rb");
        if (!fin) {
            perror(infilename);
            result = 1;
            goto cleanup;
        }
    }

    /* Opening the output truncates it, so refuse any path that aliases the input */
    if (!is_stdio(outfilename) && file_is_same_as_path(fin, outfilename)) {
        fprintf(stderr, "Error: input and output are the same file: %s\n", outfilename);
        result = 1;
        goto cleanup;
    }

    /* Open output */
    if (is_stdio(outfilename)) {
        fout = stdout;
        if (!stream_set_binary(fout)) {
            fprintf(stderr, "Error: failed to set stdout to binary mode\n");
            result = 1;
            goto cleanup;
        }
    } else {
        fout = fopen(outfilename, "wb");
        if (!fout) {
            perror(outfilename);
            result = 1;
            goto cleanup;
        }
        out_is_file = stream_is_regular_file(fout);
    }

    result = unecmify(fin, fout, &stats, is_stdio(infilename), verbose, mode2_2352);

    /* Finish the BIN before writing a CUE so a truncated image never gets a sheet */
    if (output_finish(fout, is_stdio(outfilename) ? "stdout" : outfilename) != 0 && result == 0) {
        result = 1;
    }
    fout = nullptr;

    /* A truncated or unverified image under the requested name would pass for a good one */
    if (result != 0 && out_is_file && remove(outfilename) == 0) {
        fprintf(stderr, "Removed incomplete output %s\n", outfilename);
    }

    /* Write CUE file if requested */
    if (result == 0 && createcue && !is_stdio(outfilename)) {
        result = write_cue_file(outfilename, &stats);
    }

cleanup:
    if (fin && !is_stdio(infilename)) {
        fclose(fin);
    }
    if (outfilename_allocated) {
        free(outfilename);
    }

    return result;
}
