#include "eccedc.h"
#include <string.h>

/* Thread-safe initialization: use C11 threads where available, POSIX otherwise */
#if defined(__STDC_NO_THREADS__) || (defined(__APPLE__) && defined(__MACH__))
#include <pthread.h>
static pthread_once_t init_flag = PTHREAD_ONCE_INIT;
#define call_once(flag, func) pthread_once(flag, func)
#else
#include <threads.h>
static once_flag init_flag = ONCE_FLAG_INIT;
#endif

/* Lookup tables for ECC/EDC computation */
static uint8_t ecc_f_lut[256];
static uint8_t ecc_b_lut[256];
static uint32_t edc_lut[256];

static void eccedc_init_tables(void) {
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
}

void eccedc_init(void) {
    call_once(&init_flag, eccedc_init_tables);
}

[[nodiscard]] uint32_t edc_compute(uint32_t edc, const uint8_t *src, size_t size) {
    if (src == nullptr) {
        return edc;
    }
    while (size--) {
        edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
    }
    return edc;
}

void edc_compute_block(const uint8_t *src, size_t size, uint8_t *dest) {
    if (src == nullptr || dest == nullptr) {
        return;
    }
    uint32_t edc = edc_compute(0, src, size);
    dest[0] = edc & 0xFF;
    dest[1] = (edc >> 8) & 0xFF;
    dest[2] = (edc >> 16) & 0xFF;
    dest[3] = (edc >> 24) & 0xFF;
}

/*
 * Internal: Compute ECC for a block (can do either P or Q)
 */
static void ecc_compute_block(uint8_t *src, uint32_t major_count, uint32_t minor_count,
                              uint32_t major_mult, uint32_t minor_inc, uint8_t *dest) {
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
 */
[[nodiscard]] static bool ecc_verify_block(uint8_t *src, uint32_t major_count, uint32_t minor_count,
                                           uint32_t major_mult, uint32_t minor_inc, uint8_t *dest) {
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
            return false;
        }
        if (dest[major + major_count] != (ecc_a ^ ecc_b)) {
            return false;
        }
    }
    return true;
}

/*
 * Save address field and optionally zero it
 */
static void save_address(uint8_t *sector, uint8_t *address, bool zero) {
    for (int i = 0; i < ADDRESS_FIELD_SIZE; i++) {
        address[i] = sector[ADDRESS_FIELD_OFFSET + i];
        if (zero) {
            sector[ADDRESS_FIELD_OFFSET + i] = 0;
        }
    }
}

/*
 * Restore address field
 */
static void restore_address(uint8_t *sector, const uint8_t *address) {
    for (int i = 0; i < ADDRESS_FIELD_SIZE; i++) {
        sector[ADDRESS_FIELD_OFFSET + i] = address[i];
    }
}

void ecc_generate(uint8_t *sector, bool zeroaddress) {
    if (sector == nullptr) {
        return;
    }

    uint8_t address[ADDRESS_FIELD_SIZE];

    if (zeroaddress) {
        save_address(sector, address, true);
    }

    /* Compute ECC P code */
    ecc_compute_block(sector + ECC_DATA_OFFSET, ECC_P_MAJOR, ECC_P_MINOR, ECC_P_MULT, ECC_P_INC,
                      sector + OFFSET_MODE1_ECC_P);

    /* Compute ECC Q code */
    ecc_compute_block(sector + ECC_DATA_OFFSET, ECC_Q_MAJOR, ECC_Q_MINOR, ECC_Q_MULT, ECC_Q_INC,
                      sector + OFFSET_MODE1_ECC_Q);

    if (zeroaddress) {
        restore_address(sector, address);
    }
}

[[nodiscard]] bool ecc_verify(uint8_t *sector, bool zeroaddress, uint8_t *dest) {
    if (sector == nullptr || dest == nullptr) {
        return false;
    }

    uint8_t address[ADDRESS_FIELD_SIZE];

    if (zeroaddress) {
        save_address(sector, address, true);
    }

    /* Verify ECC P code */
    if (!ecc_verify_block(sector + ECC_DATA_OFFSET, ECC_P_MAJOR, ECC_P_MINOR, ECC_P_MULT, ECC_P_INC,
                          dest)) {
        if (zeroaddress) {
            restore_address(sector, address);
        }
        return false;
    }

    /* Verify ECC Q code */
    bool result = ecc_verify_block(sector + ECC_DATA_OFFSET, ECC_Q_MAJOR, ECC_Q_MINOR, ECC_Q_MULT,
                                   ECC_Q_INC, dest + (OFFSET_MODE1_ECC_Q - OFFSET_MODE1_ECC_P));

    if (zeroaddress) {
        restore_address(sector, address);
    }

    return result;
}

void eccedc_generate(uint8_t *sector, sector_type_t type) {
    if (sector == nullptr) {
        return;
    }

    switch (type) {
        case SECTOR_TYPE_MODE1:
            /* Compute EDC over bytes 0x000-0x80F */
            edc_compute_block(sector, OFFSET_MODE1_EDC, sector + OFFSET_MODE1_EDC);
            /* Write zero bytes to reserved area */
            memset(sector + OFFSET_MODE1_RESERVED, 0, RESERVED_SIZE);
            /* Generate ECC P/Q codes */
            ecc_generate(sector, false);
            break;

        case SECTOR_TYPE_MODE2_FORM1:
            /* Compute EDC over bytes 0x010-0x817 */
            edc_compute_block(sector + OFFSET_MODE2_SUBHEADER, MODE2_EDC_OFFSET,
                              sector + OFFSET_MODE2_FORM1_EDC);
            /* Generate ECC P/Q codes (with address zeroing) */
            ecc_generate(sector, true);
            break;

        case SECTOR_TYPE_MODE2_FORM2:
            /* Compute EDC over bytes 0x010-0x92B */
            edc_compute_block(sector + OFFSET_MODE2_SUBHEADER, MODE2_FORM2_EDC_OFFSET,
                              sector + OFFSET_MODE2_FORM2_EDC);
            break;

        default:
            /* Invalid type - no operation */
            break;
    }
}

void edc_write_bytes(uint32_t edc, uint8_t *dest) {
    if (dest == nullptr) {
        return;
    }
    dest[0] = edc & 0xFF;
    dest[1] = (edc >> 8) & 0xFF;
    dest[2] = (edc >> 16) & 0xFF;
    dest[3] = (edc >> 24) & 0xFF;
}

[[nodiscard]] bool edc_check_bytes(uint32_t edc, const uint8_t *src) {
    if (src == nullptr) {
        return false;
    }
    return src[0] == (edc & 0xFF) && src[1] == ((edc >> 8) & 0xFF) &&
           src[2] == ((edc >> 16) & 0xFF) && src[3] == ((edc >> 24) & 0xFF);
}

void sector_init_sync(uint8_t *sector) {
    if (sector == nullptr) {
        return;
    }
    sector[0] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++) {
        sector[i] = SYNC_BYTE_MIDDLE;
    }
    sector[11] = SYNC_BYTE_END;
}

void sector_copy_subheader(uint8_t *sector) {
    if (sector == nullptr) {
        return;
    }
    sector[OFFSET_MODE2_SUBHEADER + 0] = sector[OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE + 0];
    sector[OFFSET_MODE2_SUBHEADER + 1] = sector[OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE + 1];
    sector[OFFSET_MODE2_SUBHEADER + 2] = sector[OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE + 2];
    sector[OFFSET_MODE2_SUBHEADER + 3] = sector[OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE + 3];
}
