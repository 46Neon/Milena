#ifndef MILENA_TYPED_VM_H
#define MILENA_TYPED_VM_H

#include "typed_bytecode.h"

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
} MilenaTypedVMValue;

typedef struct {
    uint64_t max_steps;       /* 0 selects the default limit. */
    size_t max_call_depth;    /* 0 selects the default limit. */
} MilenaTypedVMOptions;

/* Internal reference executor. It accepts only serialized modules which pass
 * the canonical bytecode decoder/verifier; it never executes raw IR. On any
 * error `result` is left unchanged. A NULL options pointer uses safe defaults. */
bool milena_typed_vm_execute(const uint8_t *bytecode, size_t bytecode_size,
                             uint32_t entry_symbol_id,
                             const MilenaTypedVMValue *arguments,
                             size_t argument_count,
                             const MilenaTypedVMOptions *options,
                             MilenaTypedVMValue *result,
                             char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
