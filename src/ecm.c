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
#define fseeko _fseeki64
#define ftello _ftelli64
#define off_t  long long
#endif

#include "eccedc.h"
#include "version.h"

enum {
    /* Extra buffer space for alignment */
    INPUT_QUEUE_PADDING = 0x10,
    INPUT_QUEUE_SIZE = 1048576 + INPUT_QUEUE_PADDING
};

static const char *sector_type_names[] = {"literal", "mode1", "mode2f1", "mode2f2"};

static void banner(void) {
    fprintf(stderr, "ECM - Encoder for Error Code Modeler format v" ECM_VERSION "\n\n");
}

/*
 * Verify sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00
 */
static bool check_sync_pattern(const uint8_t *sector) {
    if (sector[0x00] != SYNC_BYTE_START)
        return false;
    for (int i = 1; i <= 10; i++) {
        if (sector[i] != SYNC_BYTE_MIDDLE)
            return false;
    }
    return sector[0x0B] == SYNC_BYTE_END;
}

/*
 * Verify reserved bytes are zero
 */
static bool check_reserved_zero(const uint8_t *sector) {
    for (int i = 0; i < RESERVED_SIZE; i++) {
        if (sector[OFFSET_MODE1_RESERVED + i] != 0)
            return false;
    }
    return true;
}

/*
 * Verify subheader duplication (bytes 0-3 == bytes 4-7)
 */
static bool check_subheader_dup(const uint8_t *sector) {
    return sector[0] == sector[4] && sector[1] == sector[5] && sector[2] == sector[6] &&
           sector[3] == sector[7];
}

/*
 * Sector type detection for raw 2352-byte sectors (with sync/header)
 *
 * NOTE: Parameter is non-const because ecc_verify() temporarily modifies
 * the address field for Mode 2 Form 1 verification (save/restore pattern).
 */
static sector_type_t check_type_raw(uint8_t *sector) {
    bool can_be_mode1 = true;
    bool can_be_mode2_form1 = true;
    bool can_be_mode2_form2 = true;

    /* All raw sectors must have sync pattern */
    if (!check_sync_pattern(sector)) {
        return SECTOR_TYPE_LITERAL;
    }

    /* Check mode byte to determine sector type */
    uint8_t mode = sector[OFFSET_MODE];
    if (mode == 0x01) {
        /* Mode 1 sector */
        can_be_mode2_form1 = false;
        can_be_mode2_form2 = false;
        if (!check_reserved_zero(sector)) {
            can_be_mode1 = false;
        }
    } else if (mode == 0x02) {
        /* Mode 2 sector - check subheader at offset 0x10 */
        can_be_mode1 = false;
        if (!check_subheader_dup(sector + OFFSET_MODE2_SUBHEADER)) {
            return SECTOR_TYPE_LITERAL;
        }
    } else {
        return SECTOR_TYPE_LITERAL;
    }

    /* EDC/ECC verification for Mode 1 */
    if (can_be_mode1) {
        uint32_t edc = edc_compute(0, sector, OFFSET_MODE1_EDC);
        if (!edc_check_bytes(edc, sector + OFFSET_MODE1_EDC)) {
            return SECTOR_TYPE_LITERAL;
        }
        if (!ecc_verify(sector, false, sector + OFFSET_MODE1_ECC_P)) {
            return SECTOR_TYPE_LITERAL;
        }
        return SECTOR_TYPE_MODE1;
    }

    /* EDC/ECC verification for Mode 2 Form 1 */
    if (can_be_mode2_form1) {
        uint32_t edc = edc_compute(0, sector + OFFSET_MODE2_SUBHEADER, MODE2_EDC_OFFSET);
        if (edc_check_bytes(edc, sector + OFFSET_MODE2_SUBHEADER + MODE2_EDC_OFFSET)) {
            if (ecc_verify(sector, true, sector + OFFSET_MODE1_ECC_P)) {
                return SECTOR_TYPE_MODE2_FORM1;
            }
        }
    }

    /* EDC verification for Mode 2 Form 2 */
    if (can_be_mode2_form2) {
        uint32_t edc = edc_compute(0, sector + OFFSET_MODE2_SUBHEADER, MODE2_FORM2_EDC_OFFSET);
        if (edc_check_bytes(edc, sector + OFFSET_MODE2_SUBHEADER + MODE2_FORM2_EDC_OFFSET)) {
            return SECTOR_TYPE_MODE2_FORM2;
        }
    }

    return SECTOR_TYPE_LITERAL;
}

/*
 * Encode a type/count combo to output
 */
[[nodiscard]] static int write_type_count(FILE *out, unsigned type, unsigned count) {
    count--;
    if (fputc(((count >= 32) << 7) | ((count & 31) << 2) | type, out) == EOF) {
        return -1;
    }
    count >>= 5;
    while (count) {
        if (fputc(((count >= 128) << 7) | (count & 127), out) == EOF) {
            return -1;
        }
        count >>= 7;
    }
    return 0;
}

/*
 * Progress tracking
 */
typedef struct {
    int64_t analyze;
    int64_t encode;
    int64_t total;
} progress_t;

static void progress_reset(progress_t *p, int64_t total) {
    p->analyze = 0;
    p->encode = 0;
    p->total = total;
}

static void progress_update(progress_t *p, int64_t analyze, int64_t encode) {
    bool changed = (analyze >> 20) != (p->analyze >> 20) || (encode >> 20) != (p->encode >> 20);
    p->analyze = analyze;
    p->encode = encode;

    if (changed) {
        int64_t d = (p->total + 64) / 128;
        if (!d)
            d = 1;
        fprintf(stderr, "Analyzing (%02u%%) Encoding (%02u%%)\r",
                (unsigned)((100 * ((analyze + 64) / 128)) / d),
                (unsigned)((100 * ((encode + 64) / 128)) / d));
    }
}

/*
 * Raw input bytes covered by one sector record.
 *
 * A Mode 1 record spans the whole 2352-byte raw sector because its address is stored in
 * the record. A Mode 2 record spans only the 2336-byte body that starts at the subheader:
 * the ECM format stores no sync, address, or mode for Mode 2, so the encoder carries those
 * 16 bytes as a literal run in front of the record (see queue_raw_sector()).
 */
static size_t record_input_size(sector_type_t type) {
    return type == SECTOR_TYPE_MODE1 ? SECTOR_SIZE_RAW : SECTOR_SIZE_MODE2;
}

/*
 * Write the stored payload of one sector record and fold the covered input into the EDC.
 * buf holds record_input_size(type) bytes: the raw sector for Mode 1, or the Mode 2 body
 * starting at the subheader for Form 1/2.
 */
static int write_sector_payload(const uint8_t *buf, FILE *out, uint32_t *edc, sector_type_t type) {
    switch (type) {
        case SECTOR_TYPE_MODE1:
            *edc = edc_compute(*edc, buf, SECTOR_SIZE_RAW);
            /* Address + user data; sync, mode, EDC, reserved, and ECC are regenerated */
            if (fwrite(buf + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, out) != MODE1_ADDRESS_SIZE ||
                fwrite(buf + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, out) != SECTOR_USER_DATA) {
                return -1;
            }
            return 0;

        case SECTOR_TYPE_MODE2_FORM1:
            *edc = edc_compute(*edc, buf, SECTOR_SIZE_MODE2);
            /* Second subheader copy + user data; first copy, EDC, and ECC are regenerated */
            if (fwrite(buf + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM1_DATA_SIZE, out) !=
                MODE2_FORM1_DATA_SIZE) {
                return -1;
            }
            return 0;

        case SECTOR_TYPE_MODE2_FORM2:
            *edc = edc_compute(*edc, buf, SECTOR_SIZE_MODE2);
            /* Second subheader copy + user data; first copy and EDC are regenerated */
            if (fwrite(buf + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM2_DATA_SIZE, out) !=
                MODE2_FORM2_DATA_SIZE) {
                return -1;
            }
            return 0;

        default:
            return -1;
    }
}

/*
 * Write a literal record: type/count header followed by the bytes themselves.
 */
static int write_literal_record(const uint8_t *buf, size_t len, FILE *out, uint32_t *edc) {
    if (write_type_count(out, SECTOR_TYPE_LITERAL, (unsigned)len) < 0 ||
        fwrite(buf, 1, len, out) != len) {
        return -1;
    }
    *edc = edc_compute(*edc, buf, len);
    return 0;
}

/*
 * Read one sector record's worth of input and write its payload (batch encode pass).
 */
static int write_sector_data(FILE *in, FILE *out, uint32_t *edc, sector_type_t type,
                             progress_t *progress) {
    uint8_t buf[SECTOR_SIZE_RAW];
    size_t size = record_input_size(type);

    if (fread(buf, 1, size, in) != size) {
        fprintf(stderr, "Error: unexpected end of input file\n");
        return -1;
    }
    if (write_sector_payload(buf, out, edc, type) < 0) {
        fprintf(stderr, "Error: failed to write output\n");
        return -1;
    }

    off_t pos = ftello(in);
    if (pos >= 0) {
        progress_update(progress, progress->analyze, pos);
    }
    return 0;
}

/*
 * Encode a run of sectors/literals of the same type
 */
static int flush_sector_run(uint32_t *edc, sector_type_t type, unsigned count, FILE *in, FILE *out,
                            progress_t *progress) {
    uint8_t buf[SECTOR_SIZE_RAW];

    if (write_type_count(out, type, count) < 0) {
        fprintf(stderr, "Error: failed to write output\n");
        return -1;
    }

    if (type == SECTOR_TYPE_LITERAL) {
        while (count) {
            unsigned b = (count > SECTOR_SIZE_RAW) ? SECTOR_SIZE_RAW : count;
            if (fread(buf, 1, b, in) != b) {
                fprintf(stderr, "Error: unexpected end of input file\n");
                return -1;
            }
            *edc = edc_compute(*edc, buf, b);
            if (fwrite(buf, 1, b, out) != b) {
                fprintf(stderr, "Error: failed to write output\n");
                return -1;
            }
            count -= b;
            off_t pos = ftello(in);
            if (pos >= 0) {
                progress_update(progress, progress->analyze, pos);
            }
        }
        return 0;
    }

    while (count--) {
        if (write_sector_data(in, out, edc, type, progress) < 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Write magic header to output file.
 */
[[nodiscard]] static int write_magic_header(FILE *out) {
    if (fputc(ECM_MAGIC_E, out) == EOF || fputc(ECM_MAGIC_C, out) == EOF ||
        fputc(ECM_MAGIC_M, out) == EOF || fputc(ECM_MAGIC_NULL, out) == EOF) {
        fprintf(stderr, "Error: failed to write magic header\n");
        return -1;
    }
    return 0;
}

/*
 * Write EDC checksum to output file.
 */
[[nodiscard]] static int write_edc_checksum(FILE *out, uint32_t edc) {
    if (fputc((edc >> 0) & 0xFF, out) == EOF || fputc((edc >> 8) & 0xFF, out) == EOF ||
        fputc((edc >> 16) & 0xFF, out) == EOF || fputc((edc >> 24) & 0xFF, out) == EOF) {
        fprintf(stderr, "Error: failed to write EDC checksum\n");
        return -1;
    }
    return 0;
}

/*
 * Print encoding statistics report.
 */
static void print_report(const unsigned typetally[4], int64_t total_in, FILE *out) {
    fprintf(stderr, "Literal bytes........... %10u\n", typetally[SECTOR_TYPE_LITERAL]);
    fprintf(stderr, "Mode 1 sectors.......... %10u\n", typetally[SECTOR_TYPE_MODE1]);
    fprintf(stderr, "Mode 2 form 1 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM1]);
    fprintf(stderr, "Mode 2 form 2 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM2]);
    off_t outpos = ftello(out);
    fprintf(stderr, "Encoded %lld bytes -> %lld bytes\n", (long long)total_in,
            (long long)(outpos >= 0 ? outpos : 0));
    fprintf(stderr, "Done.\n");
}

/*
 * Streaming mode encoder - processes input without seeking.
 * Works with stdin/pipes.
 *
 * Trade-offs vs batch mode:
 * - Lower memory usage (~2.4 KB vs ~1 MB buffer)
 * - Processes one sector at a time (no batching of same-type runs)
 * - Higher output overhead for consecutive same-type sectors
 * - Works with non-seekable streams (stdin, pipes)
 */
static int ecmify_streaming(FILE *in, FILE *out, bool verbose) {
    uint8_t buf[SECTOR_SIZE_RAW];
    uint32_t inedc = 0;
    unsigned typetally[4] = {0};
    int64_t total_in = 0;
    unsigned sector_num = 0;

    ECM_VERBOSE(verbose, "Using streaming mode (stdin/pipe)");

    if (write_magic_header(out) < 0) {
        return 1;
    }

    for (;;) {
        size_t dataavail = fread(buf, 1, SECTOR_SIZE_RAW, in);
        if (dataavail == 0) {
            break;
        }
        total_in += (int64_t)dataavail;

        sector_type_t type =
            dataavail < SECTOR_SIZE_RAW ? SECTOR_TYPE_LITERAL : check_type_raw(buf);

        ECM_VERBOSE(verbose, "Sector %u: type=%s, size=%zu", sector_num++, sector_type_names[type],
                    dataavail);

        if (type == SECTOR_TYPE_LITERAL) {
            if (write_literal_record(buf, dataavail, out, &inedc) < 0) {
                goto writeerr;
            }
            typetally[SECTOR_TYPE_LITERAL] += (unsigned)dataavail;
            continue;
        }

        const uint8_t *record = buf;
        if (type != SECTOR_TYPE_MODE1) {
            /* Sync + header travel as literal bytes; see queue_raw_sector() */
            if (write_literal_record(buf, SECTOR_SYNC_HEADER_SIZE, out, &inedc) < 0) {
                goto writeerr;
            }
            typetally[SECTOR_TYPE_LITERAL] += SECTOR_SYNC_HEADER_SIZE;
            record = buf + SECTOR_SYNC_HEADER_SIZE;
        }
        if (write_type_count(out, type, 1) < 0 ||
            write_sector_payload(record, out, &inedc, type) < 0) {
            goto writeerr;
        }
        typetally[type]++;
    }

    if (write_type_count(out, 0, 0) < 0) {
        fprintf(stderr, "Error: failed to write end marker\n");
        return 1;
    }

    if (write_edc_checksum(out, inedc) < 0) {
        return 1;
    }

    print_report(typetally, total_in, out);
    return 0;

writeerr:
    fprintf(stderr, "Error: failed to write output\n");
    return 1;
}

/*
 * A pending run of same-type records in batch mode. Runs are described by input offset so
 * the encode pass can seek back and re-read them; count is bytes for literal runs and
 * sectors otherwise.
 */
typedef struct {
    sector_type_t type;
    int64_t start;
    int64_t count;
} run_t;

typedef struct {
    FILE *in;
    FILE *out;
    uint32_t edc;
    unsigned typetally[4];
    progress_t progress;
    run_t run;
    bool verbose;
} batch_encoder_t;

/*
 * Encode the pending run, if any, by seeking back to where it started.
 */
static int run_flush(batch_encoder_t *enc, const char *why) {
    run_t *run = &enc->run;

    if (run->count == 0) {
        return 0;
    }

    ECM_VERBOSE(enc->verbose, "%s: type=%s, count=%lld", why, sector_type_names[run->type],
                (long long)run->count);
    if (fseeko(enc->in, (off_t)run->start, SEEK_SET) != 0) {
        fprintf(stderr, "Error: failed to seek input file\n");
        return -1;
    }
    enc->typetally[run->type] += (unsigned)run->count;
    if (flush_sector_run(&enc->edc, run->type, (unsigned)run->count, enc->in, enc->out,
                         &enc->progress) < 0) {
        return -1;
    }
    run->count = 0;
    return 0;
}

/*
 * Append count units of the given type found at input offset start, flushing the pending
 * run when the type changes or its count nears the 32-bit type/count limit.
 */
static int run_add(batch_encoder_t *enc, sector_type_t type, int64_t start, int64_t count) {
    run_t *run = &enc->run;

    if (run->count != 0 && run->type != type && run_flush(enc, "Flushing batch") < 0) {
        return -1;
    }
    if (run->count == 0) {
        run->type = type;
        run->start = start;
    }
    run->count += count;

    if (run->count >= (int64_t)(UINT32_MAX - SECTOR_SIZE_RAW)) {
        return run_flush(enc, "Splitting batch");
    }
    return 0;
}

/*
 * Queue one raw 2352-byte sector found at input offset pos.
 *
 * Mode 2 sectors become a 16-byte literal run (sync, address, mode) followed by a type 2/3
 * record for the body. That is the layout the upstream encoder produces, and it is what
 * keeps the original address in the stream: the record itself has no field for it.
 */
static int queue_raw_sector(batch_encoder_t *enc, sector_type_t type, int64_t pos) {
    switch (type) {
        case SECTOR_TYPE_LITERAL:
            return run_add(enc, SECTOR_TYPE_LITERAL, pos, SECTOR_SIZE_RAW);
        case SECTOR_TYPE_MODE1:
            return run_add(enc, SECTOR_TYPE_MODE1, pos, 1);
        case SECTOR_TYPE_MODE2_FORM1:
        case SECTOR_TYPE_MODE2_FORM2:
            if (run_add(enc, SECTOR_TYPE_LITERAL, pos, SECTOR_SYNC_HEADER_SIZE) < 0) {
                return -1;
            }
            return run_add(enc, type, pos + SECTOR_SYNC_HEADER_SIZE, 1);
    }
    return -1;
}

/*
 * Batch mode encoding function - uses seeking for better performance.
 *
 * Trade-offs vs streaming mode:
 * - Uses ~1 MB input buffer for efficient I/O
 * - Batches consecutive same-type sectors into single records
 * - Better compression ratio for homogeneous sector runs
 * - Requires seekable input (regular files only)
 * - Two-pass: analyze then encode
 */
static int ecmify(FILE *in, FILE *out, bool verbose) {
    uint8_t *inputqueue = nullptr;
    batch_encoder_t enc = {.in = in, .out = out, .verbose = verbose};
    int64_t incheckpos = 0;
    int64_t inbufferpos = 0;
    int64_t intotallength;
    size_t inqueuestart = 0;
    size_t dataavail = 0;
    int result = 0;

    ECM_VERBOSE(verbose, "Using batch mode (seekable file)");

    inputqueue = malloc(INPUT_QUEUE_SIZE);
    if (!inputqueue) {
        fprintf(stderr, "Error: failed to allocate input buffer\n");
        return 1;
    }

    if (fseeko(in, 0, SEEK_END) != 0) {
        fprintf(stderr, "Error: failed to seek input file\n");
        free(inputqueue);
        return 1;
    }
    off_t endpos = ftello(in);
    if (endpos < 0) {
        fprintf(stderr, "Error: failed to determine input file size\n");
        free(inputqueue);
        return 1;
    }
    intotallength = (int64_t)endpos;
    if (fseeko(in, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Error: failed to rewind input file\n");
        free(inputqueue);
        return 1;
    }
    progress_reset(&enc.progress, intotallength);

    if (write_magic_header(out) < 0) {
        free(inputqueue);
        return 1;
    }

    for (;;) {
        /* Fill input buffer if needed */
        if (dataavail < SECTOR_SIZE_RAW && (int64_t)dataavail < intotallength - inbufferpos) {
            size_t willread = (size_t)(intotallength - inbufferpos);
            if (willread > (INPUT_QUEUE_SIZE - INPUT_QUEUE_PADDING) - dataavail) {
                willread = (INPUT_QUEUE_SIZE - INPUT_QUEUE_PADDING) - dataavail;
            }
            if (inqueuestart) {
                memmove(inputqueue + INPUT_QUEUE_PADDING,
                        inputqueue + INPUT_QUEUE_PADDING + inqueuestart, dataavail);
                inqueuestart = 0;
            }
            if (willread) {
                progress_update(&enc.progress, inbufferpos, enc.progress.encode);
                if (fseeko(in, (off_t)inbufferpos, SEEK_SET) != 0) {
                    fprintf(stderr, "Error: failed to seek input file\n");
                    result = 1;
                    goto cleanup;
                }
                if (fread(inputqueue + INPUT_QUEUE_PADDING + dataavail, 1, willread, in) !=
                    willread) {
                    fprintf(stderr, "Error: failed to read input file\n");
                    result = 1;
                    goto cleanup;
                }
                inbufferpos += (int64_t)willread;
                dataavail += willread;
            }
        }

        if (dataavail == 0)
            break;

        /* Classify at raw-sector granularity; a short tail can only be literal */
        size_t step;
        int rc;
        if (dataavail < SECTOR_SIZE_RAW) {
            step = dataavail;
            rc = run_add(&enc, SECTOR_TYPE_LITERAL, incheckpos, (int64_t)step);
        } else {
            step = SECTOR_SIZE_RAW;
            sector_type_t type = check_type_raw(inputqueue + INPUT_QUEUE_PADDING + inqueuestart);
            rc = queue_raw_sector(&enc, type, incheckpos);
        }
        if (rc < 0) {
            result = 1;
            goto cleanup;
        }

        incheckpos += (int64_t)step;
        inqueuestart += step;
        dataavail -= step;
    }

    if (run_flush(&enc, "Flushing final batch") < 0) {
        result = 1;
        goto cleanup;
    }

    if (write_type_count(out, 0, 0) < 0) {
        fprintf(stderr, "Error: failed to write end marker\n");
        result = 1;
        goto cleanup;
    }

    if (write_edc_checksum(out, enc.edc) < 0) {
        result = 1;
        goto cleanup;
    }

    print_report(enc.typetally, intotallength, out);

cleanup:
    free(inputqueue);
    return result;
}

/*
 * Check if filename is "-" for stdin/stdout
 */
static bool is_stdio(const char *filename) {
    return filename[0] == '-' && filename[1] == '\0';
}

int main(int argc, char **argv) {
    FILE *fin = nullptr;
    FILE *fout = nullptr;
    const char *infilename;
    char *outfilename = nullptr;
    bool outfilename_allocated = false;
    bool verbose = false;
    int result = 0;
    int argoffset = 0;

    banner();
    eccedc_init();

    /* Check for -v/--verbose option */
    if (argc >= 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0)) {
        verbose = true;
        argoffset = 1;
    }

    if (argc < 2 + argoffset || argc > 3 + argoffset) {
        fprintf(stderr, "usage: %s [-v|--verbose] cdimagefile [ecmfile]\n", argv[0]);
        fprintf(stderr, "       use '-' for stdin/stdout\n");
        return 1;
    }

    infilename = argv[1 + argoffset];

    /* Determine output filename */
    if (argc == 3 + argoffset) {
        outfilename = argv[2 + argoffset];
    } else if (is_stdio(infilename)) {
        outfilename = "-";
    } else {
        size_t len = strlen(infilename);
        outfilename = malloc(len + 5);
        if (!outfilename) {
            fprintf(stderr, "Error: failed to allocate output filename\n");
            return 1;
        }
        outfilename_allocated = true;
        snprintf(outfilename, len + 5, "%s.ecm", infilename);
    }

    fprintf(stderr, "Encoding %s to %s.\n", infilename, outfilename);

    /* Open input */
    if (is_stdio(infilename)) {
        fin = stdin;
    } else {
        fin = fopen(infilename, "rb");
        if (!fin) {
            perror(infilename);
            result = 1;
            goto cleanup;
        }
    }

    /* Open output */
    if (is_stdio(outfilename)) {
        fout = stdout;
    } else {
        fout = fopen(outfilename, "wb");
        if (!fout) {
            perror(outfilename);
            result = 1;
            goto cleanup;
        }
    }

    /* Use streaming mode for stdin, batch mode for regular files */
    if (is_stdio(infilename)) {
        result = ecmify_streaming(fin, fout, verbose);
    } else {
        result = ecmify(fin, fout, verbose);
    }

cleanup:
    if (fout && !is_stdio(outfilename)) {
        fclose(fout);
    }
    if (fin && !is_stdio(infilename)) {
        fclose(fin);
    }
    if (outfilename_allocated) {
        free(outfilename);
    }

    return result;
}
