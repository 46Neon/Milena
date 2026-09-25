#include "ir.h"
#include <stdarg.h>
#include <string.h>
#include <math.h>

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
    free(program->blocks);
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
    ins->arg1 = arg1 ? strdup(arg1) : NULL;
    ins->arg2 = arg2 ? strdup(arg2) : NULL;
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

static bool ir_fail(char *error, size_t capacity, const char *format, ...) {
    if (error && capacity > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(error, capacity, format, args);
        va_end(args);
    }
    return false;
}

static bool ir_reserve_instructions(IRProgram *program) {
    IRInstruction *grown;
    size_t capacity;
    if (program->count < program->capacity) return true;
    capacity = program->capacity == 0 ? 8 : program->capacity * 2;
    if (capacity < program->capacity || capacity > SIZE_MAX / sizeof(*grown)) return false;
    grown = (IRInstruction *)realloc(program->instructions, capacity * sizeof(*grown));
    if (!grown) return false;
    program->instructions = grown;
    program->capacity = capacity;
    return true;
}

bool ir_program_add_block(IRProgram *program, uint32_t block_id) {
    IRBasicBlock *grown;
    size_t capacity;
    if (!program || block_id == 0) return false;
    if (program->block_count > 0 && !program->blocks[program->block_count - 1].terminated) return false;
    for (size_t i = 0; i < program->block_count; ++i) {
        if (program->blocks[i].id == block_id) return false;
    }
    if (program->block_count == program->block_capacity) {
        capacity = program->block_capacity == 0 ? 4 : program->block_capacity * 2;
        if (capacity < program->block_capacity || capacity > SIZE_MAX / sizeof(*grown)) return false;
        grown = (IRBasicBlock *)realloc(program->blocks, capacity * sizeof(*grown));
        if (!grown) return false;
        program->blocks = grown;
        program->block_capacity = capacity;
    }
    IRBasicBlock *block = &program->blocks[program->block_count++];
    memset(block, 0, sizeof(*block));
    block->id = block_id;
    block->first_instruction = program->count;
    return true;
}

bool ir_block_append_instruction(IRProgram *program, uint32_t block_id,
                                 IROpCode opcode, uint32_t result_id,
                                 IRType result_type, uint32_t operand1_id,
                                 uint32_t operand2_id, int64_t integer_immediate,
                                 double float_immediate, uint32_t target_true,
                                 uint32_t target_false) {
    IRBasicBlock *block;
    IRInstruction *instruction;
    bool terminator;
    if (!program || program->block_count == 0) return false;
    block = &program->blocks[program->block_count - 1];
    if (block->id != block_id || block->terminated ||
        block->first_instruction + block->instruction_count != program->count ||
        !ir_reserve_instructions(program)) return false;
    terminator = opcode == IR_BRANCH || opcode == IR_COND_BRANCH || opcode == IR_RETURN;
    instruction = &program->instructions[program->count++];
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->result_id = result_id;
    instruction->result_type = result_type;
    instruction->operand1_id = operand1_id;
    instruction->operand2_id = operand2_id;
    instruction->block_id = block_id;
    instruction->integer_immediate = integer_immediate;
    instruction->float_immediate = float_immediate;
    instruction->target_true = target_true;
    instruction->target_false = target_false;
    block->instruction_count++;
    if (terminator) {
        block->terminated = true;
        block->successor_true = target_true;
        block->successor_false = target_false;
    }
    return true;
}

typedef struct {
    uint32_t id;
    IRType type;
    uint32_t block_id;
} IRValidatedValue;

static const IRValidatedValue *ir_find_value(const IRValidatedValue *values,
                                              size_t count, uint32_t id) {
    for (size_t i = 0; i < count; ++i) {
        if (values[i].id == id) return &values[i];
    }
    return NULL;
}

static bool ir_use_has_type(const IRValidatedValue *values, size_t value_count,
                            uint32_t id, uint32_t block_id, IRType type,
                            char *error, size_t error_capacity) {
    const IRValidatedValue *value = ir_find_value(values, value_count, id);
    if (!value) return ir_fail(error, error_capacity, "undefined IR value %u", id);
    if (value->block_id != block_id)
        return ir_fail(error, error_capacity, "IR value %u is not defined in block %u", id, block_id);
    if (value->type != type)
        return ir_fail(error, error_capacity, "IR value %u has wrong type", id);
    return true;
}

static bool ir_define_value(IRValidatedValue *values, size_t *value_count,
                            size_t value_capacity, const IRInstruction *instruction,
                            char *error, size_t error_capacity) {
    if (instruction->result_id == 0 || instruction->result_type == IR_TYPE_INVALID ||
        instruction->result_type == IR_TYPE_VOID)
        return ir_fail(error, error_capacity, "value-producing opcode has invalid result metadata");
    if (*value_count >= value_capacity || ir_find_value(values, *value_count, instruction->result_id))
        return ir_fail(error, error_capacity, "duplicate or excessive IR value id %u", instruction->result_id);
    values[*value_count].id = instruction->result_id;
    values[*value_count].type = instruction->result_type;
    values[*value_count].block_id = instruction->block_id;
    (*value_count)++;
    return true;
}

static bool ir_target_exists(const IRProgram *program, uint32_t target) {
    for (size_t i = 0; i < program->block_count; ++i) {
        if (program->blocks[i].id == target) return true;
    }
    return false;
}

bool ir_program_validate(const IRProgram *program, char *error,
                         size_t error_capacity) {
    IRValidatedValue *values;
    size_t value_count = 0;
    size_t expected_first = 0;
    if (error && error_capacity > 0) error[0] = '\0';
    if (!program || !program->instructions || !program->blocks ||
        program->count == 0 || program->block_count == 0)
        return ir_fail(error, error_capacity, "typed IR must contain instructions and blocks");
    values = (IRValidatedValue *)calloc(program->count, sizeof(*values));
    if (!values) return ir_fail(error, error_capacity, "out of memory validating IR");

    for (size_t bi = 0; bi < program->block_count; ++bi) {
        const IRBasicBlock *block = &program->blocks[bi];
        size_t end;
        if (block->id == 0 || block->first_instruction != expected_first ||
            block->instruction_count == 0 || block->instruction_count > program->count - expected_first ||
            !block->terminated) {
            free(values);
            return ir_fail(error, error_capacity, "invalid or unterminated IR block at index %zu", bi);
        }
        end = block->first_instruction + block->instruction_count;
        for (size_t prior = 0; prior < bi; ++prior) {
            if (program->blocks[prior].id == block->id) {
                free(values);
                return ir_fail(error, error_capacity, "duplicate IR block id %u", block->id);
            }
        }
        for (size_t ii = block->first_instruction; ii < end; ++ii) {
            const IRInstruction *ins = &program->instructions[ii];
            bool last = ii + 1 == end;
            bool defines = false;
            if (ins->block_id != block->id || ins->arg1 || ins->arg2 || ins->arg3 ||
                ins->opcode < IR_CONST_I64 || ins->opcode >= IR_OPCODE_COUNT) {
                free(values);
                return ir_fail(error, error_capacity, "unsupported or malformed opcode/instruction at %zu", ii);
            }
            switch (ins->opcode) {
                case IR_CONST_I64:
                    defines = true;
                    if (ins->result_type != IR_TYPE_I64 || ins->operand1_id || ins->operand2_id ||
                        ins->target_true || ins->target_false || ins->float_immediate != 0.0) goto bad_shape;
                    break;
                case IR_CONST_F64:
                    defines = true;
                    if (ins->result_type != IR_TYPE_F64 || ins->operand1_id || ins->operand2_id ||
                        ins->target_true || ins->target_false || ins->integer_immediate != 0 ||
                        !isfinite(ins->float_immediate)) goto bad_shape;
                    break;
                case IR_ADD_I64:
                case IR_ADD_F64: {
                    IRType expected = ins->opcode == IR_ADD_I64 ? IR_TYPE_I64 : IR_TYPE_F64;
                    defines = true;
                    if (ins->result_type != expected || ins->operand1_id == 0 || ins->operand2_id == 0 ||
                        ins->integer_immediate || ins->float_immediate != 0.0 ||
                        ins->target_true || ins->target_false ||
                        !ir_use_has_type(values, value_count, ins->operand1_id, block->id, expected, error, error_capacity) ||
                        !ir_use_has_type(values, value_count, ins->operand2_id, block->id, expected, error, error_capacity)) {
                        free(values); return false;
                    }
                    break;
                }
                case IR_EQ_I64:
                    defines = true;
                    if (ins->result_type != IR_TYPE_BOOL || ins->operand1_id == 0 || ins->operand2_id == 0 ||
                        ins->integer_immediate || ins->float_immediate != 0.0 || ins->target_true || ins->target_false ||
                        !ir_use_has_type(values, value_count, ins->operand1_id, block->id, IR_TYPE_I64, error, error_capacity) ||
                        !ir_use_has_type(values, value_count, ins->operand2_id, block->id, IR_TYPE_I64, error, error_capacity)) {
                        free(values); return false;
                    }
                    break;
                case IR_BRANCH:
                    if (!last || ins->result_id || ins->result_type != IR_TYPE_VOID || ins->operand1_id ||
                        ins->operand2_id || ins->integer_immediate || ins->float_immediate != 0.0 ||
                        ins->target_true == 0 || ins->target_false != 0) goto bad_shape;
                    break;
                case IR_COND_BRANCH:
                    if (!last || ins->result_id || ins->result_type != IR_TYPE_VOID || ins->operand1_id == 0 ||
                        ins->operand2_id || ins->integer_immediate || ins->float_immediate != 0.0 ||
                        ins->target_true == 0 || ins->target_false == 0 ||
                        !ir_use_has_type(values, value_count, ins->operand1_id, block->id, IR_TYPE_BOOL, error, error_capacity)) {
                        free(values); return false;
                    }
                    break;
                case IR_RETURN:
                    if (!last || ins->result_id || ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || ins->target_true || ins->target_false) goto bad_shape;
                    if (ins->operand1_id == 0) {
                        if (ins->result_type != IR_TYPE_VOID) goto bad_shape;
                    } else if (ins->result_type == IR_TYPE_VOID ||
                               !ir_use_has_type(values, value_count, ins->operand1_id, block->id,
                                                ins->result_type, error, error_capacity)) {
                        free(values); return false;
                    }
                    break;
                default:
                    free(values);
                    return ir_fail(error, error_capacity, "unsupported IR opcode %d", (int)ins->opcode);
            }
            if (defines && !ir_define_value(values, &value_count, program->count, ins, error, error_capacity)) {
                free(values); return false;
            }
            if (last != (ins->opcode == IR_BRANCH || ins->opcode == IR_COND_BRANCH || ins->opcode == IR_RETURN)) {
                free(values);
                return ir_fail(error, error_capacity, "block %u must end with exactly one terminator", block->id);
            }
            continue;
        bad_shape:
            free(values);
            return ir_fail(error, error_capacity, "invalid operands, result type, or targets at instruction %zu", ii);
        }
        expected_first = end;
        if (block->successor_true != program->instructions[end - 1].target_true ||
            block->successor_false != program->instructions[end - 1].target_false) {
            free(values);
            return ir_fail(error, error_capacity, "block %u successor metadata disagrees with terminator", block->id);
        }
    }
    if (expected_first != program->count) {
        free(values);
        return ir_fail(error, error_capacity, "instructions are not assigned to a basic block");
    }
    for (size_t bi = 0; bi < program->block_count; ++bi) {
        const IRBasicBlock *block = &program->blocks[bi];
        if ((block->successor_true && !ir_target_exists(program, block->successor_true)) ||
            (block->successor_false && !ir_target_exists(program, block->successor_false))) {
            free(values);
            return ir_fail(error, error_capacity, "block %u branches to a missing block", block->id);
        }
    }
    free(values);
    return true;
}
