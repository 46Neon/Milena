#ifndef MILENA_BYTECODE_COMPILER_H
#define MILENA_BYTECODE_COMPILER_H

#include "bytecode.h"
#include "canonical_compiler.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Experimental source-to-bytecode boundary (retaining v1.0-v1.2 scalar wire
 * compatibility). Scalar lowering consumes canonical MilenaScalarHIR. On
 * success, *bytes_out owns a malloc-allocated encoded program which the caller
 * must free. Outputs are cleared on every failure. The bounded scalar subset
 * supports a zero-parameter numeric `principal`, non-recursive numeric helpers,
 * and the operations documented in BYTECODE_V1.md.
 */
MilenaStatus milena_bytecode_compile_hir(const MilenaScalarHIR *hir,
                                           uint8_t **bytes_out,
                                           size_t *length_out,
                                           MilenaError *error);

/*
 * Lower only the complete v1.3 data-plan HIR shape documented in
 * BYTECODE_DATA_ABI.md. This is a compile-only operation: it does not resolve
 * or read either path. The returned encoded bytes are independently verified
 * with milena_bytecode_verify_data before success; the caller owns and must
 * free them. Both outputs are cleared on every failure.
 */
MilenaStatus milena_bytecode_compile_data_hir(const MilenaDataHIR *hir,
                                                uint8_t **bytes_out,
                                                size_t *length_out,
                                                MilenaError *error);

/* Parse source once through the canonical lexer/parser/semantic/HIR builder,
 * require its exact supported data-HIR shape, and return a verified v1.3 plan. */
MilenaStatus milena_bytecode_compile_data_source(const char *source,
                                                   uint8_t **bytes_out,
                                                   size_t *length_out,
                                                   MilenaError *error);

/* Parse once and dispatch to the canonical scalar or exact v1.3 data lowerer. */
MilenaStatus milena_bytecode_compile_source(const char *source,
                                             uint8_t **bytes_out,
                                             size_t *length_out,
                                             MilenaError *error);

/*
 * Experimental Linux x86-64 AOT backend for verified MLBC v1.0/v1.1/v1.2 bytes.
 * The generated artifact is native code built from source-specific C; v1.1/v1.2
 * function calls become direct C/native calls, not a bytecode-dispatch loop.
 * v1.2 types are verified before native emission. Input is verified before
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
