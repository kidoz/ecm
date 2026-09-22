#ifndef TEST_TMPFILE_H
#define TEST_TMPFILE_H

/*
 * Portable scratch stream for tests and benchmarks.
 *
 * tmpfile() on the Windows C runtimes creates the file in the root of the current drive,
 * which usually needs administrator rights and fails silently otherwise. Create the file in
 * the temp directory instead and let the CRT delete it when the stream is closed.
 */

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

static inline FILE *test_tmpfile(void) {
#if defined(_WIN32) || defined(_WIN64)
    char *path = _tempnam(nullptr, "ecm");
    if (path == nullptr) {
        return nullptr;
    }
    int fd =
        _open(path, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY | _O_TEMPORARY, _S_IREAD | _S_IWRITE);
    free(path);
    if (fd < 0) {
        return nullptr;
    }
    FILE *f = _fdopen(fd, "w+b");
    if (f == nullptr) {
        _close(fd);
    }
    return f;
#else
    return tmpfile();
#endif
}

#endif /* TEST_TMPFILE_H */
