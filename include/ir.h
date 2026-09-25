#ifndef MILENA_IR_H
#define MILENA_IR_H

#include "common.h"
#include "ast.h"
#include "symbol.h"
#include <stdint.h>

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

typedef struct IRProgram {
    IRInstruction *instructions;
    size_t count;
    size_t capacity;
    SymbolTable *symbols;
    IRBasicBlock *blocks;
    size_t block_count;
    size_t block_capacity;
} IRProgram;

IRProgram* ir_program_create(void);
void ir_program_destroy(IRProgram *program);
void ir_add_instruction(IRProgram *program, IROpCode opcode, const char *arg1, const char *arg2);
void ir_add_instruction_num(IRProgram *program, IROpCode opcode, double num1);
void ir_print_program(IRProgram *program);
bool ir_generate(IRProgram *program, ASTNode *ast);

/* Incremental typed portable IR construction API. Blocks are appended in
 * layout order; each block must end in exactly one terminator before another
 * block can be opened. Validation deliberately rejects legacy string opcodes. */
bool ir_program_add_block(IRProgram *program, uint32_t block_id);
bool ir_block_append_instruction(IRProgram *program, uint32_t block_id,
                                 IROpCode opcode, uint32_t result_id,
                                 IRType result_type, uint32_t operand1_id,
                                 uint32_t operand2_id, int64_t integer_immediate,
                                 double float_immediate, uint32_t target_true,
                                 uint32_t target_false);
bool ir_program_validate(const IRProgram *program, char *error,
                         size_t error_capacity);

#endif
