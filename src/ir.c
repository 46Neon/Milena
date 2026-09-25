#include "ir.h"
#include <stdarg.h>
#include <string.h>
#include <math.h>
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
    free(program->blocks);
    free(program->parameters);
    free(program->edge_arguments);
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

bool ir_program_add_block_parameter(IRProgram *program, uint32_t block_id,
                                    uint32_t value_id, IRType type) {
    IRBlockParameter *grown;
    size_t capacity;
    IRBasicBlock *block;
    if (!program || program->block_count == 0 || value_id == 0 ||
        type <= IR_TYPE_INVALID || type >= IR_TYPE_VOID) return false;
    block = &program->blocks[program->block_count - 1];
    if (block->id != block_id || block->instruction_count != 0 || block->terminated)
        return false;
    for (size_t i = 0; i < program->parameter_count; ++i)
        if (program->parameters[i].block_id == block_id &&
            program->parameters[i].value_id == value_id) return false;
    if (program->parameter_count == program->parameter_capacity) {
        capacity = program->parameter_capacity == 0 ? 4 : program->parameter_capacity * 2;
        if (capacity < program->parameter_capacity ||
            capacity > SIZE_MAX / sizeof(*grown)) return false;
        grown = (IRBlockParameter *)realloc(program->parameters,
                                             capacity * sizeof(*grown));
        if (!grown) return false;
        program->parameters = grown;
        program->parameter_capacity = capacity;
    }
    program->parameters[program->parameter_count++] =
        (IRBlockParameter){block_id, value_id, type};
    return true;
}

bool ir_block_add_edge_argument(IRProgram *program, uint32_t source_block_id,
                                uint32_t target_block_id, uint32_t parameter_index,
                                uint32_t value_id) {
    IREdgeArgument *grown;
    size_t capacity;
    if (!program || source_block_id == 0 || target_block_id == 0 || value_id == 0)
        return false;
    if (program->edge_argument_count == program->edge_argument_capacity) {
        capacity = program->edge_argument_capacity == 0 ? 8 :
                   program->edge_argument_capacity * 2;
        if (capacity < program->edge_argument_capacity ||
            capacity > SIZE_MAX / sizeof(*grown)) return false;
        grown = (IREdgeArgument *)realloc(program->edge_arguments,
                                           capacity * sizeof(*grown));
        if (!grown) return false;
        program->edge_arguments = grown;
        program->edge_argument_capacity = capacity;
    }
    program->edge_arguments[program->edge_argument_count++] =
        (IREdgeArgument){source_block_id, target_block_id, parameter_index, value_id};
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
    size_t block_index;
    size_t instruction_index;
    bool parameter;
} IRValidatedValue;

static const IRValidatedValue *ir_find_value(const IRValidatedValue *values,
                                              size_t count, uint32_t id) {
    for (size_t i = 0; i < count; ++i)
        if (values[i].id == id) return &values[i];
    return NULL;
}

static size_t ir_find_block_index(const IRProgram *program, uint32_t id) {
    for (size_t i = 0; i < program->block_count; ++i)
        if (program->blocks[i].id == id) return i;
    return SIZE_MAX;
}

static const IRBlockParameter *ir_nth_parameter(const IRProgram *program,
                                                 uint32_t block_id,
                                                 uint32_t index) {
    uint32_t seen = 0;
    for (size_t i = 0; i < program->parameter_count; ++i) {
        if (program->parameters[i].block_id == block_id) {
            if (seen == index) return &program->parameters[i];
            ++seen;
        }
    }
    return NULL;
}

static bool ir_add_validated_value(IRValidatedValue *values, size_t *count,
                                   size_t capacity, uint32_t id, IRType type,
                                   size_t block_index, size_t instruction_index,
                                   bool parameter, char *error,
                                   size_t error_capacity) {
    if (id == 0 || type <= IR_TYPE_INVALID || type >= IR_TYPE_VOID ||
        *count >= capacity)
        return ir_fail(error, error_capacity, "value has invalid id or type");
    if (ir_find_value(values, *count, id))
        return ir_fail(error, error_capacity, "duplicate IR value id %u", id);
    values[*count] = (IRValidatedValue){id, type, block_index,
                                         instruction_index, parameter};
    ++*count;
    return true;
}

static bool ir_use_is_valid(const IRValidatedValue *values, size_t value_count,
                            uint32_t id, IRType expected_type, size_t use_block,
                            size_t use_instruction, size_t block_count,
                            const unsigned char *dominators, char *error,
                            size_t error_capacity) {
    const IRValidatedValue *value = ir_find_value(values, value_count, id);
    if (!value)
        return ir_fail(error, error_capacity, "undefined IR value %u", id);
    if (value->type != expected_type)
        return ir_fail(error, error_capacity, "IR value %u has wrong type", id);
    if (value->block_index == use_block) {
        if (!value->parameter && value->instruction_index >= use_instruction)
            return ir_fail(error, error_capacity,
                           "IR value %u is used before its definition", id);
    } else if (!dominators[use_block * block_count + value->block_index]) {
        return ir_fail(error, error_capacity,
                       "IR value %u does not dominate its use", id);
    }
    return true;
}

bool ir_program_validate(const IRProgram *program, char *error,
                         size_t error_capacity) {
    IRValidatedValue *values = NULL;
    unsigned char *reachable = NULL;
    unsigned char *dominators = NULL;
    size_t *queue = NULL;
    size_t value_count = 0;
    size_t value_capacity;
    size_t expected_first = 0;
    size_t n;
    bool changed;
    if (error && error_capacity > 0) error[0] = '\0';
    if (!program || !program->instructions || !program->blocks ||
        program->count == 0 || program->block_count == 0 ||
        (program->parameter_count && !program->parameters) ||
        (program->edge_argument_count && !program->edge_arguments))
        return ir_fail(error, error_capacity,
                       "typed IR must contain valid instructions and blocks");
    n = program->block_count;
    if (program->parameter_count > SIZE_MAX - program->count)
        return ir_fail(error, error_capacity, "too many typed IR values");
    value_capacity = program->parameter_count + program->count;
    if (n > SIZE_MAX / n)
        return ir_fail(error, error_capacity, "CFG is too large to validate");
    values = (IRValidatedValue *)calloc(value_capacity, sizeof(*values));
    reachable = (unsigned char *)calloc(n, sizeof(*reachable));
    queue = (size_t *)calloc(n, sizeof(*queue));
    dominators = (unsigned char *)calloc(n * n, sizeof(*dominators));
    if (!values || !reachable || !queue || !dominators) {
        free(values); free(reachable); free(queue); free(dominators);
        return ir_fail(error, error_capacity, "out of memory validating IR");
    }
#define IR_REJECT(...) do { \
    free(values); free(reachable); free(queue); free(dominators); \
    return ir_fail(error, error_capacity, __VA_ARGS__); \
} while (0)

    for (size_t bi = 0; bi < n; ++bi) {
        const IRBasicBlock *block = &program->blocks[bi];
        size_t end;
        if (block->id == 0 || block->first_instruction != expected_first ||
            expected_first > program->count || block->instruction_count == 0 ||
            block->instruction_count > program->count - expected_first ||
            !block->terminated)
            IR_REJECT("invalid or unterminated IR block at index %zu", bi);
        end = block->first_instruction + block->instruction_count;
        for (size_t prior = 0; prior < bi; ++prior)
            if (program->blocks[prior].id == block->id)
                IR_REJECT("duplicate IR block id %u", block->id);
        expected_first = end;
    }
    if (expected_first != program->count)
        IR_REJECT("instructions are not assigned to a basic block");

    /* Block parameters are definitions at block entry, just like phi values. */
    for (size_t pi = 0; pi < program->parameter_count; ++pi) {
        const IRBlockParameter *parameter = &program->parameters[pi];
        size_t bi = ir_find_block_index(program, parameter->block_id);
        if (bi == SIZE_MAX || parameter->type <= IR_TYPE_INVALID ||
            parameter->type >= IR_TYPE_VOID)
            IR_REJECT("invalid block parameter at index %zu", pi);
        if (bi == 0)
            IR_REJECT("entry block parameters require function-signature support");
        if (!ir_add_validated_value(values, &value_count, value_capacity,
                                    parameter->value_id, parameter->type, bi, 0,
                                    true, error, error_capacity)) {
            free(values); free(reachable); free(queue); free(dominators);
            return false;
        }
        for (size_t prior = 0; prior < pi; ++prior)
            if (program->parameters[prior].block_id == parameter->block_id &&
                program->parameters[prior].value_id == parameter->value_id)
                IR_REJECT("duplicate block parameter id %u", parameter->value_id);
    }

    /* Check block-local instruction shape and collect every SSA definition
       before checking uses, so forward/non-dominating references fail closed. */
    for (size_t bi = 0; bi < n; ++bi) {
        const IRBasicBlock *block = &program->blocks[bi];
        size_t end = block->first_instruction + block->instruction_count;
        for (size_t ii = block->first_instruction; ii < end; ++ii) {
            const IRInstruction *ins = &program->instructions[ii];
            bool last = ii + 1 == end;
            bool terminator = ins->opcode == IR_BRANCH ||
                              ins->opcode == IR_COND_BRANCH ||
                              ins->opcode == IR_RETURN;
            bool defines = false;
            if (ins->block_id != block->id || ins->arg1 || ins->arg2 || ins->arg3 ||
                ins->opcode < IR_CONST_I64 || ins->opcode >= IR_OPCODE_COUNT)
                IR_REJECT("unsupported or malformed opcode/instruction at %zu", ii);
            switch (ins->opcode) {
                case IR_CONST_I64:
                    defines = true;
                    if (ins->result_type != IR_TYPE_I64 || ins->operand1_id ||
                        ins->operand2_id || ins->target_true || ins->target_false ||
                        ins->float_immediate != 0.0) goto bad_shape;
                    break;
                case IR_CONST_F64:
                    defines = true;
                    if (ins->result_type != IR_TYPE_F64 || ins->operand1_id ||
                        ins->operand2_id || ins->target_true || ins->target_false ||
                        ins->integer_immediate != 0 || !isfinite(ins->float_immediate))
                        goto bad_shape;
                    break;
                case IR_ADD_I64:
                case IR_ADD_F64:
                    defines = true;
                    if (ins->result_type != (ins->opcode == IR_ADD_I64 ?
                                             IR_TYPE_I64 : IR_TYPE_F64) ||
                        ins->operand1_id == 0 || ins->operand2_id == 0 ||
                        ins->integer_immediate || ins->float_immediate != 0.0 ||
                        ins->target_true || ins->target_false) goto bad_shape;
                    break;
                case IR_EQ_I64:
                    defines = true;
                    if (ins->result_type != IR_TYPE_BOOL || !ins->operand1_id ||
                        !ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || ins->target_true ||
                        ins->target_false) goto bad_shape;
                    break;
                case IR_BRANCH:
                    if (!last || ins->result_id || ins->result_type != IR_TYPE_VOID ||
                        ins->operand1_id || ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || !ins->target_true ||
                        ins->target_false) goto bad_shape;
                    break;
                case IR_COND_BRANCH:
                    if (!last || ins->result_id || ins->result_type != IR_TYPE_VOID ||
                        !ins->operand1_id || ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || !ins->target_true ||
                        !ins->target_false) goto bad_shape;
                    break;
                case IR_RETURN:
                    if (!last || ins->result_id || ins->operand2_id ||
                        ins->integer_immediate || ins->float_immediate != 0.0 ||
                        ins->target_true || ins->target_false ||
                        ins->result_type <= IR_TYPE_INVALID ||
                        ins->result_type > IR_TYPE_VOID ||
                        (!ins->operand1_id && ins->result_type != IR_TYPE_VOID) ||
                        (ins->operand1_id && ins->result_type == IR_TYPE_VOID))
                        goto bad_shape;
                    break;
                default:
                    IR_REJECT("unsupported IR opcode %d", (int)ins->opcode);
            }
            if (last != terminator)
                IR_REJECT("block %u must end with exactly one terminator", block->id);
            if (defines && !ir_add_validated_value(values, &value_count,
                    value_capacity, ins->result_id, ins->result_type, bi, ii,
                    false, error, error_capacity)) {
                free(values); free(reachable); free(queue); free(dominators);
                return false;
            }
            continue;
        bad_shape:
            IR_REJECT("invalid operands, result type, or targets at instruction %zu", ii);
        }
        const IRInstruction *term = &program->instructions[end - 1];
        if (block->successor_true != term->target_true ||
            block->successor_false != term->target_false)
            IR_REJECT("block %u successor metadata disagrees with terminator", block->id);
    }

    /* Validate CFG targets, then require a single reachable region rooted at
       layout block zero. This makes dominance well-defined for every block. */
    for (size_t bi = 0; bi < n; ++bi) {
        const IRBasicBlock *block = &program->blocks[bi];
        if ((block->successor_true &&
             ir_find_block_index(program, block->successor_true) == SIZE_MAX) ||
            (block->successor_false &&
             ir_find_block_index(program, block->successor_false) == SIZE_MAX))
            IR_REJECT("block %u branches to a missing block", block->id);
    }
    size_t head = 0, tail = 0;
    reachable[0] = 1;
    queue[tail++] = 0;
    while (head < tail) {
        size_t bi = queue[head++];
        const IRBasicBlock *block = &program->blocks[bi];
        uint32_t targets[2] = {block->successor_true, block->successor_false};
        for (size_t k = 0; k < 2; ++k) {
            size_t ti;
            if (!targets[k] || (k == 1 && targets[1] == targets[0])) continue;
            ti = ir_find_block_index(program, targets[k]);
            if (!reachable[ti]) { reachable[ti] = 1; queue[tail++] = ti; }
        }
    }
    for (size_t bi = 0; bi < n; ++bi)
        if (!reachable[bi]) IR_REJECT("unreachable IR block %u", program->blocks[bi].id);

    /* Iterative classical dominator solution for the explicitly represented
       CFG; entry dominates itself, every other block starts with all blocks. */
    dominators[0] = 1;
    for (size_t bi = 1; bi < n; ++bi)
        memset(&dominators[bi * n], 1, n);
    do {
        changed = false;
        for (size_t bi = 1; bi < n; ++bi) {
            unsigned char *row = &dominators[bi * n];
            unsigned char *next = (unsigned char *)calloc(n, sizeof(*next));
            bool first = true;
            if (!next) IR_REJECT("out of memory solving CFG dominators");
            for (size_t pi = 0; pi < n; ++pi) {
                const IRBasicBlock *pred = &program->blocks[pi];
                if (pred->successor_true != program->blocks[bi].id &&
                    pred->successor_false != program->blocks[bi].id) continue;
                if (first) { memcpy(next, &dominators[pi * n], n); first = false; }
                else for (size_t k = 0; k < n; ++k)
                    next[k] = (unsigned char)(next[k] && dominators[pi * n + k]);
            }
            if (first) {
                free(next);
                IR_REJECT("reachable block %u has no predecessor", program->blocks[bi].id);
            }
            next[bi] = 1;
            if (memcmp(row, next, n) != 0) {
                memcpy(row, next, n);
                changed = true;
            }
            free(next);
        }
    } while (changed);

    /* Check ordinary operand typing and SSA availability at the exact use. */
    for (size_t bi = 0; bi < n; ++bi) {
        const IRBasicBlock *block = &program->blocks[bi];
        size_t end = block->first_instruction + block->instruction_count;
        for (size_t ii = block->first_instruction; ii < end; ++ii) {
            const IRInstruction *ins = &program->instructions[ii];
            IRType operand_type = IR_TYPE_INVALID;
            switch (ins->opcode) {
                case IR_ADD_I64: operand_type = IR_TYPE_I64; break;
                case IR_ADD_F64: operand_type = IR_TYPE_F64; break;
                case IR_EQ_I64: operand_type = IR_TYPE_I64; break;
                case IR_COND_BRANCH: operand_type = IR_TYPE_BOOL; break;
                case IR_RETURN:
                    if (ins->operand1_id) operand_type = ins->result_type;
                    break;
                default: break;
            }
            if (operand_type != IR_TYPE_INVALID &&
                !ir_use_is_valid(values, value_count, ins->operand1_id,
                    operand_type, bi, ii, n, dominators, error, error_capacity)) {
                free(values); free(reachable); free(queue); free(dominators);
                return false;
            }
            if ((ins->opcode == IR_ADD_I64 || ins->opcode == IR_ADD_F64 ||
                 ins->opcode == IR_EQ_I64) &&
                !ir_use_is_valid(values, value_count, ins->operand2_id,
                    ins->opcode == IR_ADD_I64 ? IR_TYPE_I64 :
                    ins->opcode == IR_ADD_F64 ? IR_TYPE_F64 : IR_TYPE_I64,
                    bi, ii, n, dominators, error, error_capacity)) {
                free(values); free(reachable); free(queue); free(dominators);
                return false;
            }
        }
    }

    /* Every CFG edge carries exactly one correctly typed value for each target
       parameter; each incoming value must be available at its predecessor end. */
    for (size_t ai = 0; ai < program->edge_argument_count; ++ai) {
        const IREdgeArgument *arg = &program->edge_arguments[ai];
        size_t si = ir_find_block_index(program, arg->source_block_id);
        size_t ti = ir_find_block_index(program, arg->target_block_id);
        const IRBasicBlock *source;
        const IRBlockParameter *parameter;
        bool edge_exists;
        if (si == SIZE_MAX || ti == SIZE_MAX)
            IR_REJECT("edge argument references a missing block");
        source = &program->blocks[si];
        edge_exists = source->successor_true == arg->target_block_id ||
                      source->successor_false == arg->target_block_id;
        if (!edge_exists)
            IR_REJECT("edge argument does not correspond to a CFG edge");
        parameter = ir_nth_parameter(program, arg->target_block_id,
                                     arg->parameter_index);
        if (!parameter)
            IR_REJECT("edge argument has an invalid target parameter index");
        for (size_t prior = 0; prior < ai; ++prior)
            if (program->edge_arguments[prior].source_block_id == arg->source_block_id &&
                program->edge_arguments[prior].target_block_id == arg->target_block_id &&
                program->edge_arguments[prior].parameter_index == arg->parameter_index)
                IR_REJECT("duplicate argument for CFG edge parameter");
        size_t term_index = source->first_instruction + source->instruction_count - 1;
        if (!ir_use_is_valid(values, value_count, arg->value_id, parameter->type,
                             si, term_index, n, dominators, error, error_capacity)) {
            free(values); free(reachable); free(queue); free(dominators);
            return false;
        }
    }
    for (size_t si = 0; si < n; ++si) {
        const IRBasicBlock *source = &program->blocks[si];
        uint32_t targets[2] = {source->successor_true, source->successor_false};
        for (size_t k = 0; k < 2; ++k) {
            if (!targets[k] || (k == 1 && targets[1] == targets[0])) continue;
            const IRBasicBlock *target =
                &program->blocks[ir_find_block_index(program, targets[k])];
            uint32_t param_index = 0;
            for (;;) {
                const IRBlockParameter *parameter =
                    ir_nth_parameter(program, target->id, param_index);
                if (!parameter) break;
                size_t matches = 0;
                for (size_t ai = 0; ai < program->edge_argument_count; ++ai)
                    if (program->edge_arguments[ai].source_block_id == source->id &&
                        program->edge_arguments[ai].target_block_id == target->id &&
                        program->edge_arguments[ai].parameter_index == param_index)
                        ++matches;
                if (matches != 1)
                    IR_REJECT("CFG edge %u -> %u must supply each block parameter exactly once",
                              source->id, target->id);
                ++param_index;
            }
        }
    }

    free(values); free(reachable); free(queue); free(dominators);
#undef IR_REJECT
    return true;
}
