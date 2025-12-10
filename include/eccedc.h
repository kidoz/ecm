#ifndef ECCEDC_H
#define ECCEDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    SECTOR_SIZE_RAW = 2352,   /* Full raw sector size */
    SECTOR_SIZE_MODE2 = 2336, /* Mode 2 sector size (without sync) */
    SECTOR_USER_DATA = 2048,  /* User data area size */
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

#endif /* ECCEDC_H */
