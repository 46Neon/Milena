#include "canonical_compiler.h"
#include "typed_ir.h"
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

MilenaIRProgram *milena_ir_program_create(void) {
    return (MilenaIRProgram *)calloc(1, sizeof(MilenaIRProgram));
}

void milena_ir_program_destroy(MilenaIRProgram *program) {
    if (!program) return;
    free(program->instructions);
    free(program->blocks);
    free(program->parameters);
    free(program->edge_arguments);
    free(program->signature.parameter_types);
    free(program);
}

bool milena_ir_program_set_function_signature(MilenaIRProgram *program,
                                               const MilenaIRType *parameter_types,
                                               size_t parameter_count,
                                               MilenaIRType return_type) {
    MilenaIRType *copy = NULL;
    if (!program || program->has_function_signature ||
        program->instructions || program->blocks || program->parameters ||
        program->edge_arguments || program->count || program->block_count ||
        program->parameter_count || program->edge_argument_count ||
        program->signature.parameter_types || program->signature.parameter_count ||
        program->signature.return_type != MILENA_IR_TYPE_INVALID ||
        (parameter_count && !parameter_types) ||
        parameter_count > SIZE_MAX / sizeof(*copy) ||
        return_type <= MILENA_IR_TYPE_INVALID ||
        return_type >= MILENA_IR_TYPE_VOID)
        return false;
    for (size_t i = 0; i < parameter_count; ++i)
        if (parameter_types[i] <= MILENA_IR_TYPE_INVALID ||
            parameter_types[i] >= MILENA_IR_TYPE_VOID)
            return false;
    if (parameter_count) {
        copy = (MilenaIRType *)malloc(parameter_count * sizeof(*copy));
        if (!copy) return false;
        memcpy(copy, parameter_types, parameter_count * sizeof(*copy));
    }
    program->signature.parameter_types = copy;
    program->signature.parameter_count = parameter_count;
    program->signature.return_type = return_type;
    program->has_function_signature = true;
    return true;
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

static bool ir_reserve_instructions(MilenaIRProgram *program) {
    MilenaIRInstruction *grown;
    size_t capacity;
    if (program->count < program->capacity) return true;
    capacity = program->capacity == 0 ? 8 : program->capacity * 2;
    if (capacity < program->capacity || capacity > SIZE_MAX / sizeof(*grown)) return false;
    grown = (MilenaIRInstruction *)realloc(program->instructions, capacity * sizeof(*grown));
    if (!grown) return false;
    program->instructions = grown;
    program->capacity = capacity;
    return true;
}

bool milena_ir_program_add_block(MilenaIRProgram *program, uint32_t block_id) {
    MilenaIRBasicBlock *grown;
    size_t capacity;
    if (!program || block_id == 0) return false;
    if (program->block_count > 0 && !program->blocks[program->block_count - 1].terminated) return false;
    for (size_t i = 0; i < program->block_count; ++i) {
        if (program->blocks[i].id == block_id) return false;
    }
    if (program->block_count == program->block_capacity) {
        capacity = program->block_capacity == 0 ? 4 : program->block_capacity * 2;
        if (capacity < program->block_capacity || capacity > SIZE_MAX / sizeof(*grown)) return false;
        grown = (MilenaIRBasicBlock *)realloc(program->blocks, capacity * sizeof(*grown));
        if (!grown) return false;
        program->blocks = grown;
        program->block_capacity = capacity;
    }
    MilenaIRBasicBlock *block = &program->blocks[program->block_count++];
    memset(block, 0, sizeof(*block));
    block->id = block_id;
    block->first_instruction = program->count;
    return true;
}

bool milena_ir_program_add_block_parameter(MilenaIRProgram *program, uint32_t block_id,
                                    uint32_t value_id, MilenaIRType type) {
    MilenaIRBlockParameter *grown;
    size_t capacity;
    MilenaIRBasicBlock *block;
    if (!program || program->block_count == 0 || value_id == 0 ||
        type <= MILENA_IR_TYPE_INVALID || type >= MILENA_IR_TYPE_VOID) return false;
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
        grown = (MilenaIRBlockParameter *)realloc(program->parameters,
                                             capacity * sizeof(*grown));
        if (!grown) return false;
        program->parameters = grown;
        program->parameter_capacity = capacity;
    }
    program->parameters[program->parameter_count++] =
        (MilenaIRBlockParameter){block_id, value_id, type};
    return true;
}

bool milena_ir_block_add_edge_argument(MilenaIRProgram *program, uint32_t source_block_id,
                                uint32_t target_block_id, uint32_t parameter_index,
                                uint32_t value_id) {
    MilenaIREdgeArgument *grown;
    size_t capacity;
    if (!program || source_block_id == 0 || target_block_id == 0 || value_id == 0)
        return false;
    if (program->edge_argument_count == program->edge_argument_capacity) {
        capacity = program->edge_argument_capacity == 0 ? 8 :
                   program->edge_argument_capacity * 2;
        if (capacity < program->edge_argument_capacity ||
            capacity > SIZE_MAX / sizeof(*grown)) return false;
        grown = (MilenaIREdgeArgument *)realloc(program->edge_arguments,
                                           capacity * sizeof(*grown));
        if (!grown) return false;
        program->edge_arguments = grown;
        program->edge_argument_capacity = capacity;
    }
    program->edge_arguments[program->edge_argument_count++] =
        (MilenaIREdgeArgument){source_block_id, target_block_id, parameter_index, value_id};
    return true;
}

bool milena_ir_block_append_instruction(MilenaIRProgram *program, uint32_t block_id,
                                 MilenaIROpCode opcode, uint32_t result_id,
                                 MilenaIRType result_type, uint32_t operand1_id,
                                 uint32_t operand2_id, int64_t integer_immediate,
                                 double float_immediate, uint32_t target_true,
                                 uint32_t target_false) {
    MilenaIRBasicBlock *block;
    MilenaIRInstruction *instruction;
    bool terminator;
    if (!program || program->block_count == 0) return false;
    block = &program->blocks[program->block_count - 1];
    if (block->id != block_id || block->terminated ||
        block->first_instruction + block->instruction_count != program->count ||
        !ir_reserve_instructions(program)) return false;
    terminator = opcode == MILENA_IR_BRANCH || opcode == MILENA_IR_COND_BRANCH || opcode == MILENA_IR_RETURN;
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
    MilenaIRType type;
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

static size_t ir_find_block_index(const MilenaIRProgram *program, uint32_t id) {
    for (size_t i = 0; i < program->block_count; ++i)
        if (program->blocks[i].id == id) return i;
    return SIZE_MAX;
}

static const MilenaIRBlockParameter *ir_nth_parameter(const MilenaIRProgram *program,
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
                                   size_t capacity, uint32_t id, MilenaIRType type,
                                   size_t block_index, size_t instruction_index,
                                   bool parameter, char *error,
                                   size_t error_capacity) {
    if (id == 0 || type <= MILENA_IR_TYPE_INVALID || type >= MILENA_IR_TYPE_VOID ||
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
                            uint32_t id, MilenaIRType expected_type, size_t use_block,
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

bool milena_ir_program_validate(const MilenaIRProgram *program, char *error,
                         size_t error_capacity) {
    IRValidatedValue *values = NULL;
    unsigned char *reachable = NULL;
    unsigned char *dominators = NULL;
    size_t *queue = NULL;
    size_t value_count = 0;
    size_t value_capacity;
    size_t expected_first = 0;
    size_t entry_parameter_count = 0;
    size_t n;
    bool changed;
    if (error && error_capacity > 0) error[0] = '\0';
    if (!program || !program->instructions || !program->blocks ||
        program->count == 0 || program->block_count == 0 ||
        (program->parameter_count && !program->parameters) ||
        (program->edge_argument_count && !program->edge_arguments))
        return ir_fail(error, error_capacity,
                       "typed IR must contain valid instructions and blocks");
    if (program->has_function_signature) {
        if ((program->signature.parameter_count &&
             !program->signature.parameter_types) ||
            (!program->signature.parameter_count &&
             program->signature.parameter_types) ||
            program->signature.return_type <= MILENA_IR_TYPE_INVALID ||
            program->signature.return_type >= MILENA_IR_TYPE_VOID)
            return ir_fail(error, error_capacity,
                           "invalid typed IR function signature");
        for (size_t i = 0; i < program->signature.parameter_count; ++i)
            if (program->signature.parameter_types[i] <= MILENA_IR_TYPE_INVALID ||
                program->signature.parameter_types[i] >= MILENA_IR_TYPE_VOID)
                return ir_fail(error, error_capacity,
                               "invalid function parameter type at index %zu", i);
    } else if (program->signature.parameter_types ||
               program->signature.parameter_count ||
               program->signature.return_type != MILENA_IR_TYPE_INVALID) {
        return ir_fail(error, error_capacity,
                       "function signature data is present without a signature");
    }
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
        const MilenaIRBasicBlock *block = &program->blocks[bi];
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
        const MilenaIRBlockParameter *parameter = &program->parameters[pi];
        size_t bi = ir_find_block_index(program, parameter->block_id);
        if (bi == SIZE_MAX || parameter->type <= MILENA_IR_TYPE_INVALID ||
            parameter->type >= MILENA_IR_TYPE_VOID)
            IR_REJECT("invalid block parameter at index %zu", pi);
        if (bi == 0) {
            if (!program->has_function_signature ||
                entry_parameter_count >= program->signature.parameter_count ||
                parameter->type !=
                    program->signature.parameter_types[entry_parameter_count])
                IR_REJECT("entry block parameter does not match the function signature");
            ++entry_parameter_count;
        }
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
    if ((program->has_function_signature &&
         entry_parameter_count != program->signature.parameter_count) ||
        (!program->has_function_signature && entry_parameter_count != 0))
        IR_REJECT("entry block parameter count does not match the function signature");

    /* Check block-local instruction shape and collect every SSA definition
       before checking uses, so forward/non-dominating references fail closed. */
    for (size_t bi = 0; bi < n; ++bi) {
        const MilenaIRBasicBlock *block = &program->blocks[bi];
        size_t end = block->first_instruction + block->instruction_count;
        for (size_t ii = block->first_instruction; ii < end; ++ii) {
            const MilenaIRInstruction *ins = &program->instructions[ii];
            bool last = ii + 1 == end;
            bool terminator = ins->opcode == MILENA_IR_BRANCH ||
                              ins->opcode == MILENA_IR_COND_BRANCH ||
                              ins->opcode == MILENA_IR_RETURN;
            bool defines = false;
            if (ins->block_id != block->id ||
                ins->opcode < MILENA_IR_CONST_I64 || ins->opcode >= MILENA_IR_OPCODE_COUNT)
                IR_REJECT("unsupported or malformed opcode/instruction at %zu", ii);
            switch (ins->opcode) {
                case MILENA_IR_CONST_I64:
                    defines = true;
                    if (ins->result_type != MILENA_IR_TYPE_I64 || ins->operand1_id ||
                        ins->operand2_id || ins->target_true || ins->target_false ||
                        ins->float_immediate != 0.0) goto bad_shape;
                    break;
                case MILENA_IR_CONST_F64:
                    defines = true;
                    if (ins->result_type != MILENA_IR_TYPE_F64 || ins->operand1_id ||
                        ins->operand2_id || ins->target_true || ins->target_false ||
                        ins->integer_immediate != 0 || !isfinite(ins->float_immediate))
                        goto bad_shape;
                    break;
                case MILENA_IR_CONST_BOOL:
                    defines = true;
                    if (ins->result_type != MILENA_IR_TYPE_BOOL || ins->operand1_id ||
                        ins->operand2_id || ins->target_true || ins->target_false ||
                        (ins->integer_immediate != 0 && ins->integer_immediate != 1) ||
                        ins->float_immediate != 0.0) goto bad_shape;
                    break;
                case MILENA_IR_ADD_I64:
                    defines = true;
                    if (ins->result_type != MILENA_IR_TYPE_I64 || !ins->operand1_id ||
                        !ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || ins->target_true ||
                        ins->target_false) goto bad_shape;
                    break;
                case MILENA_IR_ADD_F64:
                case MILENA_IR_SUB_F64:
                case MILENA_IR_MUL_F64:
                case MILENA_IR_DIV_F64:
                    defines = true;
                    if (ins->result_type != MILENA_IR_TYPE_F64 || !ins->operand1_id ||
                        !ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || ins->target_true ||
                        ins->target_false) goto bad_shape;
                    break;
                case MILENA_IR_EQ_I64:
                case MILENA_IR_EQ_F64:
                case MILENA_IR_NE_F64:
                case MILENA_IR_LT_F64:
                case MILENA_IR_LE_F64:
                case MILENA_IR_GT_F64:
                case MILENA_IR_GE_F64:
                    defines = true;
                    if (ins->result_type != MILENA_IR_TYPE_BOOL || !ins->operand1_id ||
                        !ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || ins->target_true ||
                        ins->target_false) goto bad_shape;
                    break;
                case MILENA_IR_BRANCH:
                    if (!last || ins->result_id || ins->result_type != MILENA_IR_TYPE_VOID ||
                        ins->operand1_id || ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || !ins->target_true ||
                        ins->target_false) goto bad_shape;
                    break;
                case MILENA_IR_COND_BRANCH:
                    if (!last || ins->result_id || ins->result_type != MILENA_IR_TYPE_VOID ||
                        !ins->operand1_id || ins->operand2_id || ins->integer_immediate ||
                        ins->float_immediate != 0.0 || !ins->target_true ||
                        !ins->target_false) goto bad_shape;
                    break;
                case MILENA_IR_RETURN:
                    if (!last || ins->result_id || ins->operand2_id ||
                        ins->integer_immediate || ins->float_immediate != 0.0 ||
                        ins->target_true || ins->target_false ||
                        ins->result_type <= MILENA_IR_TYPE_INVALID ||
                        ins->result_type > MILENA_IR_TYPE_VOID ||
                        (!ins->operand1_id && ins->result_type != MILENA_IR_TYPE_VOID) ||
                        (ins->operand1_id && ins->result_type == MILENA_IR_TYPE_VOID) ||
                        (program->has_function_signature &&
                         ins->result_type != program->signature.return_type))
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
        const MilenaIRInstruction *term = &program->instructions[end - 1];
        if (block->successor_true != term->target_true ||
            block->successor_false != term->target_false)
            IR_REJECT("block %u successor metadata disagrees with terminator", block->id);
    }

    /* Validate CFG targets, then require a single reachable region rooted at
       layout block zero. This makes dominance well-defined for every block. */
    for (size_t bi = 0; bi < n; ++bi) {
        const MilenaIRBasicBlock *block = &program->blocks[bi];
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
        const MilenaIRBasicBlock *block = &program->blocks[bi];
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
                const MilenaIRBasicBlock *pred = &program->blocks[pi];
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
        const MilenaIRBasicBlock *block = &program->blocks[bi];
        size_t end = block->first_instruction + block->instruction_count;
        for (size_t ii = block->first_instruction; ii < end; ++ii) {
            const MilenaIRInstruction *ins = &program->instructions[ii];
            MilenaIRType operand_type = MILENA_IR_TYPE_INVALID;
            switch (ins->opcode) {
                case MILENA_IR_ADD_I64:
                case MILENA_IR_EQ_I64:
                    operand_type = MILENA_IR_TYPE_I64;
                    break;
                case MILENA_IR_ADD_F64:
                case MILENA_IR_SUB_F64:
                case MILENA_IR_MUL_F64:
                case MILENA_IR_DIV_F64:
                case MILENA_IR_EQ_F64:
                case MILENA_IR_NE_F64:
                case MILENA_IR_LT_F64:
                case MILENA_IR_LE_F64:
                case MILENA_IR_GT_F64:
                case MILENA_IR_GE_F64:
                    operand_type = MILENA_IR_TYPE_F64;
                    break;
                case MILENA_IR_COND_BRANCH: operand_type = MILENA_IR_TYPE_BOOL; break;
                case MILENA_IR_RETURN:
                    if (ins->operand1_id) operand_type = ins->result_type;
                    break;
                default: break;
            }
            if (operand_type != MILENA_IR_TYPE_INVALID &&
                !ir_use_is_valid(values, value_count, ins->operand1_id,
                    operand_type, bi, ii, n, dominators, error, error_capacity)) {
                free(values); free(reachable); free(queue); free(dominators);
                return false;
            }
            if ((ins->opcode == MILENA_IR_ADD_I64 || ins->opcode == MILENA_IR_EQ_I64 ||
                 ins->opcode == MILENA_IR_ADD_F64 || ins->opcode == MILENA_IR_SUB_F64 ||
                 ins->opcode == MILENA_IR_MUL_F64 || ins->opcode == MILENA_IR_DIV_F64 ||
                 ins->opcode == MILENA_IR_EQ_F64 || ins->opcode == MILENA_IR_NE_F64 ||
                 ins->opcode == MILENA_IR_LT_F64 || ins->opcode == MILENA_IR_LE_F64 ||
                 ins->opcode == MILENA_IR_GT_F64 || ins->opcode == MILENA_IR_GE_F64) &&
                !ir_use_is_valid(values, value_count, ins->operand2_id,
                    (ins->opcode == MILENA_IR_ADD_I64 || ins->opcode == MILENA_IR_EQ_I64) ?
                        MILENA_IR_TYPE_I64 : MILENA_IR_TYPE_F64,
                    bi, ii, n, dominators, error, error_capacity)) {
                free(values); free(reachable); free(queue); free(dominators);
                return false;
            }
        }
    }

    /* Every CFG edge carries exactly one correctly typed value for each target
       parameter; each incoming value must be available at its predecessor end. */
    for (size_t ai = 0; ai < program->edge_argument_count; ++ai) {
        const MilenaIREdgeArgument *arg = &program->edge_arguments[ai];
        size_t si = ir_find_block_index(program, arg->source_block_id);
        size_t ti = ir_find_block_index(program, arg->target_block_id);
        const MilenaIRBasicBlock *source;
        const MilenaIRBlockParameter *parameter;
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
        const MilenaIRBasicBlock *source = &program->blocks[si];
        uint32_t targets[2] = {source->successor_true, source->successor_false};
        for (size_t k = 0; k < 2; ++k) {
            if (!targets[k] || (k == 1 && targets[1] == targets[0])) continue;
            const MilenaIRBasicBlock *target =
                &program->blocks[ir_find_block_index(program, targets[k])];
            uint32_t param_index = 0;
            for (;;) {
                const MilenaIRBlockParameter *parameter =
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

typedef struct {
    size_t symbol_id;
    uint32_t value_id;
    MilenaIRType type;
} IRScalarBinding;

static bool ir_hir_value_type(MilenaHIRValueType source, MilenaIRType *target) {
    if (!target) return false;
    switch (source) {
        case MILENA_HIR_NUMBER: *target = MILENA_IR_TYPE_F64; return true;
        case MILENA_HIR_BOOLEAN: *target = MILENA_IR_TYPE_BOOL; return true;
        default: *target = MILENA_IR_TYPE_INVALID; return false;
    }
}

static size_t ir_scalar_binding_index(const IRScalarBinding *bindings,
                                      size_t count, size_t symbol_id) {
    if (!symbol_id) return SIZE_MAX;
    for (size_t i = 0; i < count; ++i)
        if (bindings[i].symbol_id == symbol_id) return i;
    return SIZE_MAX;
}

static bool ir_scalar_next_value(uint32_t *next_value, uint32_t *value_id,
                                char *error, size_t error_capacity) {
    if (!next_value || !value_id || *next_value == 0 ||
        *next_value == UINT32_MAX)
        return ir_fail(error, error_capacity, "typed IR value-id space exhausted");
    *value_id = (*next_value)++;
    return true;
}

static bool ir_scalar_append_value(MilenaIRProgram *program, uint32_t block_id,
                                   MilenaIROpCode opcode, MilenaIRType type,
                                   uint32_t left, uint32_t right,
                                   int64_t integer_immediate,
                                   double float_immediate,
                                   uint32_t *next_value, uint32_t *result,
                                   char *error, size_t error_capacity) {
    if (!ir_scalar_next_value(next_value, result, error, error_capacity))
        return false;
    if (!milena_ir_block_append_instruction(program, block_id, opcode, *result, type,
                                     left, right, integer_immediate,
                                     float_immediate, 0, 0))
        return ir_fail(error, error_capacity,
                       "could not append typed scalar IR instruction");
    return true;
}

static bool ir_scalar_lower_expression(MilenaIRProgram *program, uint32_t block_id,
                                       const MilenaHIRExpression *expression,
                                       const IRScalarBinding *bindings,
                                       size_t binding_count, uint32_t *next_value,
                                       uint32_t *result, MilenaIRType *result_type,
                                       char *error, size_t error_capacity) {
    if (!program || !expression || !next_value || !result || !result_type)
        return ir_fail(error, error_capacity, "invalid scalar HIR expression");
    if (!ir_hir_value_type(expression->value_type, result_type))
        return ir_fail(error, error_capacity, "unsupported scalar HIR value type");
    switch (expression->kind) {
        case MILENA_HIR_EXPR_LITERAL:
            if (*result_type == MILENA_IR_TYPE_F64) {
                if (!isfinite(expression->as.number))
                    return ir_fail(error, error_capacity,
                                   "non-finite scalar literal is not supported");
                return ir_scalar_append_value(program, block_id, MILENA_IR_CONST_F64, MILENA_IR_TYPE_F64,
                    0, 0, 0, expression->as.number, next_value, result,
                    error, error_capacity);
            }
            return ir_scalar_append_value(program, block_id, MILENA_IR_CONST_BOOL, MILENA_IR_TYPE_BOOL,
                0, 0, expression->as.boolean ? 1 : 0, 0.0, next_value, result,
                error, error_capacity);
        case MILENA_HIR_EXPR_VARIABLE: {
            size_t index = ir_scalar_binding_index(bindings, binding_count,
                                                    expression->resolved_symbol_id);
            if (index == SIZE_MAX)
                return ir_fail(error, error_capacity,
                               "scalar variable has no dominating typed binding");
            if (bindings[index].type != *result_type)
                return ir_fail(error, error_capacity,
                               "scalar variable binding type does not match use");
            *result = bindings[index].value_id;
            return true;
        }
        case MILENA_HIR_EXPR_BINARY: {
            const MilenaHIRExpression *left = expression->as.binary.left;
            const MilenaHIRExpression *right = expression->as.binary.right;
            uint32_t left_id, right_id;
            MilenaIRType left_type, right_type;
            MilenaIROpCode opcode;
            bool comparison = false;
            if (!left || !right ||
                !ir_scalar_lower_expression(program, block_id, left, bindings,
                    binding_count, next_value, &left_id, &left_type, error,
                    error_capacity) ||
                !ir_scalar_lower_expression(program, block_id, right, bindings,
                    binding_count,
                    next_value, &right_id, &right_type, error, error_capacity))
                return false;
            if (left_type != MILENA_IR_TYPE_F64 || right_type != MILENA_IR_TYPE_F64)
                return ir_fail(error, error_capacity,
                               "typed scalar binary operands must be numeric");
            switch (expression->as.binary.operation) {
                case AST_OPERATOR_ADD: opcode = MILENA_IR_ADD_F64; break;
                case AST_OPERATOR_SUBTRACT: opcode = MILENA_IR_SUB_F64; break;
                case AST_OPERATOR_MULTIPLY: opcode = MILENA_IR_MUL_F64; break;
                case AST_OPERATOR_DIVIDE: opcode = MILENA_IR_DIV_F64; break;
                case AST_OPERATOR_EQUAL: opcode = MILENA_IR_EQ_F64; comparison = true; break;
                case AST_OPERATOR_NOT_EQUAL: opcode = MILENA_IR_NE_F64; comparison = true; break;
                case AST_OPERATOR_LESS: opcode = MILENA_IR_LT_F64; comparison = true; break;
                case AST_OPERATOR_LESS_EQUAL: opcode = MILENA_IR_LE_F64; comparison = true; break;
                case AST_OPERATOR_GREATER: opcode = MILENA_IR_GT_F64; comparison = true; break;
                case AST_OPERATOR_GREATER_EQUAL: opcode = MILENA_IR_GE_F64; comparison = true; break;
                default:
                    return ir_fail(error, error_capacity,
                                   "unsupported typed scalar binary operator");
            }
            if ((comparison && *result_type != MILENA_IR_TYPE_BOOL) ||
                (!comparison && *result_type != MILENA_IR_TYPE_F64))
                return ir_fail(error, error_capacity,
                               "scalar HIR operator result type is inconsistent");
            return ir_scalar_append_value(program, block_id, opcode,
                *result_type, left_id, right_id, 0, 0.0, next_value, result,
                error, error_capacity);
        }
        case MILENA_HIR_EXPR_CALL:
            return ir_fail(error, error_capacity,
                           "function calls are not in the typed scalar IR slice yet");
        default:
            return ir_fail(error, error_capacity,
                           "unsupported typed scalar HIR expression kind");
    }
}

static bool ir_scalar_lower_return_branch(
    MilenaIRProgram *program, uint32_t block_id,
    MilenaHIRStatement *const *statements, size_t statement_count,
    const IRScalarBinding *bindings, size_t binding_count, uint32_t *next_value,
    char *error, size_t error_capacity) {
    uint32_t value_id;
    MilenaIRType value_type;
    const MilenaHIRStatement *statement;
    if (!statements || statement_count != 1 || !statements[0] ||
        statements[0]->kind != MILENA_HIR_STMT_RETURN ||
        !statements[0]->as.expression)
        return ir_fail(error, error_capacity,
                       "typed scalar conditional arms must each contain one return");
    statement = statements[0];
    if (!ir_scalar_lower_expression(program, block_id, statement->as.expression,
            bindings, binding_count, next_value, &value_id, &value_type,
            error, error_capacity)) return false;
    if (!milena_ir_block_append_instruction(program, block_id, MILENA_IR_RETURN, 0,
            value_type, value_id, 0, 0, 0.0, 0, 0))
        return ir_fail(error, error_capacity,
                       "could not append conditional-arm return");
    return true;
}

static bool ir_scalar_lower_assignment_sequence(
    MilenaIRProgram *program, uint32_t *current_block_id,
    uint32_t *next_block_id, uint32_t *next_value,
    const MilenaHIRStatement *const *statements, size_t statement_count,
    IRScalarBinding *bindings, size_t binding_count, unsigned depth,
    char *error, size_t error_capacity) {
    if (depth > 128)
        return ir_fail(error, error_capacity,
                       "typed scalar conditional nesting exceeds the lowering limit");
    for (size_t i = 0; i < statement_count; ++i) {
        const MilenaHIRStatement *statement = statements ? statements[i] : NULL;
        if (!statement)
            return ir_fail(error, error_capacity,
                           "typed scalar conditional contains a missing statement");
        if (statement->kind == MILENA_HIR_STMT_ASSIGN) {
            size_t index;
            uint32_t value_id;
            MilenaIRType expression_type, statement_type;
            if (!statement->resolved_symbol_id || !statement->as.expression ||
                !ir_hir_value_type(statement->value_type, &statement_type))
                return ir_fail(error, error_capacity,
                    "typed scalar si/sino merge supports only assignments to existing bindings");
            index = ir_scalar_binding_index(bindings, binding_count,
                                            statement->resolved_symbol_id);
            if (index == SIZE_MAX || bindings[index].type != statement_type)
                return ir_fail(error, error_capacity,
                    "typed scalar conditional assignment has no compatible outer binding");
            if (!ir_scalar_lower_expression(program, *current_block_id,
                    statement->as.expression, bindings, binding_count,
                    next_value, &value_id, &expression_type,
                    error, error_capacity)) return false;
            if (expression_type != statement_type)
                return ir_fail(error, error_capacity,
                    "typed scalar conditional assignment changes its binding type");
            bindings[index].value_id = value_id;
            continue;
        }
        if (statement->kind == MILENA_HIR_STMT_IF) {
            uint32_t condition_id, then_block_id, else_block_id, merge_block_id;
            uint32_t then_end_block_id, else_end_block_id;
            uint32_t nested_next_block_id;
            MilenaIRType condition_type;
            IRScalarBinding *then_bindings = NULL, *else_bindings = NULL;
            size_t parameter_index = 0;
            if (!statement->as.conditional.else_body ||
                statement->as.conditional.else_count == 0)
                return ir_fail(error, error_capacity,
                    "nested typed scalar si/sino merge requires an explicit nonempty sino branch");
            if (!statement->as.conditional.condition || !next_block_id ||
                *next_block_id == 0 || *next_block_id > UINT32_MAX - 3)
                return ir_fail(error, error_capacity,
                               "typed scalar block-id space exhausted");
            then_block_id = *next_block_id;
            else_block_id = then_block_id + 1;
            merge_block_id = then_block_id + 2;
            *next_block_id += 3;
            nested_next_block_id = *next_block_id;
            if (binding_count > SIZE_MAX / sizeof(*then_bindings))
                return ir_fail(error, error_capacity,
                               "too many scalar bindings in conditional");
            if (binding_count) {
                then_bindings = malloc(binding_count * sizeof(*then_bindings));
                else_bindings = malloc(binding_count * sizeof(*else_bindings));
                if (!then_bindings || !else_bindings) {
                    free(then_bindings); free(else_bindings);
                    return ir_fail(error, error_capacity,
                        "out of memory lowering nested scalar conditional assignments");
                }
                memcpy(then_bindings, bindings,
                       binding_count * sizeof(*then_bindings));
                memcpy(else_bindings, bindings,
                       binding_count * sizeof(*else_bindings));
            }
            if (!ir_scalar_lower_expression(program, *current_block_id,
                    statement->as.conditional.condition, bindings, binding_count,
                    next_value, &condition_id, &condition_type,
                    error, error_capacity)) {
                free(then_bindings); free(else_bindings);
                return false;
            }
            if (condition_type != MILENA_IR_TYPE_BOOL) {
                free(then_bindings); free(else_bindings);
                return ir_fail(error, error_capacity,
                               "scalar si condition must lower to BOOL");
            }
            if (!milena_ir_block_append_instruction(program, *current_block_id,
                    MILENA_IR_COND_BRANCH, 0, MILENA_IR_TYPE_VOID, condition_id,
                    0, 0, 0.0, then_block_id, else_block_id) ||
                !milena_ir_program_add_block(program, then_block_id)) {
                free(then_bindings); free(else_bindings);
                return ir_fail(error, error_capacity,
                    "could not create nested typed scalar conditional blocks");
            }
            then_end_block_id = then_block_id;
            if (!ir_scalar_lower_assignment_sequence(program,
                    &then_end_block_id, &nested_next_block_id, next_value,
                    (const MilenaHIRStatement *const *)statement->as.conditional.then_body,
                    statement->as.conditional.then_count, then_bindings,
                    binding_count, depth + 1, error, error_capacity) ||
                !milena_ir_block_append_instruction(program, then_end_block_id,
                    MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0.0,
                    merge_block_id, 0) ||
                !milena_ir_program_add_block(program, else_block_id)) {
                free(then_bindings); free(else_bindings);
                if (!error || !error[0])
                    ir_fail(error, error_capacity,
                            "could not terminate nested typed scalar then branch");
                return false;
            }
            else_end_block_id = else_block_id;
            if (!ir_scalar_lower_assignment_sequence(program,
                    &else_end_block_id, &nested_next_block_id, next_value,
                    (const MilenaHIRStatement *const *)statement->as.conditional.else_body,
                    statement->as.conditional.else_count, else_bindings,
                    binding_count, depth + 1, error, error_capacity) ||
                !milena_ir_block_append_instruction(program, else_end_block_id,
                    MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0.0,
                    merge_block_id, 0) ||
                !milena_ir_program_add_block(program, merge_block_id)) {
                free(then_bindings); free(else_bindings);
                if (!error || !error[0])
                    ir_fail(error, error_capacity,
                            "could not terminate nested typed scalar else branch");
                return false;
            }
            for (size_t binding_index = 0; binding_index < binding_count;
                 ++binding_index) {
                if (then_bindings[binding_index].value_id ==
                    else_bindings[binding_index].value_id) continue;
                uint32_t merged_value;
                if (!ir_scalar_next_value(next_value, &merged_value,
                        error, error_capacity) ||
                    !milena_ir_program_add_block_parameter(program,
                        merge_block_id, merged_value,
                        bindings[binding_index].type) ||
                    !milena_ir_block_add_edge_argument(program, then_end_block_id,
                        merge_block_id, (uint32_t)parameter_index,
                        then_bindings[binding_index].value_id) ||
                    !milena_ir_block_add_edge_argument(program, else_end_block_id,
                        merge_block_id, (uint32_t)parameter_index,
                        else_bindings[binding_index].value_id)) {
                    free(then_bindings); free(else_bindings);
                    return ir_fail(error, error_capacity,
                            "could not define nested typed scalar merge parameter");
                }
                bindings[binding_index].value_id = merged_value;
                ++parameter_index;
            }
            free(then_bindings); free(else_bindings);
            *current_block_id = merge_block_id;
            *next_block_id = nested_next_block_id;
            continue;
        }
        return ir_fail(error, error_capacity,
            "typed scalar conditional arms support assignments and nested si/sino only; declarations and returns are unsupported");
    }
    return true;
}


static bool ir_scalar_function_return_type(const MilenaHIRFunction *function,
                                           MilenaIRType *return_type,
                                           char *error, size_t error_capacity) {
    const MilenaHIRStatement *last;
    if (!function || !function->body || !function->body_count || !return_type)
        return ir_fail(error, error_capacity,
                       "scalar HIR function has no final return");
    last = function->body[function->body_count - 1];
    if (last && last->kind == MILENA_HIR_STMT_RETURN && last->as.expression)
        return ir_hir_value_type(last->as.expression->value_type, return_type) ||
               ir_fail(error, error_capacity,
                       "unsupported scalar HIR function return type");
    if (last && last->kind == MILENA_HIR_STMT_IF &&
        last->as.conditional.then_count == 1 &&
        last->as.conditional.else_count == 1 &&
        last->as.conditional.then_body && last->as.conditional.else_body &&
        last->as.conditional.then_body[0] &&
        last->as.conditional.else_body[0] &&
        last->as.conditional.then_body[0]->kind == MILENA_HIR_STMT_RETURN &&
        last->as.conditional.else_body[0]->kind == MILENA_HIR_STMT_RETURN &&
        last->as.conditional.then_body[0]->as.expression &&
        last->as.conditional.else_body[0]->as.expression) {
        MilenaIRType then_type, else_type;
        if (!ir_hir_value_type(
                last->as.conditional.then_body[0]->as.expression->value_type,
                &then_type) ||
            !ir_hir_value_type(
                last->as.conditional.else_body[0]->as.expression->value_type,
                &else_type) || then_type != else_type)
            return ir_fail(error, error_capacity,
                           "scalar conditional return types do not match");
        *return_type = then_type;
        return true;
    }
    return ir_fail(error, error_capacity,
                   "scalar HIR function requires one final return or returning si/sino");
}

bool milena_ir_program_lower_scalar_function_body(MilenaIRProgram *program,
                                           const MilenaHIRFunction *function,
                                           char *error, size_t error_capacity) {
    MilenaIRProgram *lowered = NULL;
    IRScalarBinding *bindings = NULL;
    MilenaIRType *parameter_types = NULL;
    MilenaIRType return_type = MILENA_IR_TYPE_INVALID;
    size_t binding_count = 0;
    size_t binding_capacity;
    uint32_t next_value = 1;
    uint32_t current_block_id = 1;
    bool saw_return = false;
    bool success = false;
    if (error && error_capacity) error[0] = '\0';
    if (!program || !function || !function->name || !function->resolved_symbol_id ||
        !function->body_count || !function->body ||
        (function->parameter_count && !function->parameters) ||
        function->parameter_count >= UINT32_MAX ||
        function->body_count > SIZE_MAX - function->parameter_count ||
        program->instructions || program->blocks || program->parameters ||
        program->edge_arguments || program->signature.parameter_types ||
        program->signature.parameter_count ||
        program->signature.return_type != MILENA_IR_TYPE_INVALID ||
        program->has_function_signature || program->count || program->capacity ||
        program->block_count || program->block_capacity || program->parameter_count ||
        program->parameter_capacity || program->edge_argument_count ||
        program->edge_argument_capacity)
        return ir_fail(error, error_capacity,
                       "scalar IR lowering requires a supported HIR function and fresh output");
    binding_capacity = function->body_count + function->parameter_count;
    if (binding_capacity > SIZE_MAX / sizeof(*bindings) ||
        function->parameter_count > SIZE_MAX / sizeof(*parameter_types))
        return ir_fail(error, error_capacity, "scalar HIR function is too large");
    if (!ir_scalar_function_return_type(function, &return_type,
                                        error, error_capacity)) return false;
    lowered = milena_ir_program_create();
    bindings = (IRScalarBinding *)calloc(binding_capacity, sizeof(*bindings));
    if (function->parameter_count)
        parameter_types = (MilenaIRType *)calloc(function->parameter_count,
                                                  sizeof(*parameter_types));
    if (!lowered || !bindings || (function->parameter_count && !parameter_types)) {
        milena_ir_program_destroy(lowered);
        free(bindings);
        free(parameter_types);
        return ir_fail(error, error_capacity, "out of memory lowering scalar HIR function");
    }
    for (size_t i = 0; i < function->parameter_count; ++i) {
        const MilenaHIRFunctionParameter *parameter = &function->parameters[i];
        if (!parameter->resolved_symbol_id || !parameter->name ||
            parameter->value_type != MILENA_HIR_NUMBER ||
            !ir_hir_value_type(parameter->value_type, &parameter_types[i])) {
            ir_fail(error, error_capacity,
                    "scalar HIR input parameters must have resolved numeric types");
            goto cleanup;
        }
        if (ir_scalar_binding_index(bindings, binding_count,
                                    parameter->resolved_symbol_id) != SIZE_MAX) {
            ir_fail(error, error_capacity,
                    "scalar HIR function contains duplicate parameter bindings");
            goto cleanup;
        }
        uint32_t value_id = (uint32_t)i + 1;
        bindings[binding_count++] = (IRScalarBinding){
            parameter->resolved_symbol_id, value_id, parameter_types[i]};
    }
    if (!milena_ir_program_set_function_signature(lowered, parameter_types,
            function->parameter_count, return_type)) {
        ir_fail(error, error_capacity,
                "could not establish scalar IR function signature");
        goto cleanup;
    }
    next_value = (uint32_t)function->parameter_count + 1;
    if (!milena_ir_program_add_block(lowered, 1)) {
        ir_fail(error, error_capacity, "could not create scalar IR entry block");
        goto cleanup;
    }
    for (size_t i = 0; i < function->parameter_count; ++i) {
        if (!milena_ir_program_add_block_parameter(lowered, 1,
                (uint32_t)i + 1, parameter_types[i])) {
            ir_fail(error, error_capacity,
                    "could not define scalar IR input at entry block");
            goto cleanup;
        }
    }
    for (size_t i = 0; i < function->body_count; ++i) {
        const MilenaHIRStatement *statement = function->body[i];
        if (!statement || saw_return) {
            ir_fail(error, error_capacity,
                    "scalar IR slice requires statements before one final return");
            goto cleanup;
        }
        if (statement->kind == MILENA_HIR_STMT_DECLARE ||
            statement->kind == MILENA_HIR_STMT_ASSIGN) {
            size_t index = ir_scalar_binding_index(bindings, binding_count,
                                                    statement->resolved_symbol_id);
            uint32_t value_id;
            MilenaIRType expression_type, statement_type;
            bool is_declaration = statement->kind == MILENA_HIR_STMT_DECLARE;
            if (!statement->resolved_symbol_id || !statement->as.expression ||
                !ir_hir_value_type(statement->value_type, &statement_type)) {
                ir_fail(error, error_capacity,
                        "scalar declaration or assignment lacks resolved typed data");
                goto cleanup;
            }
            if ((is_declaration && index != SIZE_MAX) ||
                (!is_declaration && index == SIZE_MAX)) {
                ir_fail(error, error_capacity,
                        "scalar declaration/assignment binding is inconsistent");
                goto cleanup;
            }
            if (!ir_scalar_lower_expression(lowered, current_block_id,
                    statement->as.expression, bindings, binding_count,
                    &next_value, &value_id,
                    &expression_type, error, error_capacity)) goto cleanup;
            if (expression_type != statement_type ||
                (!is_declaration && bindings[index].type != statement_type)) {
                ir_fail(error, error_capacity,
                        "scalar assignment changes or mismatches its binding type");
                goto cleanup;
            }
            if (is_declaration) {
                bindings[binding_count++] = (IRScalarBinding){
                    statement->resolved_symbol_id, value_id, statement_type};
            } else {
                bindings[index].value_id = value_id;
            }
            continue;
        }
        if (statement->kind == MILENA_HIR_STMT_RETURN &&
            i + 1 == function->body_count && statement->as.expression) {
            uint32_t return_value;
            MilenaIRType lowered_return_type;
            if (!ir_scalar_lower_expression(lowered, current_block_id,
                    statement->as.expression, bindings, binding_count,
                    &next_value, &return_value,
                    &lowered_return_type, error, error_capacity)) goto cleanup;
            if (!milena_ir_block_append_instruction(lowered, current_block_id,
                    MILENA_IR_RETURN, 0, lowered_return_type, return_value, 0, 0, 0.0, 0, 0)) {
                ir_fail(error, error_capacity, "could not append scalar IR return");
                goto cleanup;
            }
            saw_return = true;
            continue;
        }
        if (statement->kind == MILENA_HIR_STMT_IF &&
            i + 1 == function->body_count) {
            uint32_t condition_id;
            MilenaIRType condition_type;
            uint32_t then_block_id = (uint32_t)lowered->block_count + 1;
            uint32_t else_block_id = then_block_id + 1;
            if (statement->as.conditional.then_count != 1 ||
                statement->as.conditional.else_count != 1 ||
                !statement->as.conditional.then_body ||
                !statement->as.conditional.else_body ||
                !ir_scalar_lower_expression(lowered, current_block_id,
                    statement->as.conditional.condition, bindings, binding_count,
                    &next_value, &condition_id, &condition_type,
                    error, error_capacity)) goto cleanup;
            if (condition_type != MILENA_IR_TYPE_BOOL) {
                ir_fail(error, error_capacity,
                        "scalar si condition must lower to BOOL");
                goto cleanup;
            }
            if (!milena_ir_block_append_instruction(lowered, current_block_id,
                    MILENA_IR_COND_BRANCH, 0, MILENA_IR_TYPE_VOID, condition_id,
                    0, 0, 0.0, then_block_id, else_block_id)) {
                ir_fail(error, error_capacity,
                        "could not append scalar conditional branch");
                goto cleanup;
            }
            if (!milena_ir_program_add_block(lowered, then_block_id) ||
                !ir_scalar_lower_return_branch(lowered, then_block_id,
                    statement->as.conditional.then_body,
                    statement->as.conditional.then_count, bindings,
                    binding_count, &next_value, error, error_capacity) ||
                !milena_ir_program_add_block(lowered, else_block_id) ||
                !ir_scalar_lower_return_branch(lowered, else_block_id,
                    statement->as.conditional.else_body,
                    statement->as.conditional.else_count, bindings,
                    binding_count, &next_value, error, error_capacity))
                goto cleanup;
            saw_return = true;
            continue;
        }
        if (statement->kind == MILENA_HIR_STMT_IF) {
            uint32_t condition_id;
            uint32_t then_block_id, else_block_id, merge_block_id;
            uint32_t then_end_block_id, else_end_block_id;
            uint32_t next_block_id;
            MilenaIRType condition_type;
            IRScalarBinding *then_bindings = NULL, *else_bindings = NULL;
            size_t parameter_index = 0;
            if (!statement->as.conditional.else_body ||
                statement->as.conditional.else_count == 0) {
                ir_fail(error, error_capacity,
                    "typed scalar si/sino merge requires an explicit nonempty sino branch");
                goto cleanup;
            }
            if (lowered->block_count > UINT32_MAX - 3) {
                ir_fail(error, error_capacity, "typed scalar block-id space exhausted");
                goto cleanup;
            }
            then_block_id = (uint32_t)lowered->block_count + 1;
            else_block_id = then_block_id + 1;
            merge_block_id = then_block_id + 2;
            next_block_id = merge_block_id + 1;
            if (binding_count > SIZE_MAX / sizeof(*then_bindings)) {
                ir_fail(error, error_capacity,
                        "too many scalar bindings in conditional");
                goto cleanup;
            }
            if (binding_count) {
                then_bindings = malloc(binding_count * sizeof(*then_bindings));
                else_bindings = malloc(binding_count * sizeof(*else_bindings));
                if (!then_bindings || !else_bindings) {
                    free(then_bindings); free(else_bindings);
                    ir_fail(error, error_capacity,
                            "out of memory lowering scalar conditional assignments");
                    goto cleanup;
                }
                memcpy(then_bindings, bindings,
                       binding_count * sizeof(*then_bindings));
                memcpy(else_bindings, bindings,
                       binding_count * sizeof(*else_bindings));
            }
            if (!ir_scalar_lower_expression(lowered, current_block_id,
                    statement->as.conditional.condition, bindings, binding_count,
                    &next_value, &condition_id, &condition_type,
                    error, error_capacity)) {
                free(then_bindings); free(else_bindings);
                goto cleanup;
            }
            if (condition_type != MILENA_IR_TYPE_BOOL) {
                free(then_bindings); free(else_bindings);
                ir_fail(error, error_capacity,
                        "scalar si condition must lower to BOOL");
                goto cleanup;
            }
            if (!milena_ir_block_append_instruction(lowered, current_block_id,
                    MILENA_IR_COND_BRANCH, 0, MILENA_IR_TYPE_VOID, condition_id,
                    0, 0, 0.0, then_block_id, else_block_id) ||
                !milena_ir_program_add_block(lowered, then_block_id)) {
                free(then_bindings); free(else_bindings);
                ir_fail(error, error_capacity,
                        "could not create typed scalar conditional branch blocks");
                goto cleanup;
            }
            then_end_block_id = then_block_id;
            if (!ir_scalar_lower_assignment_sequence(lowered,
                    &then_end_block_id, &next_block_id, &next_value,
                    (const MilenaHIRStatement *const *)statement->as.conditional.then_body,
                    statement->as.conditional.then_count, then_bindings,
                    binding_count, 0, error, error_capacity) ||
                !milena_ir_block_append_instruction(lowered, then_end_block_id,
                    MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0.0,
                    merge_block_id, 0) ||
                !milena_ir_program_add_block(lowered, else_block_id)) {
                free(then_bindings); free(else_bindings);
                if (!error || !error[0])
                    ir_fail(error, error_capacity,
                            "could not terminate typed scalar then branch");
                goto cleanup;
            }
            else_end_block_id = else_block_id;
            if (!ir_scalar_lower_assignment_sequence(lowered,
                    &else_end_block_id, &next_block_id, &next_value,
                    (const MilenaHIRStatement *const *)statement->as.conditional.else_body,
                    statement->as.conditional.else_count, else_bindings,
                    binding_count, 0, error, error_capacity) ||
                !milena_ir_block_append_instruction(lowered, else_end_block_id,
                    MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0.0,
                    merge_block_id, 0) ||
                !milena_ir_program_add_block(lowered, merge_block_id)) {
                free(then_bindings); free(else_bindings);
                if (!error || !error[0])
                    ir_fail(error, error_capacity,
                            "could not terminate typed scalar else branch");
                goto cleanup;
            }
            for (size_t binding_index = 0; binding_index < binding_count;
                 ++binding_index) {
                if (then_bindings[binding_index].value_id ==
                    else_bindings[binding_index].value_id) continue;
                uint32_t merged_value;
                if (!ir_scalar_next_value(&next_value, &merged_value,
                        error, error_capacity) ||
                    !milena_ir_program_add_block_parameter(lowered,
                        merge_block_id, merged_value,
                        bindings[binding_index].type) ||
                    !milena_ir_block_add_edge_argument(lowered, then_end_block_id,
                        merge_block_id, (uint32_t)parameter_index,
                        then_bindings[binding_index].value_id) ||
                    !milena_ir_block_add_edge_argument(lowered, else_end_block_id,
                        merge_block_id, (uint32_t)parameter_index,
                        else_bindings[binding_index].value_id)) {
                    free(then_bindings); free(else_bindings);
                    ir_fail(error, error_capacity,
                            "could not define typed scalar merge parameter");
                    goto cleanup;
                }
                bindings[binding_index].value_id = merged_value;
                ++parameter_index;
            }
            free(then_bindings); free(else_bindings);
            current_block_id = merge_block_id;
            continue;
        }
        ir_fail(error, error_capacity,
                "scalar IR slice supports local declarations/assignments, a final return, final returning si/sino, and nonfinal si/sino assignments merged through block parameters");
        goto cleanup;
    }
    if (!saw_return) {
        ir_fail(error, error_capacity, "scalar HIR function has no final return");
        goto cleanup;
    }
    if (!milena_ir_program_validate(lowered, error, error_capacity)) goto cleanup;
    *program = *lowered;
    free(lowered);
    lowered = NULL;
    success = true;
cleanup:
    milena_ir_program_destroy(lowered);
    free(bindings);
    free(parameter_types);
    return success;
}
