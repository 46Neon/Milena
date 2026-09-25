#include "ir.h"
#include <string.h>
#include <stdlib.h>

static char *ir_duplicate_string(const char *source) {
    size_t length;
    char *copy;
    if (!source) return NULL;
    length = strlen(source);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, source, length + 1);
    return copy;
}

IRProgram* ir_program_create(void) {
    IRProgram *program = (IRProgram *)calloc(1, sizeof(IRProgram));
    if (!program) return NULL;
    
    program->instructions = NULL;
    program->count = 0;
    program->capacity = 0;
    program->symbols = NULL;
    
    return program;
}

void ir_program_destroy(IRProgram *program) {
    if (!program) return;
    
    for (size_t i = 0; i < program->count; i++) {
        free(program->instructions[i].arg1);
        free(program->instructions[i].arg2);
        free(program->instructions[i].arg3);
    }
    
    free(program->instructions);
    free(program);
}

void ir_add_instruction(IRProgram *program, IROpCode opcode, const char *arg1, const char *arg2) {
    if (!program) return;
    
    if (program->count >= program->capacity) {
        size_t new_capacity = program->capacity == 0 ? 16 : program->capacity * 2;
        IRInstruction *new_ins = (IRInstruction *)realloc(program->instructions, 
                                                          new_capacity * sizeof(IRInstruction));
        if (!new_ins) return;
        
        program->instructions = new_ins;
        program->capacity = new_capacity;
    }
    
    IRInstruction *ins = &program->instructions[program->count++];
    memset(ins, 0, sizeof(*ins));
    ins->opcode = opcode;
    ins->arg1 = ir_duplicate_string(arg1);
    ins->arg2 = ir_duplicate_string(arg2);
    ins->arg3 = NULL;
    ins->num_arg1 = 0.0;
    ins->num_arg2 = 0.0;
}

void ir_add_instruction_num(IRProgram *program, IROpCode opcode, double num1) {
    if (!program) return;
    
    if (program->count >= program->capacity) {
        size_t new_capacity = program->capacity == 0 ? 16 : program->capacity * 2;
        IRInstruction *new_ins = (IRInstruction *)realloc(program->instructions, 
                                                          new_capacity * sizeof(IRInstruction));
        if (!new_ins) return;
        
        program->instructions = new_ins;
        program->capacity = new_capacity;
    }
    
    IRInstruction *ins = &program->instructions[program->count++];
    memset(ins, 0, sizeof(*ins));
    ins->opcode = opcode;
    ins->arg1 = NULL;
    ins->arg2 = NULL;
    ins->arg3 = NULL;
    ins->num_arg1 = num1;
    ins->num_arg2 = 0.0;
}

void ir_print_program(IRProgram *program) {
    if (!program) return;
    
    printf("IR Program (%zu instrucciones):\n", program->count);
    for (size_t i = 0; i < program->count; i++) {
        IRInstruction *ins = &program->instructions[i];
        printf("[%03zu] ", i);
        
        switch (ins->opcode) {
            case IR_LOAD_DATASET: printf("LOAD_DATASET"); break;
            case IR_CLEAN_NULLS: printf("CLEAN_NULLS"); break;
            case IR_CLEAN_DUPLICATES: printf("CLEAN_DUPLICATES"); break;
            case IR_TRANSFORM_TOTAL: printf("TRANSFORM_TOTAL"); break;
            case IR_TRANSFORM_PERIOD: printf("TRANSFORM_PERIOD"); break;
            case IR_FILTER_CONDITION: printf("FILTER_CONDITION"); break;
            case IR_GROUP_BY: printf("GROUP_BY"); break;
            case IR_AGGREGATE_SUM: printf("AGGREGATE_SUM"); break;
            case IR_AGGREGATE_AVG: printf("AGGREGATE_AVG"); break;
            case IR_AGGREGATE_MIN: printf("AGGREGATE_MIN"); break;
            case IR_AGGREGATE_MAX: printf("AGGREGATE_MAX"); break;
            case IR_VISUALIZE: printf("VISUALIZE"); break;
            case IR_EXPORT_JSON: printf("EXPORT_JSON"); break;
            case IR_PRINT: printf("PRINT"); break;
            default: printf("UNKNOWN"); break;
        }
        
        if (ins->arg1) printf(" %s", ins->arg1);
        if (ins->arg2) printf(" %s", ins->arg2);
        if (ins->num_arg1 != 0.0) printf(" %.2f", ins->num_arg1);
        printf("\n");
    }
}

static bool ir_generate_from_ast(IRProgram *program, ASTNode *node) {
    if (!program || !node) return true;
    
    switch (node->type) {
        case AST_PROGRAMA:
            for (size_t i = 0; i < node->child_count; i++) {
                if (!ir_generate_from_ast(program, node->children[i])) {
                    return false;
                }
            }
            break;
            
        case AST_BLOQUE_ANALISIS:
            for (size_t i = 0; i < node->child_count; i++) {
                if (!ir_generate_from_ast(program, node->children[i])) {
                    return false;
                }
            }
            break;
            
        case AST_LLAMADA_CARGAR:
            if (node->value) {
                ir_add_instruction(program, IR_LOAD_DATASET, node->value, NULL);
            }
            break;
            
        case AST_COMANDO_NULOS:
            if (node->value) {
                ir_add_instruction(program, IR_CLEAN_NULLS, node->value, NULL);
            }
            break;
            
        case AST_COMANDO_DUPLICADOS:
            if (node->value) {
                ir_add_instruction(program, IR_CLEAN_DUPLICATES, node->value, NULL);
            }
            break;
            
        case AST_COMANDO_TOTAL:
            if (node->value) {
                ir_add_instruction(program, IR_TRANSFORM_TOTAL, node->value, NULL);
            }
            break;
            
        case AST_COMANDO_PERIODO:
            if (node->value) {
                ir_add_instruction(program, IR_TRANSFORM_PERIOD, node->value, NULL);
            }
            break;
            
        case AST_COMANDO_CONDICION:
            if (node->value) {
                ir_add_instruction(program, IR_FILTER_CONDITION, node->value, NULL);
            }
            break;
            
        case AST_BLOQUE_AGRUPAR:
            for (size_t i = 0; i < node->child_count; i++) {
                if (!ir_generate_from_ast(program, node->children[i])) {
                    return false;
                }
            }
            break;
            
        case AST_AGRUPACION_POR:
            if (node->value) {
                ir_add_instruction(program, IR_GROUP_BY, node->value, NULL);
            }
            break;
            
        case AST_RESUMEN_METRICA:
            if (node->value) {
                if (strstr(node->value, "total") || strstr(node->value, "sum")) {
                    ir_add_instruction(program, IR_AGGREGATE_SUM, node->value, NULL);
                } else if (strstr(node->value, "promedio") || strstr(node->value, "avg")) {
                    ir_add_instruction(program, IR_AGGREGATE_AVG, node->value, NULL);
                } else if (strstr(node->value, "max")) {
                    ir_add_instruction(program, IR_AGGREGATE_MAX, node->value, NULL);
                } else if (strstr(node->value, "min")) {
                    ir_add_instruction(program, IR_AGGREGATE_MIN, node->value, NULL);
                }
            }
            break;
            
        case AST_BLOQUE_VISUALIZAR:
            ir_add_instruction(program, IR_VISUALIZE, NULL, NULL);
            break;
            
        case AST_BLOQUE_EXPORTAR:
            if (node->value) {
                ir_add_instruction(program, IR_EXPORT_JSON, node->value, NULL);
            }
            break;
            
        default:
            // Recursivamente procesar hijos
            for (size_t i = 0; i < node->child_count; i++) {
                if (!ir_generate_from_ast(program, node->children[i])) {
                    return false;
                }
            }
            break;
    }
    
    return true;
}

bool ir_generate(IRProgram *program, ASTNode *ast) {
    if (!program || !ast) return false;
    
    return ir_generate_from_ast(program, ast);
}

