#ifndef MILENA_SHA256_H
#define MILENA_SHA256_H

#include <stdint.h>

/* Compute a file's SHA-256 digest and byte length. Returns 0 on success. */
int milena_sha256_file(const char *path, char digest_hex[65], uint64_t *size_out);

#endif
