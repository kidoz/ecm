/*
 * eccedc.c - Shared ECC/EDC computation for CD sector encoding/decoding
 *
 * Copyright (C) 2002 Neill Corlett
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Portability notes:
 * - Uses fixed-width integer types from <stdint.h>
 * - No assumptions about byte order
 * - No assumptions about struct packing
 * - No unaligned memory access
 */

#include "eccedc.h"
#include <string.h>

/* Lookup tables for ECC/EDC computation */
static uint8_t ecc_f_lut[256];
static uint8_t ecc_b_lut[256];
static uint32_t edc_lut[256];

/* Initialization flag */
static int tables_initialized = 0;

void eccedc_init(void) {
    if (tables_initialized) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        /* ECC F/B lookup tables using polynomial 0x11D */
        uint32_t j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        ecc_f_lut[i] = (uint8_t)j;
        ecc_b_lut[i ^ j] = (uint8_t)i;

        /* EDC lookup table using polynomial 0xD8018001 */
        uint32_t edc = i;
        for (j = 0; j < 8; j++) {
            edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
        }
        edc_lut[i] = edc;
    }

    tables_initialized = 1;
}

uint32_t edc_compute(uint32_t edc, const uint8_t *src, size_t size) {
    while (size--) {
        edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
    }
    return edc;
}

void edc_compute_block(const uint8_t *src, size_t size, uint8_t *dest) {
    uint32_t edc = edc_compute(0, src, size);
    dest[0] = (uint8_t)((edc >> 0) & 0xFF);
    dest[1] = (uint8_t)((edc >> 8) & 0xFF);
    dest[2] = (uint8_t)((edc >> 16) & 0xFF);
    dest[3] = (uint8_t)((edc >> 24) & 0xFF);
}

/*
 * Internal: Compute ECC for a block (can do either P or Q)
 */
static void ecc_compute_block(
    uint8_t *src,
    uint32_t major_count,
    uint32_t minor_count,
    uint32_t major_mult,
    uint32_t minor_inc,
    uint8_t *dest
) {
    uint32_t size = major_count * minor_count;

    for (uint32_t major = 0; major < major_count; major++) {
        uint32_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t ecc_a = 0;
        uint8_t ecc_b = 0;

        for (uint32_t minor = 0; minor < minor_count; minor++) {
            uint8_t temp = src[index];
            index += minor_inc;
            if (index >= size) {
                index -= size;
            }
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ecc_f_lut[ecc_a];
        }

        ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
        dest[major] = ecc_a;
        dest[major + major_count] = ecc_a ^ ecc_b;
    }
}

/*
 * Internal: Verify ECC for a block (can do either P or Q)
 * Returns 1 if ECC matches, 0 otherwise
 */
static int ecc_verify_block(
    uint8_t *src,
    uint32_t major_count,
    uint32_t minor_count,
    uint32_t major_mult,
    uint32_t minor_inc,
    uint8_t *dest
) {
    uint32_t size = major_count * minor_count;

    for (uint32_t major = 0; major < major_count; major++) {
        uint32_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t ecc_a = 0;
        uint8_t ecc_b = 0;

        for (uint32_t minor = 0; minor < minor_count; minor++) {
            uint8_t temp = src[index];
            index += minor_inc;
            if (index >= size) {
                index -= size;
            }
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ecc_f_lut[ecc_a];
        }

        ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
        if (dest[major] != ecc_a) {
            return 0;
        }
        if (dest[major + major_count] != (ecc_a ^ ecc_b)) {
            return 0;
        }
    }
    return 1;
}

void ecc_generate(uint8_t *sector, int zeroaddress) {
    uint8_t address[4];

    /* Save the address and zero it out if requested */
    if (zeroaddress) {
        for (int i = 0; i < 4; i++) {
            address[i] = sector[12 + i];
            sector[12 + i] = 0;
        }
    }

    /* Compute ECC P code */
    ecc_compute_block(
        sector + 0xC,   /* src */
        86,             /* major_count */
        24,             /* minor_count */
        2,              /* major_mult */
        86,             /* minor_inc */
        sector + OFFSET_MODE1_ECC_P
    );

    /* Compute ECC Q code */
    ecc_compute_block(
        sector + 0xC,   /* src */
        52,             /* major_count */
        43,             /* minor_count */
        86,             /* major_mult */
        88,             /* minor_inc */
        sector + OFFSET_MODE1_ECC_Q
    );

    /* Restore the address */
    if (zeroaddress) {
        for (int i = 0; i < 4; i++) {
            sector[12 + i] = address[i];
        }
    }
}

int ecc_verify(uint8_t *sector, int zeroaddress, uint8_t *dest) {
    uint8_t address[4];
    int result;

    /* Save the address and zero it out if requested */
    if (zeroaddress) {
        for (int i = 0; i < 4; i++) {
            address[i] = sector[12 + i];
            sector[12 + i] = 0;
        }
    }

    /* Verify ECC P code */
    if (!ecc_verify_block(
        sector + 0xC,
        86, 24, 2, 86,
        dest + (OFFSET_MODE1_ECC_P - OFFSET_MODE1_ECC_P)
    )) {
        if (zeroaddress) {
            for (int i = 0; i < 4; i++) {
                sector[12 + i] = address[i];
            }
        }
        return 0;
    }

    /* Verify ECC Q code */
    result = ecc_verify_block(
        sector + 0xC,
        52, 43, 86, 88,
        dest + (OFFSET_MODE1_ECC_Q - OFFSET_MODE1_ECC_P)
    );

    /* Restore the address */
    if (zeroaddress) {
        for (int i = 0; i < 4; i++) {
            sector[12 + i] = address[i];
        }
    }

    return result;
}

void eccedc_generate(uint8_t *sector, int type) {
    switch (type) {
        case SECTOR_TYPE_MODE1:
            /* Compute EDC over bytes 0x000-0x80F */
            edc_compute_block(sector + 0x00, 0x810, sector + OFFSET_MODE1_EDC);
            /* Write zero bytes to reserved area */
            memset(sector + OFFSET_MODE1_RESERVED, 0, RESERVED_SIZE);
            /* Generate ECC P/Q codes */
            ecc_generate(sector, 0);
            break;

        case SECTOR_TYPE_MODE2_FORM1:
            /* Compute EDC over bytes 0x010-0x817 */
            edc_compute_block(sector + 0x10, 0x808, sector + OFFSET_MODE2_FORM1_EDC);
            /* Generate ECC P/Q codes (with address zeroing) */
            ecc_generate(sector, 1);
            break;

        case SECTOR_TYPE_MODE2_FORM2:
            /* Compute EDC over bytes 0x010-0x92B */
            edc_compute_block(sector + 0x10, 0x91C, sector + OFFSET_MODE2_FORM2_EDC);
            break;
    }
}
