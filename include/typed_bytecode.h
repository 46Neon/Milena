#ifndef MILENA_TYPED_BYTECODE_H
#define MILENA_TYPED_BYTECODE_H

#include "typed_ir.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MILENA_BYTECODE_VERSION_MAJOR 1u
#define MILENA_BYTECODE_VERSION_MINOR 1u
#define MILENA_BYTECODE_MAX_SIZE ((size_t)64u * 1024u * 1024u)

/* Portable, little-endian serialization of a validated canonical typed-IR
 * module. Version 1.1 stores instruction source spans; the reader accepts v1.0
 * modules with empty spans for compatibility. The wire format does not serialize
 * C structs, pointers, padding, or host-sized integers. The reader rejects
 * truncated, oversized, unsupported-version, malformed, and invalid data. */
bool milena_bytecode_encode_module(const MilenaIRModule *module,
                                   uint8_t **bytes_out, size_t *size_out,
                                   char *error, size_t error_capacity);
bool milena_bytecode_decode_module(const uint8_t *bytes, size_t size,
                                   MilenaIRModule **module_out,
                                   char *error, size_t error_capacity);
bool milena_bytecode_verify(const uint8_t *bytes, size_t size,
                            char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
