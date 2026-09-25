#ifndef MILENA_VM_H
#define MILENA_VM_H

#include "typed_bytecode.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MilenaIRType type;
    union {
        int64_t i64;
        double f64;
        bool boolean;
    } as;
} MilenaVMValue;

typedef struct {
    uint64_t max_steps;       /* 0 selects the safe default. */
    size_t max_call_depth;    /* 0 selects the safe default. */
} MilenaVMOptions;

/* The canonical Milena VM entry point consumes only a complete portable MLBC
 * module that passes the bytecode decoder/verifier. It never runs raw typed IR,
 * the legacy string-based IRProgram, or dataset-specific VM instructions. The
 * result is unchanged on failure; options may be NULL for safe defaults. This
 * API is internal compiler infrastructure, not a shipped language runtime. */
bool vm_run(const uint8_t *bytecode, size_t bytecode_size,
            uint32_t entry_symbol_id, const MilenaVMValue *arguments,
            size_t argument_count, const MilenaVMOptions *options,
            MilenaVMValue *result, char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
