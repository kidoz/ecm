#ifndef ECCEDC_H
#define ECCEDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Debug logging macros
 *
 * ECM_DEBUG_LOG: Compile-time debug logging (disabled by default)
 *   - Enable by defining ECM_DEBUG before including this header
 *   - Outputs to stderr with [DEBUG] prefix
 *
 * ECM_VERBOSE: Runtime verbose logging
 *   - Controlled by verbose flag passed at runtime
 *   - Outputs to stderr
 */
#ifdef ECM_DEBUG
#define ECM_DEBUG_LOG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define ECM_DEBUG_LOG(fmt, ...) ((void)0)
#endif

#define ECM_VERBOSE(verbose, fmt, ...)                \
    do {                                              \
        if (verbose)                                  \
            fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
    } while (0)

/* Cross-platform strcasecmp compatibility */
#if defined(WIN32) || defined(WIN64) || defined(_WIN32) || defined(_WIN64)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/*
 * CD Sector Constants (C23 constexpr where possible)
 */
enum {
    SECTOR_SIZE_RAW = 2352,       /* Full raw sector size */
    SECTOR_SIZE_MODE2 = 2336,     /* Mode 2 sector size (without sync) */
    SECTOR_USER_DATA = 2048,      /* User data area size */
    SECTOR_SYNC_HEADER_SIZE = 16, /* Sync (12) + MSF address (3) + mode (1); the part of a raw
                                     Mode 2 sector that an ECM type 2/3 record does not carry */
};

/* Sync pattern offsets */
enum {
    OFFSET_HEADER = 0x00C, /* MSF address starts here */
    OFFSET_MODE = 0x00F,   /* Mode byte */
};

/* Mode 1 sector offsets */
enum {
    OFFSET_MODE1_DATA = 0x010,     /* User data start */
    OFFSET_MODE1_EDC = 0x810,      /* EDC (4 bytes) */
    OFFSET_MODE1_RESERVED = 0x814, /* Reserved (8 bytes, must be zero) */
    OFFSET_MODE1_ECC_P = 0x81C,    /* ECC P code (172 bytes) */
    OFFSET_MODE1_ECC_Q = 0x8C8,    /* ECC Q code (104 bytes) */
};

/* Mode 2 sector offsets */
enum {
    OFFSET_MODE2_SUBHEADER = 0x010, /* Subheader (8 bytes: 4 + 4 copy) */
    OFFSET_MODE2_FORM1_EDC = 0x818, /* Form 1 EDC */
    OFFSET_MODE2_FORM2_EDC = 0x92C, /* Form 2 EDC */
};

/* ECC sizes */
enum {
    ECC_P_SIZE = 172,
    ECC_Q_SIZE = 104,
    EDC_SIZE = 4,
    RESERVED_SIZE = 8,
};

/* Data sizes for ECM encoding/decoding */
enum {
    MODE1_ADDRESS_SIZE = 3,        /* MSF address bytes */
    MODE2_SUBHEADER_SIZE = 4,      /* Subheader (file number, channel, submode, coding) */
    MODE2_FORM1_DATA_SIZE = 0x804, /* Subheader + user data (4 + 2048) */
    MODE2_FORM2_DATA_SIZE = 0x918, /* Subheader + user data (4 + 2324) */
    MODE2_EDC_OFFSET = 0x808,      /* EDC offset in Mode 2 Form 1 sector */
    MODE2_FORM2_EDC_OFFSET = 0x91C /* EDC offset in Mode 2 Form 2 sector */
};

/* Type/count encoding limits */
enum {
    TYPE_COUNT_MAX_BITS = 32
}; /* Maximum bits for type/count decoding */

/* Sync pattern bytes */
enum {
    SYNC_BYTE_START = 0x00,
    SYNC_BYTE_MIDDLE = 0xFF,
    SYNC_BYTE_END = 0x00,
};

/*
 * Sector types for ECM encoding
 * 00 - literal bytes (no compression)
 * 01 - 2352 mode 1: predict sync, mode, reserved, edc, ecc
 * 02 - 2336 mode 2 form 1: predict redundant flags, edc, ecc
 * 03 - 2336 mode 2 form 2: predict redundant flags, edc
 */
typedef enum {
    SECTOR_TYPE_LITERAL = 0,
    SECTOR_TYPE_MODE1 = 1,
    SECTOR_TYPE_MODE2_FORM1 = 2,
    SECTOR_TYPE_MODE2_FORM2 = 3
} sector_type_t;

/*
 * ECM file format constants
 */
enum {
    ECM_MAGIC_E = 'E',
    ECM_MAGIC_C = 'C',
    ECM_MAGIC_M = 'M',
    ECM_MAGIC_NULL = 0x00,
};

/* ECC computation constants */
enum {
    ECC_P_MAJOR = 86,
    ECC_P_MINOR = 24,
    ECC_P_MULT = 2,
    ECC_P_INC = 86,
    ECC_Q_MAJOR = 52,
    ECC_Q_MINOR = 43,
    ECC_Q_MULT = 86,
    ECC_Q_INC = 88,
    ECC_DATA_OFFSET = 0x0C,
    ADDRESS_FIELD_OFFSET = 12,
    ADDRESS_FIELD_SIZE = 4,
};

/*
 * Initialize ECC/EDC lookup tables.
 * Thread-safe: can be called multiple times from multiple threads.
 * Must be called before any other eccedc functions.
 */
void eccedc_init(void);

/*
 * Compute EDC (Error Detection Code) for a block of data.
 *
 * @param edc   Initial EDC value (use 0 for first block, or previous result for chaining)
 * @param src   Source data buffer (must not be null)
 * @param size  Number of bytes to process
 * @return      Computed EDC value
 */
[[nodiscard]] uint32_t edc_compute(uint32_t edc, const uint8_t *src, size_t size);

/*
 * Sliding-window EDC: the EDC of a fixed-length window, moved forward one byte at a time.
 * The EDC is linear and starts from zero, so dropping the first byte of a window only needs
 * that byte's contribution across the window length, which leaving[] tabulates.
 */
typedef struct {
    uint32_t leaving[256]; /* EDC of a byte followed by the window length in zero bytes */
} edc_window_t;

/*
 * Prepare the table for windows of the given length. Calls eccedc_init() itself.
 *
 * @param w       Window table to fill (must not be null)
 * @param length  Window length in bytes
 */
void edc_window_init(edc_window_t *w, size_t length);

/*
 * Move a window forward by one byte.
 *
 * @param w         Table prepared for the window length
 * @param edc       EDC of the current window
 * @param leaving   First byte of the current window
 * @param entering  Byte just past the current window
 * @return          EDC of the window one byte further on
 */
[[nodiscard]] uint32_t edc_window_roll(const edc_window_t *w, uint32_t edc, uint8_t leaving,
                                       uint8_t entering);

/*
 * Compute EDC and write it to destination buffer in little-endian format.
 *
 * @param src   Source data buffer (must not be null)
 * @param size  Number of bytes to process
 * @param dest  Destination buffer (4 bytes, must not be null)
 */
void edc_compute_block(const uint8_t *src, size_t size, uint8_t *dest);

/*
 * Generate ECC P and Q codes for a sector.
 * Used by decoder (unecm) to reconstruct ECC data.
 *
 * @param sector       Sector buffer (at least 2352 bytes, must not be null)
 * @param zeroaddress  If true, temporarily zero the address field during computation
 */
void ecc_generate(uint8_t *sector, bool zeroaddress);

/*
 * Verify ECC P and Q codes for a sector.
 * Used by encoder (ecm) to validate sector before stripping ECC.
 *
 * @param sector       Sector buffer (must not be null)
 * @param zeroaddress  If true, temporarily zero the address field during computation
 * @param dest         ECC destination to verify against (must not be null)
 * @return             true if ECC matches, false otherwise
 */
[[nodiscard]] bool ecc_verify(uint8_t *sector, bool zeroaddress, uint8_t *dest);

/*
 * Generate complete ECC/EDC data for a sector.
 *
 * @param sector  Sector buffer (2352 bytes, must not be null)
 * @param type    Sector type (SECTOR_TYPE_MODE1, SECTOR_TYPE_MODE2_FORM1, SECTOR_TYPE_MODE2_FORM2)
 */
void eccedc_generate(uint8_t *sector, sector_type_t type);

/*
 * Write EDC as 4 little-endian bytes to buffer.
 *
 * @param edc   EDC value to write
 * @param dest  Destination buffer (4 bytes, must not be null)
 */
void edc_write_bytes(uint32_t edc, uint8_t *dest);

/*
 * Check if EDC matches at buffer location.
 *
 * @param edc  Expected EDC value
 * @param src  Source buffer (4 bytes, must not be null)
 * @return     true if EDC matches, false otherwise
 */
[[nodiscard]] bool edc_check_bytes(uint32_t edc, const uint8_t *src);

/*
 * Initialize sync pattern in sector buffer.
 * Writes the 12-byte sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00
 *
 * @param sector  Sector buffer (at least 12 bytes, must not be null)
 */
void sector_init_sync(uint8_t *sector);

/*
 * Copy subheader bytes in Mode 2 sector.
 * Copies bytes at 0x14-0x17 to 0x10-0x13.
 *
 * @param sector  Sector buffer (must not be null)
 */
void sector_copy_subheader(uint8_t *sector);

/*
 * Check whether an open stream and a path name the same underlying file.
 * Follows symlinks and recognises hard links, so it is safe to call before truncating an
 * output path that might alias the input.
 *
 * @param f     Open stream (must not be null)
 * @param path  Path to compare against (need not exist)
 * @return      true only when both resolve to the same file; false on any error
 */
[[nodiscard]] bool file_is_same_as_path(FILE *f, const char *path);

/*
 * Return the final component of a path, as a pointer into path.
 * On Windows both slashes and a drive prefix ("C:image.bin") separate components, and
 * double-byte ANSI code pages are respected so a trail byte equal to '\' is not taken for a
 * separator; elsewhere only '/' separates components.
 *
 * @param path  Path to split (must not be null)
 * @return      Pointer to the character after the last separator, or path if there is none
 */
[[nodiscard]] const char *path_basename(const char *path);

/*
 * Put a stream into binary mode so bytes pass through untranslated.
 * Needed for stdin/stdout on Windows, where the CRT defaults to text mode; a no-op elsewhere.
 *
 * @param f  Open stream (must not be null)
 * @return   true on success, false if the mode could not be changed
 */
[[nodiscard]] bool stream_set_binary(FILE *f);

/*
 * Check whether a stream is attached to a terminal, so progress lines that rewrite themselves
 * with '\r' are shown interactively but kept out of redirected logs.
 *
 * @param f  Open stream (must not be null)
 * @return   true only for a terminal
 */
[[nodiscard]] bool stream_is_terminal(FILE *f);

/*
 * Check whether a stream is a regular disk file, as opposed to a pipe, terminal, or device.
 * Positions reported by ftello() are only meaningful for regular files, and only a regular
 * file should be deleted when a run fails.
 *
 * @param f  Open stream (must not be null)
 * @return   true only for a regular file
 */
[[nodiscard]] bool stream_is_regular_file(FILE *f);

/*
 * Flush buffered output and report write failures that per-write checks cannot see.
 * Closes the stream unless it is stdout, which is left open for the process.
 *
 * @param out   Output stream (must not be null)
 * @param name  Name used in the error message
 * @return      0 on success, -1 if the flush or close failed (message already printed)
 */
[[nodiscard]] int output_finish(FILE *out, const char *name);

#endif /* ECCEDC_H */
