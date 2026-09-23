#ifndef MILENA_FILE_POSITION_H
#define MILENA_FILE_POSITION_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#endif

/* stdio's long-based ftell/fseek can truncate or fail above 2 GiB on Windows.
 * Keep offsets in a signed 64-bit domain and reject unrepresentable casts. */
static inline int64_t milena_file_tell64(FILE *file) {
#ifdef _WIN32
    __int64 position = _ftelli64(file);
    if (position < 0) return -1;
    return (int64_t)position;
#else
    off_t position = ftello(file);
    if (position < 0) return -1;
    if ((uintmax_t)position > (uintmax_t)INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int64_t)position;
#endif
}

static inline int milena_file_seek64(FILE *file, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, origin);
#else
    off_t position = (off_t)offset;
    if ((int64_t)position != offset) {
        errno = EOVERFLOW;
        return -1;
    }
    return fseeko(file, position, origin);
#endif
}

static inline bool milena_file_position_size(FILE *file, size_t *value) {
    if (!value) return false;
    int64_t position = milena_file_tell64(file);
    if (position < 0 || (uint64_t)position > SIZE_MAX) {
        if (position >= 0) errno = EOVERFLOW;
        return false;
    }
    *value = (size_t)position;
    return true;
}

#endif
