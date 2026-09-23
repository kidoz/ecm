#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "eccedc.h"
#include <errno.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/*
 * Thread-safe initialization: use C11 threads where the toolchain ships <threads.h>, POSIX
 * otherwise. MinGW-w64 GCC does not define __STDC_NO_THREADS__ yet has no <threads.h>, so
 * the header itself is probed rather than trusting the macro.
 */
#if defined(__has_include)
#if __has_include(<threads.h>)
#define ECM_HAVE_THREADS_H 1
#endif
#endif
#if defined(__STDC_NO_THREADS__) || (defined(__APPLE__) && defined(__MACH__)) || \
    !defined(ECM_HAVE_THREADS_H)
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

void edc_window_init(edc_window_t *w, size_t length) {
    static const uint8_t zeros[256] = {0};

    if (w == nullptr) {
        return;
    }
    eccedc_init();
    for (uint32_t x = 0; x < 256; x++) {
        uint8_t byte = (uint8_t)x;
        uint32_t edc = edc_compute(0, &byte, 1);
        for (size_t left = length; left > 0;) {
            size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
            edc = edc_compute(edc, zeros, n);
            left -= n;
        }
        w->leaving[x] = edc;
    }
}

[[nodiscard]] uint32_t edc_window_roll(const edc_window_t *w, uint32_t edc, uint8_t leaving,
                                       uint8_t entering) {
    /* Extend the window by the entering byte, then cancel the leaving byte's contribution */
    edc = (edc >> 8) ^ edc_lut[(edc ^ entering) & 0xFF];
    return edc ^ w->leaving[leaving];
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

#if defined(_WIN32) || defined(_WIN64)
[[nodiscard]] bool file_is_same_as_path(FILE *f, const char *path) {
    if (f == nullptr || path == nullptr) {
        return false;
    }
    HANDLE hf = (HANDLE)_get_osfhandle(_fileno(f));
    if (hf == INVALID_HANDLE_VALUE) {
        return false;
    }
    /* Zero desired access is enough to query identity and never conflicts with a
     * stream that is already open on the same file */
    HANDLE hp = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hp == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION a, b;
    bool same = GetFileInformationByHandle(hf, &a) && GetFileInformationByHandle(hp, &b) &&
                a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
                a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
    CloseHandle(hp);
    return same;
}
#else
[[nodiscard]] bool file_is_same_as_path(FILE *f, const char *path) {
    if (f == nullptr || path == nullptr) {
        return false;
    }
    struct stat a, b;
    if (fstat(fileno(f), &a) != 0 || stat(path, &b) != 0) {
        return false;
    }
    /* Device + inode identifies the file itself, so symlinks and hard links both match */
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
#endif

[[nodiscard]] const char *path_basename(const char *path) {
    if (path == nullptr) {
        return nullptr;
    }
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
#if defined(_WIN32) || defined(_WIN64)
        /* Paths arrive in the ANSI code page; in a double-byte one such as 932 the trail
         * byte of a character can be 0x5C, which must not be read as a backslash */
        if (IsDBCSLeadByte((BYTE)*p) && p[1] != '\0') {
            p++;
            continue;
        }
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
#else
        if (*p == '/') {
            base = p + 1;
        }
#endif
    }
    return base;
}

[[nodiscard]] bool stream_set_binary(FILE *f) {
    if (f == nullptr) {
        return false;
    }
#if defined(_WIN32) || defined(_WIN64)
    /* The CRT opens the standard streams in text mode: reads stop at 0x1A and every 0x0A
     * written becomes 0x0D 0x0A, which corrupts a sector stream in both directions */
    return _setmode(_fileno(f), _O_BINARY) != -1;
#else
    return true;
#endif
}

[[nodiscard]] bool stream_is_terminal(FILE *f) {
    if (f == nullptr) {
        return false;
    }
#if defined(_WIN32) || defined(_WIN64)
    return _isatty(_fileno(f)) != 0;
#else
    return isatty(fileno(f)) != 0;
#endif
}

[[nodiscard]] bool stream_is_regular_file(FILE *f) {
    if (f == nullptr) {
        return false;
    }
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    return h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_DISK;
#else
    struct stat st;
    return fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

[[nodiscard]] int output_finish(FILE *out, const char *name) {
    if (out == nullptr) {
        return 0;
    }
    /* Capture the sticky error flag before fclose() releases the stream */
    bool had_error = ferror(out) != 0;
    int rc = (out == stdout) ? fflush(out) : fclose(out);
    if (rc != 0) {
        fprintf(stderr, "Error: failed to write %s: %s\n", name, strerror(errno));
        return -1;
    }
    if (had_error) {
        fprintf(stderr, "Error: output %s is incomplete\n", name);
        return -1;
    }
    return 0;
}
