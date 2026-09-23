#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <errno.h>
#include <inttypes.h>
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

enum {
    /* Extra buffer space for alignment */
    INPUT_QUEUE_PADDING = 0x10,
    INPUT_QUEUE_SIZE = 1048576 + INPUT_QUEUE_PADDING,
    /* Streaming read-ahead; any size well above one sector works */
    STREAM_BUFFER_SIZE = 262144,
    /* Largest count one record may carry: the format declares 2^31 and above invalid */
    RECORD_COUNT_MAX = 0x7FFFFFFF,
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
 * EDCs of the two windows a header-less Mode 2 body is checked against: bytes 0..0x807 for
 * Form 1 and 0..0x91B for Form 2. Scanning repetitive literal data such as 0xFF padding passes
 * the subheader test at nearly every byte, and recomputing both windows there would cost
 * about 4 KB of work per byte, so they are rolled forward one byte at a time instead.
 */
typedef struct {
    const uint8_t *at; /* body start the EDCs below describe, or nullptr when unknown */
    uint32_t form1;    /* EDC of at[0 .. MODE2_EDC_OFFSET) */
    uint32_t form2;    /* EDC of at[0 .. MODE2_FORM2_EDC_OFFSET) */
} body_edc_t;

static edc_window_t form1_window;
static edc_window_t form2_window;
static bool body_windows_ready = false;

static void body_windows_init(void) {
    if (!body_windows_ready) {
        edc_window_init(&form1_window, MODE2_EDC_OFFSET);
        edc_window_init(&form2_window, MODE2_FORM2_EDC_OFFSET);
        body_windows_ready = true;
    }
}

/*
 * Bring the window EDCs to the body starting at p. Rolls forward from the previous position
 * when that is cheaper than recomputing. Callers reset s->at whenever the buffer moves.
 */
static void body_edc_at(body_edc_t *s, const uint8_t *p) {
    if (s->at == p) {
        return;
    }
    if (s->at != nullptr && s->at < p && (size_t)(p - s->at) < MODE2_FORM2_EDC_OFFSET) {
        for (; s->at < p; s->at++) {
            s->form1 = edc_window_roll(&form1_window, s->form1, s->at[0], s->at[MODE2_EDC_OFFSET]);
            s->form2 =
                edc_window_roll(&form2_window, s->form2, s->at[0], s->at[MODE2_FORM2_EDC_OFFSET]);
        }
        return;
    }
    s->form1 = edc_compute(0, p, MODE2_EDC_OFFSET);
    s->form2 =
        edc_compute(s->form1, p + MODE2_EDC_OFFSET, MODE2_FORM2_EDC_OFFSET - MODE2_EDC_OFFSET);
    s->at = p;
}

/*
 * Sector type detection for a header-less 2336-byte Mode 2 body (subheader onwards), as found
 * in MODE2/2336 images or wherever literal data leaves sectors off the 2352-byte grid. The
 * body needs at least SECTOR_SIZE_MODE2 readable bytes.
 */
static sector_type_t check_type_body(body_edc_t *s, const uint8_t *body) {
    if (!check_subheader_dup(body)) {
        return SECTOR_TYPE_LITERAL;
    }
    body_edc_at(s, body);
    if (edc_check_bytes(s->form1, body + MODE2_EDC_OFFSET)) {
        /* Form 1 ECC covers a zeroed address, so give the body a raw-sector frame */
        uint8_t sector[SECTOR_SIZE_RAW];
        memset(sector, 0, SECTOR_SYNC_HEADER_SIZE);
        memcpy(sector + SECTOR_SYNC_HEADER_SIZE, body, SECTOR_SIZE_MODE2);
        if (ecc_verify(sector, true, sector + OFFSET_MODE1_ECC_P)) {
            return SECTOR_TYPE_MODE2_FORM1;
        }
    }
    if (edc_check_bytes(s->form2, body + MODE2_FORM2_EDC_OFFSET)) {
        return SECTOR_TYPE_MODE2_FORM2;
    }
    return SECTOR_TYPE_LITERAL;
}

/* What the scanner found: literal bytes, then optionally one sector */
typedef struct {
    size_t literal;     /* literal bytes before the sector */
    sector_type_t type; /* record type of the sector, or SECTOR_TYPE_LITERAL for none */
    bool raw;           /* a raw 2352-byte sector rather than a header-less Mode 2 body */
} scan_t;

/*
 * Find the next sector the format can model in buf[0 .. avail), testing every byte offset as
 * the original encoder did: first a raw 2352-byte sector with sync, then a header-less
 * 2336-byte Mode 2 body. Aligned raw images pass the first test at each sector boundary; the
 * second recovers MODE2/2336 images, sectors behind an odd-sized prefix, and layouts with
 * extra per-sector bytes such as 2448-byte raw plus subchannel dumps. Everything before the
 * sector is literal.
 *
 * Unless eof is set, the scan stops at the first offset with less than a raw sector of
 * lookahead, since more input could complete a sector there.
 */
static scan_t scan_next(body_edc_t *s, uint8_t *buf, size_t avail, bool eof) {
    size_t stop = eof ? avail : (avail >= SECTOR_SIZE_RAW ? avail - SECTOR_SIZE_RAW + 1 : 0);

    for (size_t i = 0; i < stop; i++) {
        uint8_t *p = buf + i;
        size_t left = avail - i;
        /* Cheap first-byte filters keep literal data moving; the full checks follow */
        if (left >= SECTOR_SIZE_RAW && p[0] == SYNC_BYTE_START && p[1] == SYNC_BYTE_MIDDLE) {
            sector_type_t type = check_type_raw(p);
            if (type != SECTOR_TYPE_LITERAL) {
                return (scan_t){i, type, true};
            }
        }
        if (left >= SECTOR_SIZE_MODE2 && p[0] == p[4] && p[1] == p[5]) {
            sector_type_t type = check_type_body(s, p);
            if (type != SECTOR_TYPE_LITERAL) {
                return (scan_t){i, type, false};
            }
        }
    }
    return (scan_t){stop, SECTOR_TYPE_LITERAL, false};
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
    bool show; /* only a terminal gets the self-overwriting progress line */
} progress_t;

static void progress_reset(progress_t *p, int64_t total) {
    p->analyze = 0;
    p->encode = 0;
    p->total = total;
    p->show = stream_is_terminal(stderr);
}

static void progress_update(progress_t *p, int64_t analyze, int64_t encode) {
    bool changed = (analyze >> 20) != (p->analyze >> 20) || (encode >> 20) != (p->encode >> 20);
    p->analyze = analyze;
    p->encode = encode;

    if (changed && p->show) {
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
static void print_report(const uint64_t typetally[4], int64_t total_in, FILE *out) {
    fprintf(stderr, "Literal bytes........... %10" PRIu64 "\n", typetally[SECTOR_TYPE_LITERAL]);
    fprintf(stderr, "Mode 1 sectors.......... %10" PRIu64 "\n", typetally[SECTOR_TYPE_MODE1]);
    fprintf(stderr, "Mode 2 form 1 sectors... %10" PRIu64 "\n", typetally[SECTOR_TYPE_MODE2_FORM1]);
    fprintf(stderr, "Mode 2 form 2 sectors... %10" PRIu64 "\n", typetally[SECTOR_TYPE_MODE2_FORM2]);
    /* A pipe has no meaningful position, so its size is only known to the reader */
    off_t outpos = stream_is_regular_file(out) ? ftello(out) : -1;
    if (outpos >= 0) {
        fprintf(stderr, "Encoded %lld bytes -> %lld bytes\n", (long long)total_in,
                (long long)outpos);
    } else {
        fprintf(stderr, "Encoded %lld bytes\n", (long long)total_in);
    }
    fprintf(stderr, "Done.\n");
}

/*
 * Write one sector found by scan_next() as its own record. A raw Mode 2 sector carries its
 * sync and header as a 16-byte literal record first; see queue_raw_sector().
 */
static int write_sector_unit(const uint8_t *sector, scan_t unit, FILE *out, uint32_t *edc,
                             uint64_t typetally[4]) {
    const uint8_t *record = sector;
    if (unit.raw && unit.type != SECTOR_TYPE_MODE1) {
        if (write_literal_record(sector, SECTOR_SYNC_HEADER_SIZE, out, edc) < 0) {
            return -1;
        }
        typetally[SECTOR_TYPE_LITERAL] += SECTOR_SYNC_HEADER_SIZE;
        record = sector + SECTOR_SYNC_HEADER_SIZE;
    }
    if (write_type_count(out, unit.type, 1) < 0 ||
        write_sector_payload(record, out, edc, unit.type) < 0) {
        return -1;
    }
    typetally[unit.type]++;
    return 0;
}

/*
 * Streaming mode encoder - processes input without seeking.
 * Works with stdin/pipes.
 *
 * Trade-offs vs batch mode:
 * - Smaller buffer (256 KB read-ahead vs ~1 MB)
 * - One record per sector (no batching of same-type runs)
 * - Slightly higher output overhead for consecutive same-type sectors
 * - Works with non-seekable streams (stdin, pipes)
 */
static int ecmify_streaming(FILE *in, FILE *out, bool verbose) {
    uint8_t *buf = nullptr;
    size_t start = 0;
    size_t avail = 0;
    bool eof = false;
    body_edc_t scan = {nullptr, 0, 0};
    uint32_t inedc = 0;
    uint64_t typetally[4] = {0};
    int64_t total_in = 0;
    int result = 1;

    ECM_VERBOSE(verbose, "Using streaming mode (stdin/pipe)");
    body_windows_init();

    buf = malloc(STREAM_BUFFER_SIZE);
    if (!buf) {
        fprintf(stderr, "Error: failed to allocate input buffer\n");
        return 1;
    }
    if (write_magic_header(out) < 0) {
        goto done;
    }

    for (;;) {
        /* Keep a raw sector of lookahead while input lasts */
        if (!eof && avail < SECTOR_SIZE_RAW) {
            memmove(buf, buf + start, avail);
            start = 0;
            scan.at = nullptr;
            size_t want = STREAM_BUFFER_SIZE - avail;
            size_t got = fread(buf + avail, 1, want, in);
            /* A short read is only the end of input when the stream carries no error */
            if (got < want) {
                if (ferror(in)) {
                    fprintf(stderr, "Error: failed to read input: %s\n", strerror(errno));
                    goto done;
                }
                eof = true;
            }
            avail += got;
            total_in += (int64_t)got;
        }
        if (avail == 0) {
            break;
        }

        scan_t next = scan_next(&scan, buf + start, avail, eof);
        if (next.literal) {
            ECM_VERBOSE(verbose, "Literal: %zu bytes", next.literal);
            if (write_literal_record(buf + start, next.literal, out, &inedc) < 0) {
                goto writeerr;
            }
            typetally[SECTOR_TYPE_LITERAL] += next.literal;
            start += next.literal;
            avail -= next.literal;
        }
        if (next.type != SECTOR_TYPE_LITERAL) {
            size_t size = next.raw ? SECTOR_SIZE_RAW : SECTOR_SIZE_MODE2;
            ECM_VERBOSE(verbose, "Sector: type=%s, %s", sector_type_names[next.type],
                        next.raw ? "raw" : "header-less body");
            if (write_sector_unit(buf + start, next, out, &inedc, typetally) < 0) {
                goto writeerr;
            }
            start += size;
            avail -= size;
        }
    }

    if (write_type_count(out, 0, 0) < 0) {
        fprintf(stderr, "Error: failed to write end marker\n");
        goto done;
    }
    if (write_edc_checksum(out, inedc) < 0) {
        goto done;
    }

    print_report(typetally, total_in, out);
    result = 0;
    goto done;

writeerr:
    fprintf(stderr, "Error: failed to write output\n");
done:
    free(buf);
    return result;
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
    uint64_t typetally[4];
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
    enc->typetally[run->type] += (uint64_t)run->count;
    if (flush_sector_run(&enc->edc, run->type, (unsigned)run->count, enc->in, enc->out,
                         &enc->progress) < 0) {
        return -1;
    }
    run->count = 0;
    return 0;
}

/*
 * Whether count more units of the given type can join the pending run. A run holds one type,
 * and its count must stay below 2^31: the format declares larger counts invalid, and a
 * decoder that enforces that rejects the whole file.
 */
static bool run_can_extend(const run_t *run, sector_type_t type, int64_t count) {
    return run->count != 0 && run->type == type && run->count + count <= RECORD_COUNT_MAX;
}

/*
 * Append count units of the given type found at input offset start, flushing the pending
 * run first when the type changes or the record would exceed RECORD_COUNT_MAX.
 */
static int run_add(batch_encoder_t *enc, sector_type_t type, int64_t start, int64_t count) {
    run_t *run = &enc->run;

    if (run->count != 0 && !run_can_extend(run, type, count) &&
        run_flush(enc, run->type == type ? "Splitting batch" : "Flushing batch") < 0) {
        return -1;
    }
    if (run->count == 0) {
        run->type = type;
        run->start = start;
    }
    run->count += count;
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
    body_edc_t scan = {nullptr, 0, 0};
    int64_t incheckpos = 0;
    int64_t inbufferpos = 0;
    int64_t intotallength;
    size_t inqueuestart = 0;
    size_t dataavail = 0;
    int result = 0;

    ECM_VERBOSE(verbose, "Using batch mode (seekable file)");
    body_windows_init();

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
        /*
         * Keep a raw sector of lookahead while unread input remains. Comparing the buffered
         * bytes with the unread ones instead skipped the refill when a sector straddled the
         * buffer end with most of it buffered, and that sector was stored as literal bytes.
         */
        if (dataavail < SECTOR_SIZE_RAW && inbufferpos < intotallength) {
            int64_t unread = intotallength - inbufferpos;
            size_t room = (INPUT_QUEUE_SIZE - INPUT_QUEUE_PADDING) - dataavail;
            size_t willread = unread < (int64_t)room ? (size_t)unread : room;
            if (inqueuestart) {
                memmove(inputqueue + INPUT_QUEUE_PADDING,
                        inputqueue + INPUT_QUEUE_PADDING + inqueuestart, dataavail);
                inqueuestart = 0;
            }
            scan.at = nullptr; /* the buffer moved */
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

        bool eof = inbufferpos >= intotallength;
        scan_t next =
            scan_next(&scan, inputqueue + INPUT_QUEUE_PADDING + inqueuestart, dataavail, eof);
        size_t step = next.literal;
        int rc = 0;
        if (next.literal) {
            rc = run_add(&enc, SECTOR_TYPE_LITERAL, incheckpos, (int64_t)next.literal);
        }
        if (rc == 0 && next.type != SECTOR_TYPE_LITERAL) {
            int64_t at = incheckpos + (int64_t)next.literal;
            if (next.raw) {
                rc = queue_raw_sector(&enc, next.type, at);
                step += SECTOR_SIZE_RAW;
            } else {
                rc = run_add(&enc, next.type, at, 1);
                step += SECTOR_SIZE_MODE2;
            }
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

static void usage(FILE *f, const char *prog) {
    fprintf(f, "usage: %s [-v|--verbose] cdimagefile [ecmfile]\n", prog);
    fprintf(f, "       %s -h|--help | -V|--version\n", prog);
    fprintf(f, "       use '-' for stdin/stdout, and '--' before file names starting with '-'\n");
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
    bool verbose = false;
    int result = 0;

    /* Options may appear anywhere; '--' ends them, and a lone '-' is a file name */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!options_done && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "--") == 0) {
                options_done = true;
            } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
                verbose = true;
            } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
                usage(stdout, argv[0]);
                return 0;
            } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
                printf("ecm " ECM_VERSION "\n");
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

    /* Determine output filename */
    if (nfiles == 2) {
        outfilename = files[1];
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

    /* Use streaming mode for stdin, batch mode for regular files */
    if (is_stdio(infilename)) {
        result = ecmify_streaming(fin, fout, verbose);
    } else {
        result = ecmify(fin, fout, verbose);
    }

    /* Buffered data is only on disk once the flush succeeds */
    if (output_finish(fout, is_stdio(outfilename) ? "stdout" : outfilename) != 0 && result == 0) {
        result = 1;
    }
    fout = nullptr;

    /* A truncated archive under the requested name would pass for a finished one */
    if (result != 0 && out_is_file && remove(outfilename) == 0) {
        fprintf(stderr, "Removed incomplete output %s\n", outfilename);
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
