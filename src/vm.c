#include "vm.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TYPED_VM_DEFAULT_STEPS UINT64_C(1000000)
#define TYPED_VM_HARD_MAX_STEPS UINT64_C(100000000)
#define TYPED_VM_DEFAULT_DEPTH 128u
#define TYPED_VM_HARD_MAX_DEPTH 128u
#define TYPED_VM_MAX_LIVE_BYTES ((size_t)64u * 1024u * 1024u)

typedef struct {
    uint32_t id;
    bool occupied;
    MilenaVMValue value;
} VMValueSlot;

typedef struct {
    uint64_t steps;
    uint64_t max_steps;
    size_t depth;
    size_t max_depth;
    size_t live_bytes;
    char *error;
    size_t error_capacity;
} VMExecution;

static bool vm_error(VMExecution *execution, const char *format, ...) {
    if (execution && execution->error && execution->error_capacity) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(execution->error, execution->error_capacity, format, args);
        va_end(args);
    }
    return false;
}

static void *vm_calloc(VMExecution *execution, size_t count, size_t size) {
    size_t bytes;
    void *allocation;
    if (!execution || (size && count > SIZE_MAX / size)) return NULL;
    bytes = count * size;
    if (bytes > TYPED_VM_MAX_LIVE_BYTES - execution->live_bytes) return NULL;
    allocation = calloc(count, size);
    if (allocation) execution->live_bytes += bytes;
    return allocation;
}

static void vm_release(VMExecution *execution, void *allocation, size_t count,
                       size_t size) {
    size_t bytes = 0;
    if (!allocation) return;
    if (size && count <= SIZE_MAX / size) bytes = count * size;
    if (execution) {
        if (bytes <= execution->live_bytes) execution->live_bytes -= bytes;
        else execution->live_bytes = 0;
    }
    free(allocation);
}

static bool vm_value_type_matches(MilenaIRType type,
                                  const MilenaVMValue *value) {
    if (!value || value->type != type) return false;
    if (type == MILENA_IR_TYPE_F64) return isfinite(value->as.f64) != 0;
    return type == MILENA_IR_TYPE_I64 || type == MILENA_IR_TYPE_BOOL;
}

static size_t vm_hash_capacity(size_t definitions) {
    size_t wanted, capacity = 8u;
    if (definitions > SIZE_MAX / 2u) return 0;
    wanted = definitions * 2u;
    while (capacity < wanted) {
        if (capacity > SIZE_MAX / 2u) return 0;
        capacity *= 2u;
    }
    return capacity;
}

static size_t vm_slot_index(uint32_t id, size_t capacity) {
    return ((size_t)(id * UINT32_C(2654435761))) & (capacity - 1u);
}

static bool vm_set_value(VMValueSlot *slots, size_t capacity, uint32_t id,
                         MilenaVMValue value) {
    size_t index;
    if (!slots || !capacity || !id) return false;
    index = vm_slot_index(id, capacity);
    for (size_t probes = 0; probes < capacity; ++probes) {
        VMValueSlot *slot = &slots[index];
        if (!slot->occupied || slot->id == id) {
            slot->id = id;
            slot->occupied = true;
            slot->value = value;
            return true;
        }
        index = (index + 1u) & (capacity - 1u);
    }
    return false;
}

static bool vm_get_value(const VMValueSlot *slots, size_t capacity, uint32_t id,
                         MilenaVMValue *value) {
    size_t index;
    if (!slots || !capacity || !id || !value) return false;
    index = vm_slot_index(id, capacity);
    for (size_t probes = 0; probes < capacity; ++probes) {
        const VMValueSlot *slot = &slots[index];
        if (!slot->occupied) return false;
        if (slot->id == id) {
            *value = slot->value;
            return true;
        }
        index = (index + 1u) & (capacity - 1u);
    }
    return false;
}

static const MilenaIRModuleFunction *vm_find_function(
    const MilenaIRModule *module, uint32_t symbol_id) {
    if (!module || !symbol_id) return NULL;
    for (size_t i = 0; i < module->function_count; ++i)
        if (module->functions[i].symbol_id == symbol_id)
            return &module->functions[i];
    return NULL;
}

static const MilenaIRBasicBlock *vm_find_block(const MilenaIRProgram *program,
                                                uint32_t block_id) {
    if (!program || !block_id) return NULL;
    for (size_t i = 0; i < program->block_count; ++i)
        if (program->blocks[i].id == block_id) return &program->blocks[i];
    return NULL;
}

static bool vm_edge_argument(const MilenaIRProgram *program,
                             uint32_t source_block, uint32_t target_block,
                             uint32_t parameter_index, uint32_t *value_id) {
    if (!program || !value_id) return false;
    for (size_t i = 0; i < program->edge_argument_count; ++i) {
        const MilenaIREdgeArgument *argument = &program->edge_arguments[i];
        if (argument->source_block_id == source_block &&
            argument->target_block_id == target_block &&
            argument->parameter_index == parameter_index) {
            *value_id = argument->value_id;
            return true;
        }
    }
    return false;
}

static bool vm_execute_function(VMExecution *execution,
                                const MilenaIRModule *module,
                                const MilenaIRModuleFunction *function,
                                const MilenaVMValue *arguments,
                                size_t argument_count,
                                MilenaVMValue *result);

static bool vm_transfer_block(VMExecution *execution,
                              const MilenaIRProgram *program,
                              VMValueSlot *slots, size_t slot_capacity,
                              uint32_t source_block, uint32_t target_block) {
    size_t parameter_count = 0, index = 0;
    MilenaVMValue *incoming = NULL;
    for (size_t i = 0; i < program->parameter_count; ++i)
        if (program->parameters[i].block_id == target_block) ++parameter_count;
    if (parameter_count) {
        incoming = (MilenaVMValue *)vm_calloc(execution, parameter_count,
                                                    sizeof(*incoming));
        if (!incoming)
            return vm_error(execution,
                            "VM memory limit exceeded transferring block arguments");
        for (size_t i = 0; i < program->parameter_count; ++i) {
            const MilenaIRBlockParameter *parameter = &program->parameters[i];
            uint32_t value_id;
            if (parameter->block_id != target_block) continue;
            if (!vm_edge_argument(program, source_block, target_block,
                                  (uint32_t)index, &value_id) ||
                !vm_get_value(slots, slot_capacity, value_id, &incoming[index]) ||
                !vm_value_type_matches(parameter->type, &incoming[index])) {
                vm_release(execution, incoming, parameter_count, sizeof(*incoming));
                return vm_error(execution,
                                "VM could not resolve a typed CFG edge argument");
            }
            ++index;
        }
        index = 0;
        for (size_t i = 0; i < program->parameter_count; ++i) {
            const MilenaIRBlockParameter *parameter = &program->parameters[i];
            if (parameter->block_id != target_block) continue;
            if (!vm_set_value(slots, slot_capacity, parameter->value_id,
                              incoming[index++])) {
                vm_release(execution, incoming, parameter_count, sizeof(*incoming));
                return vm_error(execution, "VM SSA value table is full");
            }
        }
        vm_release(execution, incoming, parameter_count, sizeof(*incoming));
    }
    return true;
}

static bool vm_i64_add(int64_t left, int64_t right, int64_t *sum) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) return false;
    *sum = left + right;
    return true;
}

static bool vm_execute_function(VMExecution *execution,
                                const MilenaIRModule *module,
                                const MilenaIRModuleFunction *function,
                                const MilenaVMValue *arguments,
                                size_t argument_count,
                                MilenaVMValue *result) {
    const MilenaIRProgram *program;
    VMValueSlot *slots = NULL;
    size_t slot_capacity = 0, definitions = 0;
    uint32_t current_block;
    bool ok = false;
    bool entered = false;
    if (!execution || !module || !function || !function->body || !result)
        return vm_error(execution, "VM received an invalid function frame");
    if (execution->depth >= execution->max_depth)
        return vm_error(execution, "VM call depth limit exceeded");
    if (argument_count != function->parameter_count ||
        (argument_count && !arguments))
        return vm_error(execution, "VM function argument count is invalid");
    for (size_t i = 0; i < argument_count; ++i)
        if (!vm_value_type_matches(function->parameter_types[i], &arguments[i]))
            return vm_error(execution, "VM function argument type mismatch at index %zu", i);
    program = function->body;
    if (program->parameter_count > SIZE_MAX - program->count)
        return vm_error(execution, "VM function value count overflow");
    definitions = program->parameter_count + program->count;
    slot_capacity = vm_hash_capacity(definitions);
    if (!slot_capacity || slot_capacity > SIZE_MAX / sizeof(*slots))
        return vm_error(execution, "VM function value table size overflow");
    slots = (VMValueSlot *)vm_calloc(execution, slot_capacity, sizeof(*slots));
    if (!slots)
        return vm_error(execution, "VM memory limit exceeded allocating SSA values");
    ++execution->depth;
    entered = true;
    current_block = program->blocks[0].id;
    size_t input_index = 0;
    for (size_t i = 0; i < program->parameter_count; ++i) {
        const MilenaIRBlockParameter *parameter = &program->parameters[i];
        if (parameter->block_id != current_block) continue;
        if (input_index >= argument_count ||
            !vm_set_value(slots, slot_capacity, parameter->value_id,
                          arguments[input_index++])) {
            vm_error(execution, "VM could not bind function input parameters");
            goto cleanup;
        }
    }
    if (input_index != argument_count) {
        vm_error(execution, "VM entry block signature does not match arguments");
        goto cleanup;
    }

    for (;;) {
        const MilenaIRBasicBlock *block = vm_find_block(program, current_block);
        bool transferred = false;
        if (!block) {
            vm_error(execution, "VM branch targets a missing block");
            goto cleanup;
        }
        size_t end = block->first_instruction + block->instruction_count;
        for (size_t ii = block->first_instruction; ii < end; ++ii) {
            const MilenaIRInstruction *ins = &program->instructions[ii];
            MilenaVMValue left = {0}, right = {0}, produced = {0};
            uint32_t target = 0;
            if (execution->steps >= execution->max_steps) {
                vm_error(execution, "VM instruction step limit exceeded");
                goto cleanup;
            }
            ++execution->steps;
            switch (ins->opcode) {
                case MILENA_IR_CONST_I64:
                    produced.type = MILENA_IR_TYPE_I64;
                    produced.as.i64 = ins->integer_immediate;
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_CONST_F64:
                    produced.type = MILENA_IR_TYPE_F64;
                    produced.as.f64 = ins->float_immediate;
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_CONST_BOOL:
                    produced.type = MILENA_IR_TYPE_BOOL;
                    produced.as.boolean = ins->integer_immediate != 0;
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_ADD_I64:
                    if (!vm_get_value(slots, slot_capacity, ins->operand1_id, &left) ||
                        !vm_get_value(slots, slot_capacity, ins->operand2_id, &right) ||
                        left.type != MILENA_IR_TYPE_I64 || right.type != MILENA_IR_TYPE_I64 ||
                        !vm_i64_add(left.as.i64, right.as.i64, &produced.as.i64)) {
                        vm_error(execution, "VM integer addition overflow or invalid operand");
                        goto cleanup;
                    }
                    produced.type = MILENA_IR_TYPE_I64;
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_ADD_F64:
                case MILENA_IR_SUB_F64:
                case MILENA_IR_MUL_F64:
                case MILENA_IR_DIV_F64:
                    if (!vm_get_value(slots, slot_capacity, ins->operand1_id, &left) ||
                        !vm_get_value(slots, slot_capacity, ins->operand2_id, &right) ||
                        left.type != MILENA_IR_TYPE_F64 || right.type != MILENA_IR_TYPE_F64) {
                        vm_error(execution, "VM numeric operation has an invalid operand");
                        goto cleanup;
                    }
                    produced.type = MILENA_IR_TYPE_F64;
                    if (ins->opcode == MILENA_IR_ADD_F64)
                        produced.as.f64 = left.as.f64 + right.as.f64;
                    else if (ins->opcode == MILENA_IR_SUB_F64)
                        produced.as.f64 = left.as.f64 - right.as.f64;
                    else if (ins->opcode == MILENA_IR_MUL_F64)
                        produced.as.f64 = left.as.f64 * right.as.f64;
                    else {
                        if (right.as.f64 == 0.0) {
                            vm_error(execution, "VM division by zero");
                            goto cleanup;
                        }
                        produced.as.f64 = left.as.f64 / right.as.f64;
                    }
                    if (!isfinite(produced.as.f64)) {
                        vm_error(execution, "VM numeric result is not finite");
                        goto cleanup;
                    }
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_EQ_I64:
                    if (!vm_get_value(slots, slot_capacity, ins->operand1_id, &left) ||
                        !vm_get_value(slots, slot_capacity, ins->operand2_id, &right) ||
                        left.type != MILENA_IR_TYPE_I64 || right.type != MILENA_IR_TYPE_I64) {
                        vm_error(execution, "VM integer comparison has an invalid operand");
                        goto cleanup;
                    }
                    produced.type = MILENA_IR_TYPE_BOOL;
                    produced.as.boolean = left.as.i64 == right.as.i64;
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_EQ_F64:
                case MILENA_IR_NE_F64:
                case MILENA_IR_LT_F64:
                case MILENA_IR_LE_F64:
                case MILENA_IR_GT_F64:
                case MILENA_IR_GE_F64:
                    if (!vm_get_value(slots, slot_capacity, ins->operand1_id, &left) ||
                        !vm_get_value(slots, slot_capacity, ins->operand2_id, &right) ||
                        left.type != MILENA_IR_TYPE_F64 || right.type != MILENA_IR_TYPE_F64) {
                        vm_error(execution, "VM numeric comparison has an invalid operand");
                        goto cleanup;
                    }
                    produced.type = MILENA_IR_TYPE_BOOL;
                    if (ins->opcode == MILENA_IR_EQ_F64)
                        produced.as.boolean = left.as.f64 == right.as.f64;
                    else if (ins->opcode == MILENA_IR_NE_F64)
                        produced.as.boolean = left.as.f64 != right.as.f64;
                    else if (ins->opcode == MILENA_IR_LT_F64)
                        produced.as.boolean = left.as.f64 < right.as.f64;
                    else if (ins->opcode == MILENA_IR_LE_F64)
                        produced.as.boolean = left.as.f64 <= right.as.f64;
                    else if (ins->opcode == MILENA_IR_GT_F64)
                        produced.as.boolean = left.as.f64 > right.as.f64;
                    else
                        produced.as.boolean = left.as.f64 >= right.as.f64;
                    if (!vm_set_value(slots, slot_capacity, ins->result_id, produced))
                        goto value_table_error;
                    break;
                case MILENA_IR_CALL: {
                    const MilenaIRModuleFunction *callee = NULL;
                    MilenaVMValue *call_arguments = NULL;
                    MilenaVMValue call_result = {0};
                    if (ins->integer_immediate <= 0 ||
                        (uint64_t)ins->integer_immediate > UINT32_MAX) {
                        vm_error(execution, "VM call target symbol is invalid");
                        goto cleanup;
                    }
                    callee = vm_find_function(module,
                        (uint32_t)ins->integer_immediate);
                    if (!callee || ins->call_argument_count != callee->parameter_count ||
                        ins->call_argument_offset > program->call_argument_count ||
                        ins->call_argument_count > program->call_argument_count -
                            ins->call_argument_offset) {
                        vm_error(execution, "VM call target or argument slice is invalid");
                        goto cleanup;
                    }
                    if (ins->call_argument_count) {
                        call_arguments = (MilenaVMValue *)vm_calloc(
                            execution, ins->call_argument_count,
                            sizeof(*call_arguments));
                        if (!call_arguments) {
                            vm_error(execution, "VM memory limit exceeded copying call arguments");
                            goto cleanup;
                        }
                        for (size_t a = 0; a < ins->call_argument_count; ++a) {
                            uint32_t value_id = program->call_arguments[
                                ins->call_argument_offset + a];
                            if (!vm_get_value(slots, slot_capacity, value_id,
                                              &call_arguments[a])) {
                                vm_release(execution, call_arguments,
                                           ins->call_argument_count,
                                           sizeof(*call_arguments));
                                vm_error(execution, "VM call argument SSA value is unavailable");
                                goto cleanup;
                            }
                        }
                    }
                    bool called = vm_execute_function(execution, module, callee,
                        call_arguments, ins->call_argument_count, &call_result);
                    vm_release(execution, call_arguments, ins->call_argument_count,
                               sizeof(*call_arguments));
                    if (!called) goto cleanup;
                    if (!vm_value_type_matches(ins->result_type, &call_result) ||
                        !vm_set_value(slots, slot_capacity, ins->result_id, call_result))
                        goto value_table_error;
                    break;
                }
                case MILENA_IR_BRANCH:
                    target = ins->target_true;
                    if (!vm_transfer_block(execution, program, slots, slot_capacity,
                                           current_block, target)) goto cleanup;
                    current_block = target;
                    transferred = true;
                    break;
                case MILENA_IR_COND_BRANCH:
                    if (!vm_get_value(slots, slot_capacity, ins->operand1_id, &left) ||
                        left.type != MILENA_IR_TYPE_BOOL) {
                        vm_error(execution, "VM conditional branch requires a boolean value");
                        goto cleanup;
                    }
                    target = left.as.boolean ? ins->target_true : ins->target_false;
                    if (!vm_transfer_block(execution, program, slots, slot_capacity,
                                           current_block, target)) goto cleanup;
                    current_block = target;
                    transferred = true;
                    break;
                case MILENA_IR_RETURN:
                    if (!vm_get_value(slots, slot_capacity, ins->operand1_id, &produced) ||
                        !vm_value_type_matches(function->return_type, &produced)) {
                        vm_error(execution, "VM function return value is unavailable or mistyped");
                        goto cleanup;
                    }
                    *result = produced;
                    ok = true;
                    goto cleanup;
                default:
                    vm_error(execution, "VM encountered an unsupported typed IR opcode");
                    goto cleanup;
            }
            if (transferred) break;
            continue;
value_table_error:
            vm_error(execution, "VM SSA value table is full or result type is invalid");
            goto cleanup;
        }
        if (!transferred) {
            vm_error(execution, "VM basic block ended without a control-flow transfer");
            goto cleanup;
        }
    }
cleanup:
    if (entered) --execution->depth;
    vm_release(execution, slots, slot_capacity, sizeof(*slots));
    return ok;
}

bool vm_run(const uint8_t *bytecode, size_t bytecode_size,
                             uint32_t entry_symbol_id,
                             const MilenaVMValue *arguments,
                             size_t argument_count,
                             const MilenaVMOptions *options,
                             MilenaVMValue *result,
                             char *error, size_t error_capacity) {
    MilenaVMOptions selected = {0};
    VMExecution execution = {0};
    MilenaIRModule *module = NULL;
    const MilenaIRModuleFunction *entry;
    MilenaVMValue computed = {0};
    bool ok;
    if (error && error_capacity) error[0] = '\0';
    execution.error = error;
    execution.error_capacity = error_capacity;
    if (!result || !entry_symbol_id)
        return vm_error(&execution, "VM requires a result destination and entry symbol");
    if (options) selected = *options;
    if (!selected.max_steps) selected.max_steps = TYPED_VM_DEFAULT_STEPS;
    if (!selected.max_call_depth) selected.max_call_depth = TYPED_VM_DEFAULT_DEPTH;
    if (selected.max_steps > TYPED_VM_HARD_MAX_STEPS ||
        selected.max_call_depth > TYPED_VM_HARD_MAX_DEPTH)
        return vm_error(&execution, "VM execution limits exceed the supported maximum");
    execution.max_steps = selected.max_steps;
    execution.max_depth = selected.max_call_depth;
    execution.error = error;
    execution.error_capacity = error_capacity;
    if (!milena_bytecode_decode_module(bytecode, bytecode_size, &module,
                                       error, error_capacity)) return false;
    entry = vm_find_function(module, entry_symbol_id);
    if (!entry) {
        milena_ir_module_destroy(module);
        return vm_error(&execution, "VM entry function symbol is not in the module");
    }
    if (argument_count != entry->parameter_count ||
        (argument_count && !arguments)) {
        milena_ir_module_destroy(module);
        return vm_error(&execution, "VM entry function argument count is invalid");
    }
    ok = vm_execute_function(&execution, module, entry, arguments,
                             argument_count, &computed);
    milena_ir_module_destroy(module);
    if (!ok) return false;
    *result = computed;
    return true;
}
