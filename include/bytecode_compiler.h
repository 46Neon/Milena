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

/*
 * Experimental Linux x86-64 AOT backend for verified MLBC v1 bytes. The
 * generated artifact is a native executable built from straight-line C with
 * direct labels/gotos (not a bytecode-dispatch loop). Input is verified before
 * any output is published. On success, the executable prints its numeric result
 * as a C hexadecimal floating literal and exits 0. Runtime errors exit 70,
 * instruction-budget exhaustion exits 71, and stdout failure exits 74. The
 * implementation invokes the fixed /usr/bin/cc path with an argv vector (never
 * through a shell), uses private temporary files, and atomically publishes the
 * output path; callers own and must eventually remove the executable.
 */
MilenaStatus milena_bytecode_compile_native(const uint8_t *bytes,
                                             size_t length,
                                             const char *executable_path,
                                             MilenaError *error);

#ifdef __cplusplus
}
#endif
#endif
