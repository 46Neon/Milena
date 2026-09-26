#ifndef MILENA_BYTECODE_COMPILER_H
#define MILENA_BYTECODE_COMPILER_H

#include "bytecode.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Experimental source-to-bytecode v1 boundary. The source is parsed and
 * semantically validated by milena_canonical_program_parse; lowering consumes
 * only the resulting canonical MilenaScalarHIR. On success, *bytes_out owns a
 * malloc-allocated encoded program which the caller must free. Outputs are
 * cleared on every failure. This API intentionally supports only one
 * zero-parameter numeric entry function named `principal` and the closed subset
 * documented in docs/BYTECODE_V1.md.
 */
MilenaStatus milena_bytecode_compile_source(const char *source,
                                             uint8_t **bytes_out,
                                             size_t *length_out,
                                             MilenaError *error);

#ifdef __cplusplus
}
#endif
#endif
