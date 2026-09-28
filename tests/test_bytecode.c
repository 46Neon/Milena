#include "bytecode.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                                \
            return 1;                                                          \
        }                                                                       \
    } while (0)

#define BC_BUFFER_SIZE 1024u

typedef struct {
    uint8_t bytes[BC_BUFFER_SIZE];
    size_t length;
} Encoded;

static int encode_program(const MilenaBytecodeInstruction *instructions,
                          size_t count,
                          uint16_t registers,
                          const MilenaBytecodeLimits *limits,
                          Encoded *encoded,
                          MilenaBytecodeStatus expected_status) {
    MilenaBytecodeProgram program = {
        MILENA_BYTECODE_VERSION_MAJOR,
        MILENA_BYTECODE_VERSION_MINOR,
        registers,
        count,
        instructions,
        NULL, 0, NULL, 0
    };
    MilenaBytecodeDiagnostic diagnostic;
    size_t written = 0;
    memset(encoded->bytes, 0xa5, sizeof(encoded->bytes));
    encoded->length = 0;
    MilenaBytecodeStatus status = milena_bytecode_encode(
        &program, encoded->bytes, sizeof(encoded->bytes), &written, limits,
        &diagnostic);
    if (status != expected_status) {
        fprintf(stderr, "unexpected encode status: got %s, expected %s (%s)\n",
                milena_bytecode_status_name(status),
                milena_bytecode_status_name(expected_status), diagnostic.message);
        return 1;
    }
    if (status == MILENA_BC_OK) {
        encoded->length = written;
    }
    return 0;
}

static bool near(double actual, double expected) {
    return fabs(actual - expected) < 1e-12;
}

int main(void) {
    const MilenaBytecodeInstruction arithmetic[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 2.0},
        {MILENA_BC_CONST_F64, 1, 0, 0, 3.0},
        {MILENA_BC_MUL, 2, 0, 1, 0.0},
        {MILENA_BC_RETURN, 2, 0, 0, 0.0}
    };
    const MilenaBytecodeInstruction conditional[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 0.0},
        {MILENA_BC_CONST_F64, 1, 0, 0, 5.0},
        {MILENA_BC_CONST_F64, 2, 0, 0, 5.0},
        {MILENA_BC_EQ, 3, 1, 2, 0.0},
        {MILENA_BC_JUMP_IF_FALSE, 3, 7, 0, 0.0},
        {MILENA_BC_CONST_F64, 4, 0, 0, 10.0},
        {MILENA_BC_RETURN, 4, 0, 0, 0.0},
        {MILENA_BC_CONST_F64, 4, 0, 0, 20.0},
        {MILENA_BC_RETURN, 4, 0, 0, 0.0}
    };
    const MilenaBytecodeInstruction infinite_loop[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 1.0},
        {MILENA_BC_JUMP_IF_FALSE, 0, 3, 0, 0.0},
        {MILENA_BC_JUMP, 2, 0, 0, 0.0},
        {MILENA_BC_RETURN, 0, 0, 0, 0.0}
    };
    const MilenaBytecodeInstruction divide_by_zero[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 1.0},
        {MILENA_BC_CONST_F64, 1, 0, 0, 0.0},
        {MILENA_BC_DIV, 2, 0, 1, 0.0},
        {MILENA_BC_RETURN, 2, 0, 0, 0.0}
    };
    const MilenaBytecodeInstruction overflow[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, DBL_MAX},
        {MILENA_BC_MUL, 1, 0, 0, 0.0},
        {MILENA_BC_RETURN, 1, 0, 0, 0.0}
    };
    Encoded encoded;
    MilenaBytecodeDiagnostic diagnostic;
    MilenaBytecodeLimits limits = {0, 0, 0, 0, 0};
    double value = -123.0;
    size_t required = 0;

    CHECK(milena_bytecode_encoded_size(4, &required));
    CHECK(required == MILENA_BYTECODE_HEADER_SIZE +
                         4u * MILENA_BYTECODE_INSTRUCTION_SIZE);
    CHECK(!milena_bytecode_encoded_size(0, &required));
    CHECK(!milena_bytecode_encoded_size(
        (size_t)MILENA_BYTECODE_MAX_INSTRUCTIONS + 1u, &required));
    {
        MilenaBytecodeProgram program = {
            MILENA_BYTECODE_VERSION_MAJOR,
            MILENA_BYTECODE_VERSION_MINOR,
            3,
            4,
            arithmetic,
            NULL, 0, NULL, 0
        };
        CHECK(milena_bytecode_encode(&program, NULL, 0, &required, NULL,
                                     &diagnostic) == MILENA_BC_BUFFER_TOO_SMALL);
        CHECK(required == MILENA_BYTECODE_HEADER_SIZE +
                             4u * MILENA_BYTECODE_INSTRUCTION_SIZE);
    }

    CHECK(encode_program(arithmetic, 4, 3, NULL, &encoded, MILENA_BC_OK) == 0);
    CHECK(encoded.length == MILENA_BYTECODE_HEADER_SIZE +
                                4u * MILENA_BYTECODE_INSTRUCTION_SIZE);
    CHECK(memcmp(encoded.bytes, "MLBC", 4) == 0);
    CHECK(encoded.bytes[4] == 1 && encoded.bytes[5] == 0);
    CHECK(milena_bytecode_verify(encoded.bytes, encoded.length, NULL,
                                 &diagnostic) == MILENA_BC_OK);
    CHECK(milena_bytecode_run(encoded.bytes, encoded.length, NULL, &value,
                               &diagnostic) == MILENA_BC_OK);
    CHECK(near(value, 6.0));
    CHECK(encode_program(conditional, 9, 5, NULL, &encoded, MILENA_BC_OK) == 0);
    CHECK(milena_bytecode_run(encoded.bytes, encoded.length, NULL, &value,
                              &diagnostic) == MILENA_BC_OK);
    CHECK(near(value, 10.0));

    CHECK(milena_bytecode_verify(encoded.bytes, encoded.length - 1, NULL,
                                 &diagnostic) == MILENA_BC_BAD_FORMAT);
    {
        uint8_t extra[BC_BUFFER_SIZE];
        memcpy(extra, encoded.bytes, encoded.length);
        extra[encoded.length] = 0;
        CHECK(milena_bytecode_verify(extra, encoded.length + 1, NULL,
                                     &diagnostic) == MILENA_BC_BAD_FORMAT);
    }
    {
        uint8_t corrupted[BC_BUFFER_SIZE];
        memcpy(corrupted, encoded.bytes, encoded.length);
        corrupted[4] = 2;
        CHECK(milena_bytecode_verify(corrupted, encoded.length, NULL,
                                     &diagnostic) == MILENA_BC_BAD_VERSION);
        memcpy(corrupted, encoded.bytes, encoded.length);
        corrupted[MILENA_BYTECODE_HEADER_SIZE + 1] = 1;
        CHECK(milena_bytecode_verify(corrupted, encoded.length, NULL,
                                     &diagnostic) == MILENA_BC_BAD_FORMAT);
    }
    {
        uint8_t mutated[BC_BUFFER_SIZE];
        MilenaBytecodeLimits bounded = {0, 0, 0, 16, 0};
        for (size_t i = 0; i < encoded.length; ++i) {
            for (unsigned bit = 0; bit < 8u; ++bit) {
                memcpy(mutated, encoded.bytes, encoded.length);
                mutated[i] ^= (uint8_t)(1u << bit);
                MilenaBytecodeStatus status = milena_bytecode_verify(
                    mutated, encoded.length, NULL, &diagnostic);
                if (status == MILENA_BC_OK) {
                    status = milena_bytecode_run(mutated, encoded.length,
                                                 &bounded, &value, &diagnostic);
                    CHECK(status == MILENA_BC_OK ||
                          status == MILENA_BC_RUNTIME_ERROR ||
                          status == MILENA_BC_STEP_LIMIT);
                }
            }
        }
    }
    {
        MilenaBytecodeInstruction bad_opcode[] = {
            {255, 0, 0, 0, 0.0},
            {MILENA_BC_RETURN, 0, 0, 0, 0.0}
        };
        memset(encoded.bytes, 0xa5, sizeof(encoded.bytes));
        CHECK(encode_program(bad_opcode, 2, 1, NULL, &encoded,
                             MILENA_BC_BAD_OPCODE) == 0);
        for (size_t i = 0; i < sizeof(encoded.bytes); ++i) {
            CHECK(encoded.bytes[i] == 0xa5);
        }
    }
    {
        const MilenaBytecodeInstruction bad_target[] = {
            {MILENA_BC_JUMP, 2, 0, 0, 0.0},
            {MILENA_BC_RETURN, 0, 0, 0, 0.0}
        };
        CHECK(encode_program(bad_target, 2, 1, NULL, &encoded,
                             MILENA_BC_BAD_OPERAND) == 0);
    }
    {
        const MilenaBytecodeInstruction falls_through[] = {
            {MILENA_BC_CONST_F64, 0, 0, 0, 1.0}
        };
        CHECK(encode_program(falls_through, 1, 1, NULL, &encoded,
                             MILENA_BC_BAD_CONTROL_FLOW) == 0);
    }
    {
        const MilenaBytecodeInstruction bad_register[] = {
            {MILENA_BC_RETURN, 2, 0, 0, 0.0}
        };
        CHECK(encode_program(bad_register, 1, 1, NULL, &encoded,
                             MILENA_BC_BAD_OPERAND) == 0);
    }
    {
        MilenaBytecodeProgram program = {
            MILENA_BYTECODE_VERSION_MAJOR,
            MILENA_BYTECODE_VERSION_MINOR,
            3,
            4,
            arithmetic,
            NULL, 0, NULL, 0
        };
        CHECK(milena_bytecode_encode(&program, encoded.bytes, 1, &required,
                                     NULL, &diagnostic) ==
              MILENA_BC_BUFFER_TOO_SMALL);
        CHECK(required == MILENA_BYTECODE_HEADER_SIZE +
                             4u * MILENA_BYTECODE_INSTRUCTION_SIZE);
    }
    {
        limits.max_steps = 16;
        CHECK(encode_program(infinite_loop, 4, 1, NULL, &encoded,
                             MILENA_BC_OK) == 0);
        CHECK(milena_bytecode_run(encoded.bytes, encoded.length, &limits, &value,
                                  &diagnostic) == MILENA_BC_STEP_LIMIT);
        CHECK(value == 0.0);
        limits.max_steps = 0;
    }
    CHECK(encode_program(divide_by_zero, 4, 3, NULL, &encoded,
                         MILENA_BC_OK) == 0);
    CHECK(milena_bytecode_run(encoded.bytes, encoded.length, NULL, &value,
                              &diagnostic) == MILENA_BC_RUNTIME_ERROR);
    CHECK(value == 0.0);
    CHECK(encode_program(overflow, 3, 2, NULL, &encoded, MILENA_BC_OK) == 0);
    CHECK(milena_bytecode_run(encoded.bytes, encoded.length, NULL, &value,
                              &diagnostic) == MILENA_BC_RUNTIME_ERROR);

    limits.max_steps = MILENA_BYTECODE_MAX_STEP_LIMIT + 1u;
    CHECK(milena_bytecode_verify(encoded.bytes, encoded.length, &limits, NULL) ==
          MILENA_BC_LIMIT_EXCEEDED);

    /* v1.1 extends the old fixed-width record with a validated function table
       and bounded direct-call ABI; the old v1.0 fixtures above remain bytewise. */
    {
        const MilenaBytecodeInstruction calls[] = {
            {MILENA_BC_FUNCTION, 0, 0, 0, 0.0},
            {MILENA_BC_CONST_F64, 0, 0, 0, 4.0},
            {MILENA_BC_MOVE, 1, 0, 0, 0.0},
            {MILENA_BC_CALL, 2, 1, 1, 1.0},
            {MILENA_BC_RETURN, 2, 0, 0, 0.0},
            {MILENA_BC_FUNCTION, 1, 3, 1, 0.0},
            {MILENA_BC_CONST_F64, 4, 0, 0, 2.0},
            {MILENA_BC_MUL, 5, 3, 4, 0.0},
            {MILENA_BC_RETURN, 5, 0, 0, 0.0}
        };
        MilenaBytecodeProgram v11 = {MILENA_BYTECODE_VERSION_MAJOR,
            MILENA_BYTECODE_VERSION_CALL_MINOR, 6, 9, calls,
            NULL, 0, NULL, 0};
        size_t call_length = 0;
        uint8_t call_bytes[BC_BUFFER_SIZE];
        CHECK(milena_bytecode_encode(&v11, call_bytes, sizeof(call_bytes),
                    &call_length, NULL, &diagnostic) == MILENA_BC_OK);
        CHECK(milena_bytecode_run(call_bytes, call_length, NULL, &value,
                                  &diagnostic) == MILENA_BC_OK && near(value, 8.0));
        uint8_t malformed[BC_BUFFER_SIZE];
        memcpy(malformed, call_bytes, call_length);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 3u * MILENA_BYTECODE_INSTRUCTION_SIZE + 8u] = 9;
        CHECK(milena_bytecode_verify(malformed, call_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_OPERAND);
        memcpy(malformed, call_bytes, call_length);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 3u * MILENA_BYTECODE_INSTRUCTION_SIZE + 23u] = 0x40;
        CHECK(milena_bytecode_verify(malformed, call_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_OPERAND);
        memcpy(malformed, call_bytes, call_length);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 5u * MILENA_BYTECODE_INSTRUCTION_SIZE + 8u] = 6;
        CHECK(milena_bytecode_verify(malformed, call_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_OPERAND);
        memcpy(malformed, call_bytes, call_length);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 3u * MILENA_BYTECODE_INSTRUCTION_SIZE + 12u] = 6;
        CHECK(milena_bytecode_verify(malformed, call_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_OPERAND);
        memcpy(malformed, call_bytes, call_length);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 3u * MILENA_BYTECODE_INSTRUCTION_SIZE + 4u] = 6;
        CHECK(milena_bytecode_verify(malformed, call_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_OPERAND);
        MilenaBytecodeLimits excess_frames = {0, 0, 0, 0,
            MILENA_BYTECODE_MAX_CALL_DEPTH + 1u};
        CHECK(milena_bytecode_verify(call_bytes, call_length, &excess_frames,
                                     &diagnostic) == MILENA_BC_LIMIT_EXCEEDED);
        /* A v1.0 header cannot smuggle v1.1 call opcodes into an old stream. */
        memcpy(malformed, call_bytes, call_length);
        malformed[6] = MILENA_BYTECODE_V1_MINOR;
        CHECK(milena_bytecode_verify(malformed, call_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_OPERAND);
    }

    /* v1.2 gives every register a fixed numeric/boolean contract and each
       function a declared return type; v1.0/v1.1 byte streams above remain
       bytewise compatible and intentionally retain their legacy untyped ABI. */
    {
        const MilenaBytecodeInstruction typed_code[] = {
            {MILENA_BC_FUNCTION, 0, 0, 0, 0.0},
            {MILENA_BC_CONST_F64, 0, 0, 0, 4.0},
            {MILENA_BC_CONST_F64, 1, 0, 0, 5.0},
            {MILENA_BC_LT, 2, 0, 1, 0.0},
            {MILENA_BC_CALL, 3, 1, 2, 1.0},
            {MILENA_BC_JUMP_IF_FALSE, 3, 8, 0, 0.0},
            {MILENA_BC_CONST_F64, 6, 0, 0, 9.0},
            {MILENA_BC_RETURN, 6, 0, 0, 0.0},
            {MILENA_BC_CONST_F64, 6, 0, 0, 10.0},
            {MILENA_BC_RETURN, 6, 0, 0, 0.0},
            {MILENA_BC_FUNCTION, 1, 4, 1, 0.0},
            {MILENA_BC_EQ, 5, 4, 4, 0.0},
            {MILENA_BC_RETURN, 5, 0, 0, 0.0}
        };
        const uint8_t register_types[] = {
            MILENA_BC_TYPE_NUMBER, MILENA_BC_TYPE_NUMBER,
            MILENA_BC_TYPE_BOOLEAN, MILENA_BC_TYPE_BOOLEAN,
            MILENA_BC_TYPE_BOOLEAN, MILENA_BC_TYPE_BOOLEAN,
            MILENA_BC_TYPE_NUMBER
        };
        const uint8_t return_types[] = {
            MILENA_BC_TYPE_NUMBER, MILENA_BC_TYPE_BOOLEAN
        };
        MilenaBytecodeProgram typed = {
            MILENA_BYTECODE_VERSION_MAJOR,
            MILENA_BYTECODE_VERSION_TYPED_MINOR,
            7, 13, typed_code,
            register_types, sizeof(register_types),
            return_types, sizeof(return_types)
        };
        uint8_t typed_bytes[BC_BUFFER_SIZE];
        size_t typed_length = 0;
        size_t typed_base = MILENA_BYTECODE_HEADER_SIZE +
                            13u * MILENA_BYTECODE_INSTRUCTION_SIZE;
        size_t expected_typed_size = 0;
        CHECK(milena_bytecode_typed_encoded_size(13, 7, 2,
                                                  &expected_typed_size));
        CHECK(expected_typed_size == typed_base + sizeof(register_types) +
                                            sizeof(return_types));
        CHECK(milena_bytecode_encode(&typed, typed_bytes, sizeof(typed_bytes),
                                     &typed_length, NULL, &diagnostic) == MILENA_BC_OK);
        CHECK(typed_length == expected_typed_size && typed_bytes[6] == 2);
        CHECK(milena_bytecode_verify(typed_bytes, typed_length, NULL,
                                     &diagnostic) == MILENA_BC_OK);
        CHECK(milena_bytecode_run(typed_bytes, typed_length, NULL, &value,
                                  &diagnostic) == MILENA_BC_OK && near(value, 9.0));

        /* Unknown/short/extra type metadata is rejected before execution. */
        uint8_t malformed[BC_BUFFER_SIZE];
        memcpy(malformed, typed_bytes, typed_length);
        malformed[typed_base] = 0xff;
        CHECK(milena_bytecode_verify(malformed, typed_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_FORMAT);
        memcpy(malformed, typed_bytes, typed_length);
        malformed[typed_base + sizeof(register_types)] = 0xff;
        CHECK(milena_bytecode_verify(malformed, typed_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_FORMAT);
        CHECK(milena_bytecode_verify(typed_bytes, typed_length - 1u, NULL,
                                     &diagnostic) == MILENA_BC_BAD_FORMAT);
        memcpy(malformed, typed_bytes, typed_length);
        malformed[typed_length] = 0;
        CHECK(milena_bytecode_verify(malformed, typed_length + 1u, NULL,
                                     &diagnostic) == MILENA_BC_BAD_FORMAT);
        MilenaBytecodeProgram bad_table = typed;
        bad_table.register_type_count--;
        CHECK(milena_bytecode_encode(&bad_table, malformed, sizeof(malformed),
                                     &typed_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_FORMAT);
        bad_table = typed;
        bad_table.function_return_type_count--;
        CHECK(milena_bytecode_encode(&bad_table, malformed, sizeof(malformed),
                                     &typed_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_FORMAT);

        /* Numeric/bool constants and statically inconsistent branch writes. */
        memcpy(malformed, typed_bytes, expected_typed_size);
        malformed[typed_base] = MILENA_BC_TYPE_BOOLEAN;
        CHECK(milena_bytecode_verify(malformed, expected_typed_size, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);
        memcpy(malformed, typed_bytes, expected_typed_size);
        size_t branch_merge_offset = MILENA_BYTECODE_HEADER_SIZE +
                                     8u * MILENA_BYTECODE_INSTRUCTION_SIZE;
        malformed[branch_merge_offset] = MILENA_BC_CONST_BOOL;
        const uint8_t one_bits[8] = {0, 0, 0, 0, 0, 0, 0xf0, 0x3f};
        memcpy(malformed + branch_merge_offset + 16u, one_bits, sizeof(one_bits));
        CHECK(milena_bytecode_verify(malformed, expected_typed_size, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);
        /* The conditional branch itself must consume a declared boolean. */
        memcpy(malformed, typed_bytes, expected_typed_size);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 5u * MILENA_BYTECODE_INSTRUCTION_SIZE + 4u] = 0;
        CHECK(milena_bytecode_verify(malformed, expected_typed_size, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);

        /* Call argument and result registers must match the callee signature. */
        memcpy(malformed, typed_bytes, expected_typed_size);
        malformed[typed_base + 4u] = MILENA_BC_TYPE_NUMBER;
        CHECK(milena_bytecode_verify(malformed, expected_typed_size, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);
        memcpy(malformed, typed_bytes, expected_typed_size);
        malformed[typed_base + 3u] = MILENA_BC_TYPE_NUMBER;
        CHECK(milena_bytecode_verify(malformed, expected_typed_size, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);

        /* A return value must agree with the declared function return type. */
        memcpy(malformed, typed_bytes, expected_typed_size);
        malformed[MILENA_BYTECODE_HEADER_SIZE + 12u * MILENA_BYTECODE_INSTRUCTION_SIZE + 4u] = 6;
        CHECK(milena_bytecode_verify(malformed, expected_typed_size, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);

        /* Ordinary arithmetic cannot consume a boolean-typed register. */
        const MilenaBytecodeInstruction bad_arithmetic[] = {
            {MILENA_BC_FUNCTION, 0, 0, 0, 0.0},
            {MILENA_BC_CONST_BOOL, 0, 0, 0, 1.0},
            {MILENA_BC_ADD, 1, 0, 0, 0.0},
            {MILENA_BC_RETURN, 1, 0, 0, 0.0}
        };
        const uint8_t bad_arithmetic_types[] = {
            MILENA_BC_TYPE_BOOLEAN, MILENA_BC_TYPE_NUMBER
        };
        const uint8_t numeric_return[] = {MILENA_BC_TYPE_NUMBER};
        MilenaBytecodeProgram bad_arithmetic_program = {
            MILENA_BYTECODE_VERSION_MAJOR, MILENA_BYTECODE_VERSION_TYPED_MINOR,
            2, 4, bad_arithmetic, bad_arithmetic_types, 2,
            numeric_return, 1
        };
        CHECK(milena_bytecode_encode(&bad_arithmetic_program, malformed,
                                     sizeof(malformed), &typed_length, NULL,
                                     &diagnostic) == MILENA_BC_BAD_TYPE);

        /* Reject typed metadata smuggled into a legacy v1.1 encoding. */
        typed.minor = MILENA_BYTECODE_VERSION_CALL_MINOR;
        CHECK(milena_bytecode_encode(&typed, malformed, sizeof(malformed),
                                     &typed_length, NULL, &diagnostic) ==
              MILENA_BC_BAD_FORMAT);
    }

    printf("bytecode tests: ok\n");
    return 0;
}
