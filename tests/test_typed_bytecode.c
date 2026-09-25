#include "typed_bytecode.h"
#include "vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool run_bytecode_case(const uint8_t *bytecode, size_t bytecode_size,
                              uint32_t entry_symbol_id,
                              const MilenaVMValue *arguments,
                              size_t argument_count,
                              const MilenaVMOptions *options,
                              MilenaVMValue *result, char *error,
                              size_t error_capacity) {
    VirtualMachine vm = {0};
    if (error && error_capacity) error[0] = '\0';
    if (!vm_init_bytecode(&vm, bytecode, bytecode_size, entry_symbol_id,
                          arguments, argument_count, options)) {
        const char *message = vm_bytecode_error(&vm);
        if (error && error_capacity)
            (void)snprintf(error, error_capacity, "%s",
                           message ? message : "VM initialization failed");
        vm_destroy(&vm);
        return false;
    }
    if (!vm_run(&vm)) {
        const char *message = vm_bytecode_error(&vm);
        if (error && error_capacity)
            (void)snprintf(error, error_capacity, "%s",
                           message ? message : "VM execution failed");
        vm_destroy(&vm);
        return false;
    }
    if (result && !vm_get_bytecode_result(&vm, result)) {
        if (error && error_capacity)
            (void)snprintf(error, error_capacity,
                           "VM did not publish a bytecode result");
        vm_destroy(&vm);
        return false;
    }
    vm_destroy(&vm);
    return true;
}

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1u);
    assert(copy != NULL);
    memcpy(copy, text, length + 1u);
    return copy;
}

static bool append_instruction(MilenaIRProgram *program, uint32_t block_id,
                               MilenaIROpCode opcode, uint32_t result_id,
                               MilenaIRType result_type, uint32_t operand1,
                               uint32_t operand2, int64_t integer_immediate,
                               double float_immediate, uint32_t target_true,
                               uint32_t target_false) {
    return milena_ir_block_append_instruction(program, block_id, opcode, result_id,
        result_type, operand1, operand2, integer_immediate, float_immediate,
        target_true, target_false);
}

static MilenaIRModule *make_call_module(void) {
    MilenaIRModule *module = (MilenaIRModule *)calloc(1, sizeof(*module));
    MilenaIRType parameters[] = {
        MILENA_IR_TYPE_F64, MILENA_IR_TYPE_F64, MILENA_IR_TYPE_F64
    };
    assert(module != NULL);
    module->functions = (MilenaIRModuleFunction *)calloc(2u,
                                                         sizeof(*module->functions));
    assert(module->functions != NULL);
    module->function_count = 2u;

    MilenaIRModuleFunction *callee = &module->functions[0];
    callee->name = copy_text("sumar_tres");
    callee->symbol_id = 100u;
    callee->parameter_types = (MilenaIRType *)malloc(sizeof(parameters));
    assert(callee->parameter_types != NULL);
    memcpy(callee->parameter_types, parameters, sizeof(parameters));
    callee->parameter_count = 3u;
    callee->return_type = MILENA_IR_TYPE_F64;
    callee->body = milena_ir_program_create();
    assert(callee->body != NULL);
    assert(milena_ir_program_set_function_signature(callee->body, parameters,
                                                     3u, MILENA_IR_TYPE_F64));
    assert(milena_ir_program_add_block(callee->body, 1u));
    for (uint32_t i = 0; i < 3u; ++i)
        assert(milena_ir_program_add_block_parameter(callee->body, 1u, i + 1u,
                                                      MILENA_IR_TYPE_F64));
    assert(append_instruction(callee->body, 1u, MILENA_IR_ADD_F64, 4u,
        MILENA_IR_TYPE_F64, 1u, 2u, 0, 0.0, 0u, 0u));
    assert(append_instruction(callee->body, 1u, MILENA_IR_ADD_F64, 5u,
        MILENA_IR_TYPE_F64, 4u, 3u, 0, 0.0, 0u, 0u));
    assert(append_instruction(callee->body, 1u, MILENA_IR_RETURN, 0u,
        MILENA_IR_TYPE_F64, 5u, 0u, 0, 0.0, 0u, 0u));

    MilenaIRModuleFunction *caller = &module->functions[1];
    caller->name = copy_text("principal");
    caller->symbol_id = 200u;
    caller->return_type = MILENA_IR_TYPE_F64;
    caller->body = milena_ir_program_create();
    assert(caller->body != NULL);
    assert(milena_ir_program_set_function_signature(caller->body, NULL, 0u,
                                                     MILENA_IR_TYPE_F64));
    assert(milena_ir_program_add_block(caller->body, 2u));
    for (uint32_t i = 0; i < 3u; ++i)
        assert(append_instruction(caller->body, 2u, MILENA_IR_CONST_F64, i + 1u,
            MILENA_IR_TYPE_F64, 0u, 0u, 0, (double)(i + 1u), 0u, 0u));
    const uint32_t argument_ids[] = {1u, 2u, 3u};
    assert(milena_ir_block_append_call(caller->body, 2u, 4u,
        MILENA_IR_TYPE_F64, 100u, argument_ids, 3u));
    assert(append_instruction(caller->body, 2u, MILENA_IR_RETURN, 0u,
        MILENA_IR_TYPE_F64, 4u, 0u, 0, 0.0, 0u, 0u));

    callee->body->module_context = module;
    caller->body->module_context = module;
    return module;
}

static MilenaIRModule *make_division_by_zero_module(void) {
    MilenaIRModule *module = (MilenaIRModule *)calloc(1, sizeof(*module));
    assert(module != NULL);
    module->functions = (MilenaIRModuleFunction *)calloc(1u,
                                                         sizeof(*module->functions));
    assert(module->functions != NULL);
    module->function_count = 1u;
    MilenaIRModuleFunction *function = &module->functions[0];
    function->name = copy_text("dividir_por_cero");
    function->symbol_id = 300u;
    function->return_type = MILENA_IR_TYPE_F64;
    function->body = milena_ir_program_create();
    assert(function->body != NULL);
    assert(milena_ir_program_set_function_signature(function->body, NULL, 0u,
                                                     MILENA_IR_TYPE_F64));
    assert(milena_ir_program_add_block(function->body, 1u));
    assert(append_instruction(function->body, 1u, MILENA_IR_CONST_F64, 1u,
        MILENA_IR_TYPE_F64, 0u, 0u, 0, 1.0, 0u, 0u));
    assert(append_instruction(function->body, 1u, MILENA_IR_CONST_F64, 2u,
        MILENA_IR_TYPE_F64, 0u, 0u, 0, 0.0, 0u, 0u));
    assert(append_instruction(function->body, 1u, MILENA_IR_DIV_F64, 3u,
        MILENA_IR_TYPE_F64, 1u, 2u, 0, 0.0, 0u, 0u));
    assert(append_instruction(function->body, 1u, MILENA_IR_RETURN, 0u,
        MILENA_IR_TYPE_F64, 3u, 0u, 0, 0.0, 0u, 0u));
    function->body->module_context = module;
    return module;
}

static MilenaIRModule *make_bounded_loop_module(void) {
    MilenaIRModule *module = (MilenaIRModule *)calloc(1, sizeof(*module));
    assert(module != NULL);
    module->functions = (MilenaIRModuleFunction *)calloc(1u,
                                                         sizeof(*module->functions));
    assert(module->functions != NULL);
    module->function_count = 1u;
    MilenaIRModuleFunction *function = &module->functions[0];
    function->name = copy_text("bucle_limitado");
    function->symbol_id = 301u;
    function->return_type = MILENA_IR_TYPE_F64;
    function->body = milena_ir_program_create();
    assert(function->body != NULL);
    assert(milena_ir_program_set_function_signature(function->body, NULL, 0u,
                                                     MILENA_IR_TYPE_F64));
    assert(milena_ir_program_add_block(function->body, 1u));
    assert(append_instruction(function->body, 1u, MILENA_IR_CONST_BOOL, 1u,
        MILENA_IR_TYPE_BOOL, 0u, 0u, 1, 0.0, 0u, 0u));
    assert(append_instruction(function->body, 1u, MILENA_IR_COND_BRANCH, 0u,
        MILENA_IR_TYPE_VOID, 1u, 0u, 0, 0.0, 2u, 3u));
    assert(milena_ir_program_add_block(function->body, 2u));
    assert(append_instruction(function->body, 2u, MILENA_IR_BRANCH, 0u,
        MILENA_IR_TYPE_VOID, 0u, 0u, 0, 0.0, 2u, 0u));
    assert(milena_ir_program_add_block(function->body, 3u));
    assert(append_instruction(function->body, 3u, MILENA_IR_CONST_F64, 2u,
        MILENA_IR_TYPE_F64, 0u, 0u, 0, 42.0, 0u, 0u));
    assert(append_instruction(function->body, 3u, MILENA_IR_RETURN, 0u,
        MILENA_IR_TYPE_F64, 2u, 0u, 0, 0.0, 0u, 0u));
    function->body->module_context = module;
    return module;
}

static void test_roundtrip_module_with_direct_call(void) {
    char error[256] = {0};
    uint8_t *encoded = NULL;
    uint8_t *again = NULL;
    size_t encoded_size = 0, again_size = 0;
    MilenaIRModule *module = make_call_module();
    MilenaIRModule *decoded = NULL;
    assert(milena_ir_module_validate(module, error, sizeof(error)));
    assert(milena_bytecode_encode_module(module, &encoded, &encoded_size,
                                          error, sizeof(error)));
    assert(encoded != NULL && encoded_size > 16u);
    assert(milena_bytecode_verify(encoded, encoded_size, error, sizeof(error)));
    assert(milena_bytecode_decode_module(encoded, encoded_size, &decoded,
                                         error, sizeof(error)));
    assert(decoded != NULL && decoded->function_count == 2u);
    assert(strcmp(decoded->functions[0].name, "sumar_tres") == 0);
    assert(decoded->functions[0].symbol_id == 100u);
    assert(decoded->functions[0].parameter_count == 3u);
    assert(decoded->functions[0].body->parameters[2].value_id == 3u);
    assert(decoded->functions[1].body->instructions[3].opcode == MILENA_IR_CALL);
    assert(decoded->functions[1].body->instructions[3].integer_immediate == 100);
    assert(decoded->functions[1].body->instructions[3].call_argument_offset == 0u);
    assert(decoded->functions[1].body->instructions[3].call_argument_count == 3u);
    assert(decoded->functions[1].body->call_argument_count == 3u);
    assert(decoded->functions[1].body->call_arguments[0] == 1u);
    assert(decoded->functions[1].body->call_arguments[1] == 2u);
    assert(decoded->functions[1].body->call_arguments[2] == 3u);
    assert(milena_bytecode_encode_module(decoded, &again, &again_size,
                                         error, sizeof(error)));
    assert(again_size == encoded_size);
    assert(memcmp(again, encoded, encoded_size) == 0);

    /* The reference VM accepts only verified portable bytecode and executes
       the Spanish-named three-argument function through the canonical module. */
    MilenaVMValue result = {0};
    assert(run_bytecode_case(encoded, encoded_size, 200u, NULL, 0u,
                                   NULL, &result, error, sizeof(error)));
    assert(result.type == MILENA_IR_TYPE_F64 && result.as.f64 == 6.0);
    MilenaVMValue previous = {MILENA_IR_TYPE_BOOL, {.boolean = true}};
    result = previous;
    MilenaVMValue unexpected_argument = {MILENA_IR_TYPE_F64, {.f64 = 1.0}};
    assert(!run_bytecode_case(encoded, encoded_size, 200u,
                                    &unexpected_argument, 1u, NULL, &result,
                                    error, sizeof(error)));
    assert(result.type == previous.type && result.as.boolean == previous.as.boolean);

    free(again);
    free(encoded);
    milena_ir_module_destroy(decoded);
    milena_ir_module_destroy(module);
}

static void expect_invalid(const uint8_t *bytes, size_t size) {
    char error[256] = {0};
    MilenaIRModule *module = NULL;
    assert(!milena_bytecode_verify(bytes, size, error, sizeof(error)));
    assert(error[0] != '\0');
    assert(!milena_bytecode_decode_module(bytes, size, &module,
                                          error, sizeof(error)));
    assert(module == NULL);
}

static void set_u32_le(uint8_t *bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4u; ++i)
        bytes[offset + i] = (uint8_t)(value >> (i * 8u));
}

static void set_payload_length(uint8_t *bytes, size_t total_size) {
    set_u32_le(bytes, 12u, (uint32_t)(total_size - 16u));
}

static void test_rejects_bad_headers_and_lengths(void) {
    char error[256] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    MilenaIRModule *module = make_call_module();
    assert(milena_bytecode_encode_module(module, &bytes, &size,
                                          error, sizeof(error)));
    milena_ir_module_destroy(module);

    /* Match the declared length so the decoder reaches the truncated record. */
    uint8_t *truncated = (uint8_t *)malloc(size);
    assert(truncated != NULL);
    memcpy(truncated, bytes, size);
    set_payload_length(truncated, size - 1u);
    expect_invalid(truncated, size - 1u);
    free(truncated);

    /* A true trailing byte with a matching header must fail after module parse. */
    uint8_t *trailing = (uint8_t *)malloc(size + 1u);
    assert(trailing != NULL);
    memcpy(trailing, bytes, size);
    trailing[size] = 0xa5u;
    set_payload_length(trailing, size + 1u);
    expect_invalid(trailing, size + 1u);
    free(trailing);

    uint8_t *mutated = (uint8_t *)malloc(size);
    assert(mutated != NULL);
    memcpy(mutated, bytes, size);
    mutated[0] ^= 0x40u;
    expect_invalid(mutated, size);

    memcpy(mutated, bytes, size);
    mutated[4] = (uint8_t)(MILENA_BYTECODE_VERSION_MAJOR + 1u);
    expect_invalid(mutated, size);

    memcpy(mutated, bytes, size);
    mutated[8] = 1u; /* unsupported header flags */
    expect_invalid(mutated, size);

    memcpy(mutated, bytes, size);
    mutated[12] ^= 1u;
    expect_invalid(mutated, size);

    memcpy(mutated, bytes, size);
    /* function_count follows the 16-byte header and must be nonzero */
    memset(mutated + 16u, 0, 4u);
    expect_invalid(mutated, size);

    memcpy(mutated, bytes, size);
    /* The 16-byte header is followed by module function_count (4 bytes),
       function metadata and body counts. For the 10-byte name, body counts
       are block=46, parameters=50, edges=54, call args=58, instructions=62.
       Each count fits alone; their combined wire size does not. */
    set_u32_le(mutated, 50u, 35u);  /* block parameter count */
    set_u32_le(mutated, 54u, 29u);  /* edge argument count */
    set_u32_le(mutated, 58u, 100u); /* call argument count */
    set_u32_le(mutated, 62u, 8u);   /* instruction count */
    char resource_error[256] = {0};
    assert(!milena_bytecode_verify(mutated, size, resource_error,
                                   sizeof(resource_error)));
    assert(strstr(resource_error,
                  "arrays exceed remaining bytecode length") != NULL);
    expect_invalid(mutated, size);

    memcpy(mutated, bytes, size);
    /* Locate the first opcode from the format's fixed-width field sizes and
       this fixture's three F64 signature/block parameters. */
    const size_t first_opcode_offset = 16u + 4u + 4u + 4u +
        (sizeof("sumar_tres") - 1u) + 4u + 1u + 3u + 5u * 4u + 1u + 4u + 3u +
        24u + 3u * 12u;
    assert(first_opcode_offset + 1u < size);
    mutated[first_opcode_offset] = 0xffu;
    expect_invalid(mutated, size);

    free(mutated);
    free(bytes);
}

static void test_rejects_signature_mismatch_and_invalid_source_module(void) {
    char error[256] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    MilenaIRModule *module = make_call_module();
    assert(milena_bytecode_encode_module(module, &bytes, &size,
                                          error, sizeof(error)));

    uint8_t *mutated = (uint8_t *)malloc(size);
    assert(mutated != NULL);
    memcpy(mutated, bytes, size);
    /* Module function_count precedes the first function's symbol/name/arity. */
    mutated[42u] = (uint8_t)MILENA_IR_TYPE_BOOL;
    expect_invalid(mutated, size);

    module->functions[1].body->instructions[3].integer_immediate = 999u;
    uint8_t *rejected = NULL;
    size_t rejected_size = 0;
    assert(!milena_bytecode_encode_module(module, &rejected, &rejected_size,
                                          error, sizeof(error)));
    assert(rejected == NULL && rejected_size == 0u);

    free(mutated);
    free(bytes);
    milena_ir_module_destroy(module);
}


static MilenaIRModule *make_branch_merge_module(void) {
    MilenaIRModule *module = (MilenaIRModule *)calloc(1, sizeof(*module));
    assert(module != NULL);
    module->functions = (MilenaIRModuleFunction *)calloc(1u,
                                                         sizeof(*module->functions));
    assert(module->functions != NULL);
    module->function_count = 1u;
    MilenaIRModuleFunction *function = &module->functions[0];
    function->name = copy_text("elegir_valor");
    function->symbol_id = 400u;
    function->return_type = MILENA_IR_TYPE_I64;
    function->body = milena_ir_program_create();
    assert(function->body != NULL);
    assert(milena_ir_program_set_function_signature(function->body, NULL, 0u,
                                                     MILENA_IR_TYPE_I64));
    /* Blocks are appended after the previous block has a terminator. */
    assert(milena_ir_program_add_block(function->body, 1u));
    assert(append_instruction(function->body, 1u, MILENA_IR_CONST_BOOL, 1u,
        MILENA_IR_TYPE_BOOL, 0u, 0u, 0, 0.0, 0u, 0u));
    assert(append_instruction(function->body, 1u, MILENA_IR_COND_BRANCH, 0u,
        MILENA_IR_TYPE_VOID, 1u, 0u, 0, 0.0, 2u, 3u));
    assert(milena_ir_program_add_block(function->body, 2u));
    assert(append_instruction(function->body, 2u, MILENA_IR_CONST_I64, 2u,
        MILENA_IR_TYPE_I64, 0u, 0u, 41, 0.0, 0u, 0u));
    assert(append_instruction(function->body, 2u, MILENA_IR_BRANCH, 0u,
        MILENA_IR_TYPE_VOID, 0u, 0u, 0, 0.0, 4u, 0u));
    assert(milena_ir_program_add_block(function->body, 3u));
    assert(append_instruction(function->body, 3u, MILENA_IR_CONST_I64, 3u,
        MILENA_IR_TYPE_I64, 0u, 0u, 42, 0.0, 0u, 0u));
    assert(append_instruction(function->body, 3u, MILENA_IR_BRANCH, 0u,
        MILENA_IR_TYPE_VOID, 0u, 0u, 0, 0.0, 4u, 0u));
    assert(milena_ir_program_add_block(function->body, 4u));
    assert(milena_ir_program_add_block_parameter(function->body, 4u, 4u,
                                                  MILENA_IR_TYPE_I64));
    assert(milena_ir_block_add_edge_argument(function->body, 2u, 4u, 0u, 2u));
    assert(milena_ir_block_add_edge_argument(function->body, 3u, 4u, 0u, 3u));
    assert(append_instruction(function->body, 4u, MILENA_IR_RETURN, 0u,
        MILENA_IR_TYPE_I64, 4u, 0u, 0, 0.0, 0u, 0u));
    function->body->module_context = module;
    return module;
}

static void test_vm_branch_ssa_merge(void) {
    char error[256] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    MilenaIRModule *module = make_branch_merge_module();
    MilenaVMValue result = {0};
    assert(milena_ir_module_validate(module, error, sizeof(error)));
    assert(milena_bytecode_encode_module(module, &bytes, &size,
                                         error, sizeof(error)));
    assert(run_bytecode_case(bytes, size, 400u, NULL, 0u, NULL,
                                   &result, error, sizeof(error)));
    assert(result.type == MILENA_IR_TYPE_I64 && result.as.i64 == 42);
    free(bytes);
    milena_ir_module_destroy(module);
}

static void test_vm_forward_call_and_call_depth_limit(void) {
    char error[256] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    MilenaIRModule *module = make_call_module();
    /* Put principal before sumar_tres: this is a true forward call in wire order. */
    MilenaIRModuleFunction swap = module->functions[0];
    module->functions[0] = module->functions[1];
    module->functions[1] = swap;
    module->functions[0].body->module_context = module;
    module->functions[1].body->module_context = module;
    MilenaVMValue result = {0};
    assert(milena_bytecode_encode_module(module, &bytes, &size,
                                         error, sizeof(error)));
    assert(run_bytecode_case(bytes, size, 200u, NULL, 0u, NULL,
                                   &result, error, sizeof(error)));
    assert(result.type == MILENA_IR_TYPE_F64 && result.as.f64 == 6.0);
    MilenaVMValue previous = {MILENA_IR_TYPE_I64, {.i64 = 99}};
    result = previous;
    MilenaVMOptions shallow = {100u, 1u};
    assert(!run_bytecode_case(bytes, size, 200u, NULL, 0u, &shallow,
                                    &result, error, sizeof(error)));
    assert(strstr(error, "call depth") != NULL);
    assert(result.type == previous.type && result.as.i64 == previous.as.i64);
    free(bytes);
    milena_ir_module_destroy(module);
}

static void test_vm_errors_and_limits(void) {
    char error[256] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    MilenaIRModule *division = make_division_by_zero_module();
    assert(milena_bytecode_encode_module(division, &bytes, &size,
                                         error, sizeof(error)));
    MilenaVMValue previous = {MILENA_IR_TYPE_F64, {.f64 = 17.0}};
    MilenaVMValue result = previous;
    assert(!run_bytecode_case(bytes, size, 300u, NULL, 0u, NULL,
                                    &result, error, sizeof(error)));
    assert(strstr(error, "division by zero") != NULL);
    assert(result.type == previous.type && result.as.f64 == previous.as.f64);
    free(bytes);
    milena_ir_module_destroy(division);

    MilenaIRModule *loop = make_bounded_loop_module();
    bytes = NULL;
    size = 0;
    assert(milena_bytecode_encode_module(loop, &bytes, &size,
                                         error, sizeof(error)));
    MilenaVMOptions short_budget = {3u, 8u};
    result = previous;
    assert(!run_bytecode_case(bytes, size, 301u, NULL, 0u,
                                    &short_budget, &result, error, sizeof(error)));
    assert(strstr(error, "step limit") != NULL);
    assert(result.type == previous.type && result.as.f64 == previous.as.f64);
    free(bytes);
    milena_ir_module_destroy(loop);

    MilenaIRModule *calls = make_call_module();
    bytes = NULL;
    size = 0;
    assert(milena_bytecode_encode_module(calls, &bytes, &size,
                                         error, sizeof(error)));
    uint8_t *malformed = (uint8_t *)malloc(size);
    assert(malformed != NULL);
    memcpy(malformed, bytes, size);
    malformed[0] ^= 0x01u;
    result = previous;
    assert(!run_bytecode_case(malformed, size, 200u, NULL, 0u, NULL,
                                    &result, error, sizeof(error)));
    assert(error[0] != '\0' && result.type == previous.type &&
           result.as.f64 == previous.as.f64);
    free(malformed);

    MilenaVMValue bad_arguments[] = {
        {MILENA_IR_TYPE_F64, {.f64 = 1.0}},
        {MILENA_IR_TYPE_BOOL, {.boolean = true}},
        {MILENA_IR_TYPE_F64, {.f64 = 3.0}}
    };
    result = previous;
    assert(!run_bytecode_case(bytes, size, 100u, bad_arguments, 3u,
                                    NULL, &result, error, sizeof(error)));
    assert(strstr(error, "argument type mismatch") != NULL);
    assert(result.type == previous.type && result.as.f64 == previous.as.f64);
    result = previous;
    assert(!run_bytecode_case(bytes, size, 999u, NULL, 0u, NULL,
                                    &result, error, sizeof(error)));
    assert(strstr(error, "entry function") != NULL);
    assert(result.type == previous.type && result.as.f64 == previous.as.f64);
    free(bytes);
    milena_ir_module_destroy(calls);
}

static void test_original_vm_lifecycle(void) {
    IRInstruction instructions[2] = {{0}};
    instructions[0].opcode = IR_PRINT;
    instructions[1].opcode = IR_PRINT;
    IRProgram program = {0};
    program.instructions = instructions;
    program.count = sizeof(instructions) / sizeof(instructions[0]);
    VirtualMachine vm = {0};
    assert(vm_init(&vm, &program));
    assert(vm.mode == MILENA_VM_MODE_ORIGINAL_IR);
    vm_step(&vm);
    assert(vm.pc == 1u && vm.running);
    assert(vm_run(&vm));
    assert(vm.pc == 2u);
    vm_destroy(&vm);
}

int main(void) {
    test_original_vm_lifecycle();
    test_roundtrip_module_with_direct_call();
    test_rejects_bad_headers_and_lengths();
    test_rejects_signature_mismatch_and_invalid_source_module();
    test_vm_branch_ssa_merge();
    test_vm_forward_call_and_call_depth_limit();
    test_vm_errors_and_limits();
    puts("typed bytecode verifier and internal reference VM tests passed");
    return 0;
}
