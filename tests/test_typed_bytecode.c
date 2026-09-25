#include "typed_bytecode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    callee->name = copy_text("triple");
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
    assert(append_instruction(callee->body, 1u, MILENA_IR_RETURN, 0u,
        MILENA_IR_TYPE_F64, 1u, 0u, 0, 0.0, 0u, 0u));

    MilenaIRModuleFunction *caller = &module->functions[1];
    caller->name = copy_text("entry");
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
    assert(strcmp(decoded->functions[0].name, "triple") == 0);
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
    /* Each array count independently fits in the remaining payload, but their
       combined records do not. The decoder must reject before allocating them. */
    set_u32_le(mutated, 46u, 35u);  /* block parameter count */
    set_u32_le(mutated, 50u, 29u);  /* edge argument count */
    set_u32_le(mutated, 54u, 100u); /* call argument count */
    set_u32_le(mutated, 58u, 8u);   /* instruction count */
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
        (sizeof("triple") - 1u) + 4u + 1u + 3u + 5u * 4u + 1u + 4u + 3u +
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
    /* First function return type: header + function count + symbol + name
       length + six-byte "triple" + parameter count. */
    mutated[38u] = (uint8_t)MILENA_IR_TYPE_BOOL;
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

int main(void) {
    test_roundtrip_module_with_direct_call();
    test_rejects_bad_headers_and_lengths();
    test_rejects_signature_mismatch_and_invalid_source_module();
    puts("typed bytecode encode/decode/verifier tests passed");
    return 0;
}
