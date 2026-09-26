#ifndef MILENA_VM_H
#define MILENA_VM_H

#include "common.h"
#include "ir.h"
#include "dataset.h"
#include "gc.h"
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

typedef enum {
    MILENA_VM_MODE_UNINITIALIZED = 0,
    MILENA_VM_MODE_ORIGINAL_IR = 1,
    MILENA_VM_MODE_VERIFIED_BYTECODE = 2
} MilenaVMMode;

struct VMBytecodePayload;

typedef struct VirtualMachine {
    /* Original Milena VM state and lifecycle retained for compatibility. */
    IRProgram *program;
    size_t pc;
    Dataset *dataset;
    Dataset *result;
    GC *gc;
    bool running;
    bool has_error;
    MilenaErrorInfo error;

    /* The verified-bytecode execution state is owned by this same VM object. */
    MilenaVMMode mode;
    struct VMBytecodePayload *bytecode_state;
} VirtualMachine;

bool vm_init(VirtualMachine *vm, IRProgram *program);
bool vm_run(VirtualMachine *vm);
void vm_step(VirtualMachine *vm);
void vm_destroy(VirtualMachine *vm);

/* Initialize the original VM object in its verified-MLBC mode. The bytecode is
 * decoded and semantically verified before vm_run can execute it. */
bool vm_init_bytecode(VirtualMachine *vm, const uint8_t *bytecode,
                      size_t bytecode_size, uint32_t entry_symbol_id,
                      const MilenaVMValue *arguments, size_t argument_count,
                      const MilenaVMOptions *options);
bool vm_get_bytecode_result(const VirtualMachine *vm,
                            MilenaVMValue *result_out);
const char *vm_bytecode_error(const VirtualMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
