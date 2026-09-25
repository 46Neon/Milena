#ifndef MILENA_TYPED_IR_H
#define MILENA_TYPED_IR_H

#include "common.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MilenaHIRFunction MilenaHIRFunction;
typedef struct MilenaScalarHIR MilenaScalarHIR;

typedef enum {
    MILENA_IR_CONST_I64,
    MILENA_IR_CONST_F64,
    MILENA_IR_ADD_I64,
    MILENA_IR_ADD_F64,
    MILENA_IR_EQ_I64,
    MILENA_IR_BRANCH,
    MILENA_IR_COND_BRANCH,
    MILENA_IR_RETURN,
    MILENA_IR_CONST_BOOL,
    MILENA_IR_SUB_F64,
    MILENA_IR_MUL_F64,
    MILENA_IR_DIV_F64,
    MILENA_IR_EQ_F64,
    MILENA_IR_NE_F64,
    MILENA_IR_LT_F64,
    MILENA_IR_LE_F64,
    MILENA_IR_GT_F64,
    MILENA_IR_GE_F64,
    MILENA_IR_CALL,
    MILENA_IR_OPCODE_COUNT
} MilenaIROpCode;

typedef enum {
    MILENA_IR_TYPE_INVALID = 0,
    MILENA_IR_TYPE_I64,
    MILENA_IR_TYPE_F64,
    MILENA_IR_TYPE_BOOL,
    MILENA_IR_TYPE_VOID
} MilenaIRType;

typedef struct MilenaIRInstruction {
    MilenaIROpCode opcode;
    MilenaIRType result_type;
    uint32_t result_id;
    uint32_t operand1_id;
    uint32_t operand2_id;
    /* Slice into MilenaIRProgram.call_arguments; used only by MILENA_IR_CALL. */
    size_t call_argument_offset;
    size_t call_argument_count;
    uint32_t block_id;
    int64_t integer_immediate;
    double float_immediate;
    uint32_t target_true;
    uint32_t target_false;
} MilenaIRInstruction;

typedef struct MilenaIRBasicBlock {
    uint32_t id;
    size_t first_instruction;
    size_t instruction_count;
    uint32_t successor_true;
    uint32_t successor_false;
    bool terminated;
} MilenaIRBasicBlock;

typedef struct MilenaIRBlockParameter {
    uint32_t block_id;
    uint32_t value_id;
    MilenaIRType type;
} MilenaIRBlockParameter;

typedef struct MilenaIREdgeArgument {
    uint32_t source_block_id;
    uint32_t target_block_id;
    uint32_t parameter_index;
    uint32_t value_id;
} MilenaIREdgeArgument;

typedef struct MilenaIRFunctionSignature {
    MilenaIRType *parameter_types;
    size_t parameter_count;
    MilenaIRType return_type;
} MilenaIRFunctionSignature;

typedef struct MilenaIRModule MilenaIRModule;

typedef struct MilenaIRProgram {
    MilenaIRInstruction *instructions;
    size_t count;
    size_t capacity;
    uint32_t *call_arguments;
    size_t call_argument_count;
    size_t call_argument_capacity;
    MilenaIRBasicBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    MilenaIRBlockParameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    MilenaIREdgeArgument *edge_arguments;
    size_t edge_argument_count;
    size_t edge_argument_capacity;
    MilenaIRFunctionSignature signature;
    bool has_function_signature;
    const MilenaIRModule *module_context; /* Borrowed while owned by the canonical module. */
} MilenaIRProgram;

typedef struct MilenaIRModuleFunction {
    char *name;
    uint32_t symbol_id;
    MilenaIRType *parameter_types;
    size_t parameter_count;
    MilenaIRType return_type;
    MilenaIRProgram *body;
} MilenaIRModuleFunction;

typedef struct MilenaIRModule {
    MilenaIRModuleFunction *functions;
    size_t function_count;
} MilenaIRModule;

/* This canonical typed representation is independent of the experimental
 * string-based IRProgram declared by ir.h. */
MilenaIRProgram *milena_ir_program_create(void);
void milena_ir_program_destroy(MilenaIRProgram *program);
bool milena_ir_program_set_function_signature(MilenaIRProgram *program,
                                               const MilenaIRType *parameter_types,
                                               size_t parameter_count,
                                               MilenaIRType return_type);
bool milena_ir_program_add_block(MilenaIRProgram *program, uint32_t block_id);
bool milena_ir_program_add_block_parameter(MilenaIRProgram *program,
                                            uint32_t block_id,
                                            uint32_t value_id,
                                            MilenaIRType type);
bool milena_ir_block_add_edge_argument(MilenaIRProgram *program,
                                        uint32_t source_block_id,
                                        uint32_t target_block_id,
                                        uint32_t parameter_index,
                                        uint32_t value_id);
bool milena_ir_block_append_instruction(MilenaIRProgram *program,
                                         uint32_t block_id,
                                         MilenaIROpCode opcode,
                                         uint32_t result_id,
                                         MilenaIRType result_type,
                                         uint32_t operand1_id,
                                         uint32_t operand2_id,
                                         int64_t integer_immediate,
                                         double float_immediate,
                                         uint32_t target_true,
                                         uint32_t target_false);
/* Append a call with a copied, owned argument vector. The target is a stable
 * module symbol ID; call arguments are not stored in the fixed scalar operands. */
bool milena_ir_block_append_call(MilenaIRProgram *program, uint32_t block_id,
                                 uint32_t result_id, MilenaIRType result_type,
                                 uint32_t target_symbol_id,
                                 const uint32_t *argument_ids,
                                 size_t argument_count);
bool milena_ir_program_validate(const MilenaIRProgram *program, char *error,
                                size_t error_capacity);
void milena_ir_module_destroy(MilenaIRModule *module);
bool milena_ir_module_validate(const MilenaIRModule *module, char *error,
                               size_t error_capacity);
bool milena_ir_module_lower_scalar_hir(MilenaIRModule **output,
                                        const MilenaScalarHIR *hir,
                                        char *error, size_t error_capacity);
bool milena_ir_program_lower_scalar_function_body_in_module(
    MilenaIRProgram *program, const MilenaHIRFunction *function,
    const MilenaIRModule *module, char *error, size_t error_capacity);

/* Lower one canonical scalar-HIR function body into a fresh, verified typed-IR
 * body. The current slice accepts typed numeric (F64) input parameters and
 * numeric/bool locals, one final return, nonterminal si/sino statements whose
 * arms may declare branch-scoped locals and assign bindings visible on entry
 * (outer bindings are merged with typed block parameters), including nested
 * complete si/sino statements, or a final si/sino whose two arms each return
 * immediately. Branch-local declarations never escape their lexical branch.
 * The IR carries an explicit signature and entry-block parameter definitions.
 * Calls, missing else arms, returns inside assignment branches, and other
 * control-flow shapes fail closed. Nested lowering is bounded to 128 levels.
 * `program` must be empty. */
bool milena_ir_program_lower_scalar_function_body(
    MilenaIRProgram *program, const MilenaHIRFunction *function, char *error,
    size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
