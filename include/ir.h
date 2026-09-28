#ifndef MILENA_IR_H
#define MILENA_IR_H

#include "common.h"
#include "ast.h"
#include "symbol.h"
#include <stdint.h>

/* Experimental legacy string IR. Production typed lowering uses typed_ir.h and
 * MilenaIRProgram; this separate container is retained only by the isolated
 * compiler/assembler/VM prototype. */
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
    IR_PRINT
} IROpCode;

typedef struct IRInstruction {
    IROpCode opcode;
    char *arg1;
    char *arg2;
    char *arg3;
    double num_arg1;
    double num_arg2;
} IRInstruction;

typedef struct IRProgram {
    IRInstruction *instructions;
    size_t count;
    size_t capacity;
    SymbolTable *symbols;
} IRProgram;

IRProgram *ir_program_create(void);
void ir_program_destroy(IRProgram *program);
void ir_add_instruction(IRProgram *program, IROpCode opcode,
                        const char *arg1, const char *arg2);
void ir_add_instruction_num(IRProgram *program, IROpCode opcode, double num1);
void ir_print_program(IRProgram *program);
bool ir_generate(IRProgram *program, ASTNode *ast);

#endif
