/*
 * eccedc.h - Shared ECC/EDC computation for CD sector encoding/decoding
 *
 * Copyright (C) 2002 Neill Corlett
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ECCEDC_H
#define ECCEDC_H

#include <stdint.h>
#include <stddef.h>

/* Cross-platform strcasecmp compatibility */
#if defined(WIN32) || defined(WIN64) || defined(_WIN32) || defined(_WIN64)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/*
 * CD Sector Constants
 */
#define SECTOR_SIZE_RAW         2352    /* Full raw sector size */
#define SECTOR_SIZE_MODE2       2336    /* Mode 2 sector size (without sync) */
#define SECTOR_USER_DATA        2048    /* User data area size */

/* Sync pattern offsets */
#define OFFSET_SYNC_START       0x000
#define OFFSET_SYNC_END         0x00C
#define OFFSET_HEADER           0x00C   /* MSF address starts here */
#define OFFSET_MODE             0x00F   /* Mode byte */

/* Mode 1 sector offsets */
#define OFFSET_MODE1_DATA       0x010   /* User data start */
#define OFFSET_MODE1_EDC        0x810   /* EDC (4 bytes) */
#define OFFSET_MODE1_RESERVED   0x814   /* Reserved (8 bytes, must be zero) */
#define OFFSET_MODE1_ECC_P      0x81C   /* ECC P code (172 bytes) */
#define OFFSET_MODE1_ECC_Q      0x8C8   /* ECC Q code (104 bytes) */

/* Mode 2 sector offsets (relative to sector + 0x10 for form 1/2) */
#define OFFSET_MODE2_SUBHEADER  0x010   /* Subheader (8 bytes: 4 + 4 copy) */
#define OFFSET_MODE2_DATA       0x018   /* User data start */
#define OFFSET_MODE2_FORM1_EDC  0x818   /* Form 1 EDC */
#define OFFSET_MODE2_FORM1_ECC_P 0x81C  /* Form 1 ECC P */
#define OFFSET_MODE2_FORM1_ECC_Q 0x8C8  /* Form 1 ECC Q */
#define OFFSET_MODE2_FORM2_EDC  0x92C   /* Form 2 EDC */

/* ECC sizes */
#define ECC_P_SIZE              172
#define ECC_Q_SIZE              104
#define EDC_SIZE                4
#define RESERVED_SIZE           8

/* Data sizes for ECM encoding/decoding */
#define MODE1_ADDRESS_SIZE      3       /* MSF address bytes */
#define MODE2_SUBHEADER_SIZE    4       /* Subheader (file number, channel, submode, coding) */
#define MODE2_FORM1_DATA_SIZE   0x804   /* Subheader + user data (4 + 2048) */
#define MODE2_FORM2_DATA_SIZE   0x918   /* Subheader + user data (4 + 2324) */
#define MODE2_EDC_OFFSET        0x808   /* EDC offset in Mode 2 Form 1 sector */
#define MODE2_FORM2_EDC_OFFSET  0x91C   /* EDC offset in Mode 2 Form 2 sector */

/* Type/count encoding limits */
#define TYPE_COUNT_MAX_BITS     32      /* Maximum bits for type/count decoding */

/* Sync pattern bytes */
#define SYNC_BYTE_START         0x00
#define SYNC_BYTE_MIDDLE        0xFF
#define SYNC_BYTE_END           0x00

/*
 * Sector types for ECM encoding
 * 00 - literal bytes (no compression)
 * 01 - 2352 mode 1: predict sync, mode, reserved, edc, ecc
 * 02 - 2336 mode 2 form 1: predict redundant flags, edc, ecc
 * 03 - 2336 mode 2 form 2: predict redundant flags, edc
 */
#define SECTOR_TYPE_LITERAL     0
#define SECTOR_TYPE_MODE1       1
#define SECTOR_TYPE_MODE2_FORM1 2
#define SECTOR_TYPE_MODE2_FORM2 3

/*
 * ECM file format constants
 */
#define ECM_MAGIC_E             'E'
#define ECM_MAGIC_C             'C'
#define ECM_MAGIC_M             'M'
#define ECM_MAGIC_NULL          0x00

/*
 * Initialize ECC/EDC lookup tables.
 * Must be called before any other eccedc functions.
 */
void eccedc_init(void);

/*
 * Compute EDC (Error Detection Code) for a block of data.
 *
 * @param edc   Initial EDC value (use 0 for first block, or previous result for chaining)
 * @param src   Source data buffer
 * @param size  Number of bytes to process
 * @return      Computed EDC value
 */
uint32_t edc_compute(uint32_t edc, const uint8_t *src, size_t size);

/*
 * Compute EDC and write it to destination buffer in little-endian format.
 *
 * @param src   Source data buffer
 * @param size  Number of bytes to process
 * @param dest  Destination buffer (4 bytes)
 */
void edc_compute_block(const uint8_t *src, size_t size, uint8_t *dest);

/*
 * Generate ECC P and Q codes for a sector.
 * Used by decoder (unecm) to reconstruct ECC data.
 *
 * @param sector       Sector buffer (at least 2352 bytes)
 * @param zeroaddress  If non-zero, temporarily zero the address field during computation
 */
void ecc_generate(uint8_t *sector, int zeroaddress);

/*
 * Verify ECC P and Q codes for a sector.
 * Used by encoder (ecm) to validate sector before stripping ECC.
 *
 * @param sector       Sector buffer
 * @param zeroaddress  If non-zero, temporarily zero the address field during computation
 * @param dest         ECC destination to verify against
 * @return             1 if ECC matches, 0 otherwise
 */
int ecc_verify(uint8_t *sector, int zeroaddress, uint8_t *dest);

/*
 * Generate complete ECC/EDC data for a sector.
 *
 * @param sector  Sector buffer (2352 bytes)
 * @param type    Sector type (SECTOR_TYPE_MODE1, SECTOR_TYPE_MODE2_FORM1, SECTOR_TYPE_MODE2_FORM2)
 */
void eccedc_generate(uint8_t *sector, int type);

#endif /* ECCEDC_H */
