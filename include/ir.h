#ifndef MILENA_IR_H
#define MILENA_IR_H

#include "common.h"
#include "ast.h"
#include "symbol.h"
#include <stdint.h>

#ifndef MILENA_CANONICAL_COMPILER_H
typedef struct MilenaHIRFunction MilenaHIRFunction;
#endif

typedef enum {
    IR_LOAD_DATASET,
    IR_CLEAN_NULLS,
    IR_CLEAN_DUPLICATES,
    IR_TRANSFORM_TOTAL,
    IR_TRANSFORM_PERIOD,
    IR_FILTER_CONDITION,
    IR_GROUP_BY,
    IR_AGGREGATE_SUM,
    IR_AGGREGATE_AVG,
    IR_AGGREGATE_MIN,
    IR_AGGREGATE_MAX,
    IR_VISUALIZE,
    IR_EXPORT_JSON,
    IR_PRINT,
    /* Typed portable-IR subset. Legacy operations above remain experimental. */
    IR_CONST_I64,
    IR_CONST_F64,
    IR_ADD_I64,
    IR_ADD_F64,
    IR_EQ_I64,
    IR_BRANCH,
    IR_COND_BRANCH,
    IR_RETURN,
    IR_CONST_BOOL,
    IR_SUB_F64,
    IR_MUL_F64,
    IR_DIV_F64,
    IR_EQ_F64,
    IR_NE_F64,
    IR_LT_F64,
    IR_LE_F64,
    IR_GT_F64,
    IR_GE_F64,
    IR_OPCODE_COUNT
} IROpCode;

typedef enum {
    IR_TYPE_INVALID = 0,
    IR_TYPE_I64,
    IR_TYPE_F64,
    IR_TYPE_BOOL,
    IR_TYPE_VOID
} IRType;

typedef struct IRInstruction {
    IROpCode opcode;
    char *arg1;
    char *arg2;
    char *arg3;
    double num_arg1;
    double num_arg2;
    /* Typed-IR metadata; value id zero is reserved for "no value". */
    IRType result_type;
    uint32_t result_id;
    uint32_t operand1_id;
    uint32_t operand2_id;
    uint32_t block_id;
    int64_t integer_immediate;
    double float_immediate;
    uint32_t target_true;
    uint32_t target_false;
} IRInstruction;

typedef struct IRBasicBlock {
    uint32_t id;
    size_t first_instruction;
    size_t instruction_count;
    uint32_t successor_true;
    uint32_t successor_false;
    bool terminated;
} IRBasicBlock;

typedef struct IRBlockParameter {
    uint32_t block_id;
    uint32_t value_id;
    IRType type;
} IRBlockParameter;

typedef struct IREdgeArgument {
    uint32_t source_block_id;
    uint32_t target_block_id;
    uint32_t parameter_index;
    uint32_t value_id;
} IREdgeArgument;

typedef struct IRProgram {
    IRInstruction *instructions;
    size_t count;
    size_t capacity;
    SymbolTable *symbols;
    IRBasicBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    IRBlockParameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    IREdgeArgument *edge_arguments;
    size_t edge_argument_count;
    size_t edge_argument_capacity;
} IRProgram;

IRProgram* ir_program_create(void);
void ir_program_destroy(IRProgram *program);
void ir_add_instruction(IRProgram *program, IROpCode opcode, const char *arg1, const char *arg2);
void ir_add_instruction_num(IRProgram *program, IROpCode opcode, double num1);
void ir_print_program(IRProgram *program);
bool ir_generate(IRProgram *program, ASTNode *ast);

/* Incremental typed portable IR construction API. Blocks are appended in
 * layout order; each block must end in exactly one terminator before another
 * block can be opened. Block parameters and explicit edge arguments provide
 * SSA-style value flow. Validation rejects legacy string opcodes and checks
 * CFG reachability, dominance, and edge argument arity/types. */
bool ir_program_add_block(IRProgram *program, uint32_t block_id);
/* Parameters are added while the target block is open, before its first
 * instruction. Every incoming CFG edge must supply one typed argument per
 * parameter, in parameter-index order. */
bool ir_program_add_block_parameter(IRProgram *program, uint32_t block_id,
                                    uint32_t value_id, IRType type);
bool ir_block_add_edge_argument(IRProgram *program, uint32_t source_block_id,
                                uint32_t target_block_id, uint32_t parameter_index,
                                uint32_t value_id);
bool ir_block_append_instruction(IRProgram *program, uint32_t block_id,
                                 IROpCode opcode, uint32_t result_id,
                                 IRType result_type, uint32_t operand1_id,
                                 uint32_t operand2_id, int64_t integer_immediate,
                                 double float_immediate, uint32_t target_true,
                                 uint32_t target_false);
bool ir_program_validate(const IRProgram *program, char *error,
                         size_t error_capacity);

/* Lower one canonical scalar-HIR function body into a fresh, verified typed-IR
 * body. The current slice accepts zero parameters, numeric/bool locals, and
 * either one final return or a final si/sino whose two arms each return
 * immediately. Calls and other control-flow shapes fail closed. `program`
 * must be empty. Function identity/signatures are not yet represented in IR. */
bool ir_program_lower_scalar_function_body(IRProgram *program,
                                           const MilenaHIRFunction *function,
                                           char *error, size_t error_capacity);

#endif
