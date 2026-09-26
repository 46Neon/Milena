#include "typed_bytecode.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BYTECODE_HEADER_SIZE 16u
#define BYTECODE_MAX_FUNCTIONS 1024u
#define BYTECODE_MAX_NAME_SIZE 4096u
/* IR validation solves dominators with an O(blocks^2) matrix; keep hostile
   files from amplifying a small blob into unbounded verifier work/memory. */
#define BYTECODE_MAX_BLOCKS 512u
#define BYTECODE_MAX_ITEMS 1000000u
#define BYTECODE_BLOCK_RECORD_SIZE 24u
#define BYTECODE_PARAMETER_RECORD_SIZE 12u
#define BYTECODE_EDGE_RECORD_SIZE 16u
#define BYTECODE_CALL_ARGUMENT_RECORD_SIZE 4u
#define BYTECODE_INSTRUCTION_RECORD_SIZE 52u

static const uint8_t bytecode_magic[4] = {'M', 'L', 'B', 'C'};

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    char *error;
    size_t error_capacity;
} ByteWriter;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t position;
    char *error;
    size_t error_capacity;
} ByteReader;

static bool bytecode_error(char *error, size_t capacity, const char *format, ...) {
    if (error && capacity) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(error, capacity, format, args);
        va_end(args);
    }
    return false;
}

static bool writer_reserve(ByteWriter *writer, size_t extra) {
    size_t needed;
    size_t capacity;
    uint8_t *grown;
    if (extra > MILENA_BYTECODE_MAX_SIZE - writer->length)
        return bytecode_error(writer->error, writer->error_capacity,
                              "typed bytecode exceeds the 64 MiB format limit");
    needed = writer->length + extra;
    if (needed <= writer->capacity) return true;
    capacity = writer->capacity ? writer->capacity : 256u;
    while (capacity < needed) {
        if (capacity > MILENA_BYTECODE_MAX_SIZE / 2u) {
            capacity = MILENA_BYTECODE_MAX_SIZE;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed)
        return bytecode_error(writer->error, writer->error_capacity,
                              "typed bytecode buffer size overflow");
    grown = (uint8_t *)realloc(writer->data, capacity);
    if (!grown)
        return bytecode_error(writer->error, writer->error_capacity,
                              "out of memory growing typed bytecode buffer");
    writer->data = grown;
    writer->capacity = capacity;
    return true;
}

static bool writer_bytes(ByteWriter *writer, const void *data, size_t size) {
    if (!writer_reserve(writer, size)) return false;
    if (size) memcpy(writer->data + writer->length, data, size);
    writer->length += size;
    return true;
}

static bool writer_u8(ByteWriter *writer, uint8_t value) {
    return writer_bytes(writer, &value, 1u);
}

static bool writer_u16(ByteWriter *writer, uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_u32(ByteWriter *writer, uint32_t value) {
    uint8_t bytes[4];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> (i * 8u));
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_u64(ByteWriter *writer, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> (i * 8u));
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_zeroes(ByteWriter *writer, size_t count) {
    static const uint8_t zeroes[8] = {0};
    while (count) {
        size_t chunk = count < sizeof(zeroes) ? count : sizeof(zeroes);
        if (!writer_bytes(writer, zeroes, chunk)) return false;
        count -= chunk;
    }
    return true;
}

static bool reader_bytes(ByteReader *reader, void *output, size_t size) {
    if (size > reader->length - reader->position)
        return bytecode_error(reader->error, reader->error_capacity,
                              "truncated typed bytecode");
    if (size && output) memcpy(output, reader->data + reader->position, size);
    reader->position += size;
    return true;
}

static bool reader_u8(ByteReader *reader, uint8_t *value) {
    return reader_bytes(reader, value, 1u);
}

static bool reader_u16(ByteReader *reader, uint16_t *value) {
    uint8_t bytes[2];
    if (!reader_bytes(reader, bytes, sizeof(bytes))) return false;
    *value = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
    return true;
}

static bool reader_u32(ByteReader *reader, uint32_t *value) {
    uint8_t bytes[4];
    if (!reader_bytes(reader, bytes, sizeof(bytes))) return false;
    *value = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i)
        *value |= (uint32_t)bytes[i] << (i * 8u);
    return true;
}

static bool reader_u64(ByteReader *reader, uint64_t *value) {
    uint8_t bytes[8];
    if (!reader_bytes(reader, bytes, sizeof(bytes))) return false;
    *value = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i)
        *value |= (uint64_t)bytes[i] << (i * 8u);
    return true;
}

static bool reader_reserved_zeroes(ByteReader *reader, size_t count) {
    uint8_t bytes[8];
    while (count) {
        size_t chunk = count < sizeof(bytes) ? count : sizeof(bytes);
        if (!reader_bytes(reader, bytes, chunk)) return false;
        for (size_t i = 0; i < chunk; ++i)
            if (bytes[i] != 0)
                return bytecode_error(reader->error, reader->error_capacity,
                                      "nonzero reserved byte in typed bytecode");
        count -= chunk;
    }
    return true;
}

static uint64_t encode_signed_i64(int64_t value) {
    /* Conversion to unsigned is defined modulo 2^64 by C. The file format
       defines signed integers as two's-complement 64-bit values. */
    return (uint64_t)value;
}

static int64_t decode_signed_i64(uint64_t bits) {
    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;
    return -1 - (int64_t)(UINT64_MAX - bits);
}

static bool host_has_binary64(void) {
    double one = 1.0;
    double negative_zero = -0.0;
    uint64_t one_bits = 0;
    uint64_t negative_zero_bits = 0;
    if (sizeof(double) != sizeof(uint64_t) || FLT_RADIX != 2 ||
        DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024) return false;
    memcpy(&one_bits, &one, sizeof(one_bits));
    memcpy(&negative_zero_bits, &negative_zero, sizeof(negative_zero_bits));
    return one_bits == UINT64_C(0x3ff0000000000000) &&
           negative_zero_bits == UINT64_C(0x8000000000000000);
}

static bool write_float64(ByteWriter *writer, double value) {
    uint64_t bits;
    if (!host_has_binary64())
        return bytecode_error(writer->error, writer->error_capacity,
                              "host double format is not IEEE-754 binary64");
    memcpy(&bits, &value, sizeof(bits));
    return writer_u64(writer, bits);
}

static bool read_float64(ByteReader *reader, double *value) {
    uint64_t bits;
    if (!host_has_binary64())
        return bytecode_error(reader->error, reader->error_capacity,
                              "host double format is not IEEE-754 binary64");
    if (!reader_u64(reader, &bits)) return false;
    memcpy(value, &bits, sizeof(bits));
    return true;
}

static bool writer_count(ByteWriter *writer, size_t count, size_t maximum,
                         const char *what) {
    if (count > maximum || count > UINT32_MAX)
        return bytecode_error(writer->error, writer->error_capacity,
                              "%s count exceeds typed bytecode limit", what);
    return writer_u32(writer, (uint32_t)count);
}

static bool writer_type(ByteWriter *writer, MilenaIRType type) {
    if (type <= MILENA_IR_TYPE_INVALID || type > MILENA_IR_TYPE_VOID)
        return bytecode_error(writer->error, writer->error_capacity,
                              "invalid IR type while encoding bytecode");
    return writer_u8(writer, (uint8_t)type);
}

static bool encode_program(ByteWriter *writer, const MilenaIRProgram *program) {
    if (!program || !program->has_function_signature)
        return bytecode_error(writer->error, writer->error_capacity,
                              "typed IR function has no explicit signature");
    if (!writer_count(writer, program->block_count, BYTECODE_MAX_BLOCKS, "block") ||
        !writer_count(writer, program->parameter_count, BYTECODE_MAX_ITEMS, "block parameter") ||
        !writer_count(writer, program->edge_argument_count, BYTECODE_MAX_ITEMS, "edge argument") ||
        !writer_count(writer, program->call_argument_count, BYTECODE_MAX_ITEMS, "call argument") ||
        !writer_count(writer, program->count, BYTECODE_MAX_ITEMS, "instruction"))
        return false;

    if (!writer_type(writer, program->signature.return_type) ||
        !writer_count(writer, program->signature.parameter_count, BYTECODE_MAX_ITEMS,
                      "function parameter"))
        return false;
    for (size_t i = 0; i < program->signature.parameter_count; ++i)
        if (!writer_type(writer, program->signature.parameter_types[i])) return false;

    for (size_t i = 0; i < program->block_count; ++i) {
        const MilenaIRBasicBlock *block = &program->blocks[i];
        if (!writer_u32(writer, block->id) ||
            !writer_u32(writer, (uint32_t)block->first_instruction) ||
            !writer_u32(writer, (uint32_t)block->instruction_count) ||
            !writer_u32(writer, block->successor_true) ||
            !writer_u32(writer, block->successor_false) ||
            !writer_u8(writer, block->terminated ? 1u : 0u) ||
            !writer_zeroes(writer, 3u)) return false;
    }
    for (size_t i = 0; i < program->parameter_count; ++i) {
        const MilenaIRBlockParameter *parameter = &program->parameters[i];
        if (!writer_u32(writer, parameter->block_id) ||
            !writer_u32(writer, parameter->value_id) ||
            !writer_type(writer, parameter->type) || !writer_zeroes(writer, 3u))
            return false;
    }
    for (size_t i = 0; i < program->edge_argument_count; ++i) {
        const MilenaIREdgeArgument *argument = &program->edge_arguments[i];
        if (!writer_u32(writer, argument->source_block_id) ||
            !writer_u32(writer, argument->target_block_id) ||
            !writer_u32(writer, argument->parameter_index) ||
            !writer_u32(writer, argument->value_id)) return false;
    }
    for (size_t i = 0; i < program->call_argument_count; ++i)
        if (!writer_u32(writer, program->call_arguments[i])) return false;
    for (size_t i = 0; i < program->count; ++i) {
        const MilenaIRInstruction *instruction = &program->instructions[i];
        if (instruction->opcode < MILENA_IR_CONST_I64 ||
            instruction->opcode >= MILENA_IR_OPCODE_COUNT ||
            !writer_u16(writer, (uint16_t)instruction->opcode) ||
            !writer_type(writer, instruction->result_type) ||
            !writer_u8(writer, 0u) || !writer_u32(writer, instruction->result_id) ||
            !writer_u32(writer, instruction->operand1_id) ||
            !writer_u32(writer, instruction->operand2_id) ||
            !writer_u32(writer, instruction->block_id) ||
            !writer_count(writer, instruction->call_argument_offset,
                          BYTECODE_MAX_ITEMS, "call argument offset") ||
            !writer_count(writer, instruction->call_argument_count,
                          BYTECODE_MAX_ITEMS, "call argument slice") ||
            !writer_u64(writer, encode_signed_i64(instruction->integer_immediate)) ||
            !write_float64(writer, instruction->float_immediate) ||
            !writer_u32(writer, instruction->target_true) ||
            !writer_u32(writer, instruction->target_false)) return false;
    }
    return true;
}

bool milena_bytecode_encode_module(const MilenaIRModule *module,
                                   uint8_t **bytes_out, size_t *size_out,
                                   char *error, size_t error_capacity) {
    ByteWriter writer = {0};
    size_t payload_size;
    if (error && error_capacity) error[0] = '\0';
    if (bytes_out) *bytes_out = NULL;
    if (size_out) *size_out = 0;
    if (!bytes_out || !size_out || !module)
        return bytecode_error(error, error_capacity,
                              "bytecode encoder requires module and output pointers");
    if (module->function_count == 0 ||
        module->function_count > BYTECODE_MAX_FUNCTIONS)
        return bytecode_error(error, error_capacity,
                              "IR module function count exceeds bytecode limit");
    if (!milena_ir_module_validate(module, error, error_capacity)) return false;
    for (size_t i = 0; i < module->function_count; ++i)
        if (module->functions[i].body->module_context != module)
            return bytecode_error(error, error_capacity,
                                  "IR function is not bound to the module being encoded");

    writer.error = error;
    writer.error_capacity = error_capacity;
    if (!writer_bytes(&writer, bytecode_magic, sizeof(bytecode_magic)) ||
        !writer_u16(&writer, MILENA_BYTECODE_VERSION_MAJOR) ||
        !writer_u16(&writer, MILENA_BYTECODE_VERSION_MINOR) ||
        !writer_u32(&writer, 0u) || !writer_u32(&writer, 0u) ||
        !writer_count(&writer, module->function_count, BYTECODE_MAX_FUNCTIONS,
                      "function")) goto fail;

    for (size_t i = 0; i < module->function_count; ++i) {
        const MilenaIRModuleFunction *function = &module->functions[i];
        size_t name_length = strlen(function->name);
        if (name_length == 0 || name_length > BYTECODE_MAX_NAME_SIZE ||
            name_length > UINT32_MAX) {
            bytecode_error(error, error_capacity,
                           "function name length exceeds bytecode limit");
            goto fail;
        }
        if (!writer_u32(&writer, function->symbol_id) ||
            !writer_u32(&writer, (uint32_t)name_length) ||
            !writer_bytes(&writer, function->name, name_length) ||
            !writer_count(&writer, function->parameter_count, BYTECODE_MAX_ITEMS,
                          "function parameter") ||
            !writer_type(&writer, function->return_type)) goto fail;
        for (size_t p = 0; p < function->parameter_count; ++p)
            if (!writer_type(&writer, function->parameter_types[p])) goto fail;
        if (!encode_program(&writer, function->body)) goto fail;
    }

    payload_size = writer.length - BYTECODE_HEADER_SIZE;
    if (payload_size > UINT32_MAX) {
        bytecode_error(error, error_capacity, "typed bytecode payload is too large");
        goto fail;
    }
    for (size_t i = 0; i < 4u; ++i)
        writer.data[12u + i] = (uint8_t)((uint32_t)payload_size >> (i * 8u));
    *bytes_out = writer.data;
    *size_out = writer.length;
    return true;
fail:
    free(writer.data);
    return false;
}

static bool reader_count(ByteReader *reader, uint32_t maximum, size_t record_size,
                         uint32_t *count, const char *what) {
    if (!reader_u32(reader, count)) return false;
    if (*count > maximum || (record_size &&
        (size_t)*count > (reader->length - reader->position) / record_size))
        return bytecode_error(reader->error, reader->error_capacity,
                              "%s count exceeds remaining bytecode or format limit", what);
    return true;
}

static bool reader_type(ByteReader *reader, MilenaIRType *type) {
    uint8_t raw;
    if (!reader_u8(reader, &raw)) return false;
    if (raw <= MILENA_IR_TYPE_INVALID || raw > MILENA_IR_TYPE_VOID)
        return bytecode_error(reader->error, reader->error_capacity,
                              "invalid IR type in typed bytecode");
    *type = (MilenaIRType)raw;
    return true;
}

static bool read_name(ByteReader *reader, char **name) {
    uint32_t length;
    char *copy;
    if (!reader_u32(reader, &length)) return false;
    if (!length || length > BYTECODE_MAX_NAME_SIZE ||
        length > reader->length - reader->position)
        return bytecode_error(reader->error, reader->error_capacity,
                              "invalid or truncated function name in typed bytecode");
    copy = (char *)malloc((size_t)length + 1u);
    if (!copy)
        return bytecode_error(reader->error, reader->error_capacity,
                              "out of memory decoding function name");
    if (!reader_bytes(reader, copy, length)) { free(copy); return false; }
    if (memchr(copy, '\0', length)) {
        free(copy);
        return bytecode_error(reader->error, reader->error_capacity,
                              "embedded NUL in typed bytecode function name");
    }
    copy[length] = '\0';
    *name = copy;
    return true;
}

static bool decode_program(ByteReader *reader, MilenaIRProgram *program) {
    uint32_t block_count, parameter_count, edge_count;
    uint32_t call_argument_count, instruction_count;
    uint32_t signature_parameter_count;
    MilenaIRType return_type;
    MilenaIRType *signature_parameters = NULL;
    if (!reader_count(reader, BYTECODE_MAX_BLOCKS, BYTECODE_BLOCK_RECORD_SIZE,
                      &block_count, "block")) return false;
    if (block_count == 0)
        return bytecode_error(reader->error, reader->error_capacity,
                              "typed IR function has no basic blocks");
    if (!reader_count(reader, BYTECODE_MAX_ITEMS, BYTECODE_PARAMETER_RECORD_SIZE,
                      &parameter_count, "block parameter") ||
        !reader_count(reader, BYTECODE_MAX_ITEMS, BYTECODE_EDGE_RECORD_SIZE,
                      &edge_count, "edge argument") ||
        !reader_count(reader, BYTECODE_MAX_ITEMS, BYTECODE_CALL_ARGUMENT_RECORD_SIZE,
                      &call_argument_count, "call argument") ||
        !reader_count(reader, BYTECODE_MAX_ITEMS, BYTECODE_INSTRUCTION_RECORD_SIZE,
                      &instruction_count, "instruction")) return false;
    if (instruction_count == 0)
        return bytecode_error(reader->error, reader->error_capacity,
                              "typed IR function has no instructions");
    if (!reader_type(reader, &return_type) ||
        !reader_count(reader, BYTECODE_MAX_ITEMS, 1u, &signature_parameter_count,
                      "function parameter")) return false;
    if (signature_parameter_count) {
        if ((size_t)signature_parameter_count >
            SIZE_MAX / sizeof(*signature_parameters))
            return bytecode_error(reader->error, reader->error_capacity,
                                  "function signature allocation size overflow");
        signature_parameters = (MilenaIRType *)calloc(signature_parameter_count,
                                                        sizeof(*signature_parameters));
        if (!signature_parameters)
            return bytecode_error(reader->error, reader->error_capacity,
                                  "out of memory decoding function signature");
    }
    for (uint32_t i = 0; i < signature_parameter_count; ++i) {
        if (!reader_type(reader, &signature_parameters[i])) {
            free(signature_parameters);
            return false;
        }
    }
    bool signature_ok = milena_ir_program_set_function_signature(program,
        signature_parameters, signature_parameter_count, return_type);
    free(signature_parameters);
    if (!signature_ok)
        return bytecode_error(reader->error, reader->error_capacity,
                              "invalid function signature in typed bytecode");

    /* Individual count checks below cannot prove that all arrays fit together.
       Reject their combined wire size before allocating to prevent malformed
       counts from causing avoidable memory amplification. */
    size_t encoded_array_bytes =
        (size_t)block_count * BYTECODE_BLOCK_RECORD_SIZE +
        (size_t)parameter_count * BYTECODE_PARAMETER_RECORD_SIZE +
        (size_t)edge_count * BYTECODE_EDGE_RECORD_SIZE +
        (size_t)call_argument_count * BYTECODE_CALL_ARGUMENT_RECORD_SIZE +
        (size_t)instruction_count * BYTECODE_INSTRUCTION_RECORD_SIZE;
    if (encoded_array_bytes > reader->length - reader->position)
        return bytecode_error(reader->error, reader->error_capacity,
                              "typed IR arrays exceed remaining bytecode length");

    if ((size_t)block_count > SIZE_MAX / sizeof(*program->blocks) ||
        (size_t)parameter_count > SIZE_MAX / sizeof(*program->parameters) ||
        (size_t)edge_count > SIZE_MAX / sizeof(*program->edge_arguments) ||
        (size_t)call_argument_count > SIZE_MAX / sizeof(*program->call_arguments) ||
        (size_t)instruction_count > SIZE_MAX / sizeof(*program->instructions))
        return bytecode_error(reader->error, reader->error_capacity,
                              "typed bytecode allocation size overflow");
    program->blocks = (MilenaIRBasicBlock *)calloc(block_count, sizeof(*program->blocks));
    program->parameters = parameter_count ?
        (MilenaIRBlockParameter *)calloc(parameter_count, sizeof(*program->parameters)) : NULL;
    program->edge_arguments = edge_count ?
        (MilenaIREdgeArgument *)calloc(edge_count, sizeof(*program->edge_arguments)) : NULL;
    program->call_arguments = call_argument_count ?
        (uint32_t *)calloc(call_argument_count, sizeof(*program->call_arguments)) : NULL;
    program->instructions = (MilenaIRInstruction *)calloc(instruction_count,
                                                            sizeof(*program->instructions));
    if (!program->blocks || (parameter_count && !program->parameters) ||
        (edge_count && !program->edge_arguments) ||
        (call_argument_count && !program->call_arguments) || !program->instructions)
        return bytecode_error(reader->error, reader->error_capacity,
                              "out of memory decoding typed IR arrays");
    program->block_count = program->block_capacity = block_count;
    program->parameter_count = program->parameter_capacity = parameter_count;
    program->edge_argument_count = program->edge_argument_capacity = edge_count;
    program->call_argument_count = program->call_argument_capacity = call_argument_count;
    program->count = program->capacity = instruction_count;

    for (uint32_t i = 0; i < block_count; ++i) {
        MilenaIRBasicBlock *block = &program->blocks[i];
        uint32_t first_instruction, item_count;
        uint8_t terminated;
        if (!reader_u32(reader, &block->id) ||
            !reader_u32(reader, &first_instruction) ||
            !reader_u32(reader, &item_count) ||
            !reader_u32(reader, &block->successor_true) ||
            !reader_u32(reader, &block->successor_false) ||
            !reader_u8(reader, &terminated) || !reader_reserved_zeroes(reader, 3u))
            return false;
        if (terminated > 1u)
            return bytecode_error(reader->error, reader->error_capacity,
                                  "invalid block terminator flag in typed bytecode");
        block->first_instruction = first_instruction;
        block->instruction_count = item_count;
        block->terminated = terminated != 0;
    }
    for (uint32_t i = 0; i < parameter_count; ++i) {
        MilenaIRBlockParameter *parameter = &program->parameters[i];
        if (!reader_u32(reader, &parameter->block_id) ||
            !reader_u32(reader, &parameter->value_id) ||
            !reader_type(reader, &parameter->type) ||
            !reader_reserved_zeroes(reader, 3u)) return false;
    }
    for (uint32_t i = 0; i < edge_count; ++i) {
        MilenaIREdgeArgument *argument = &program->edge_arguments[i];
        if (!reader_u32(reader, &argument->source_block_id) ||
            !reader_u32(reader, &argument->target_block_id) ||
            !reader_u32(reader, &argument->parameter_index) ||
            !reader_u32(reader, &argument->value_id)) return false;
    }
    for (uint32_t i = 0; i < call_argument_count; ++i)
        if (!reader_u32(reader, &program->call_arguments[i])) return false;
    for (uint32_t i = 0; i < instruction_count; ++i) {
        MilenaIRInstruction *instruction = &program->instructions[i];
        uint32_t call_argument_offset, instruction_call_argument_count;
        uint16_t opcode;
        uint8_t raw_type, reserved;
        uint64_t integer_bits;
        if (!reader_u16(reader, &opcode) || !reader_u8(reader, &raw_type) ||
            !reader_u8(reader, &reserved) || !reader_u32(reader, &instruction->result_id) ||
            !reader_u32(reader, &instruction->operand1_id) ||
            !reader_u32(reader, &instruction->operand2_id) ||
            !reader_u32(reader, &instruction->block_id) ||
            !reader_u32(reader, &call_argument_offset) ||
            !reader_u32(reader, &instruction_call_argument_count) ||
            !reader_u64(reader, &integer_bits) ||
            !read_float64(reader, &instruction->float_immediate) ||
            !reader_u32(reader, &instruction->target_true) ||
            !reader_u32(reader, &instruction->target_false)) return false;
        if (reserved != 0u || opcode >= MILENA_IR_OPCODE_COUNT ||
            raw_type <= MILENA_IR_TYPE_INVALID || raw_type > MILENA_IR_TYPE_VOID)
            return bytecode_error(reader->error, reader->error_capacity,
                                  "unsupported opcode, type, or flags in typed bytecode");
        instruction->opcode = (MilenaIROpCode)opcode;
        instruction->result_type = (MilenaIRType)raw_type;
        instruction->call_argument_offset = call_argument_offset;
        instruction->call_argument_count = instruction_call_argument_count;
        instruction->integer_immediate = decode_signed_i64(integer_bits);
    }
    return true;
}

bool milena_bytecode_decode_module(const uint8_t *bytes, size_t size,
                                   MilenaIRModule **module_out,
                                   char *error, size_t error_capacity) {
    ByteReader reader = {0};
    uint8_t magic[4];
    uint16_t major, minor;
    uint32_t flags, payload_size, function_count;
    MilenaIRModule *module = NULL;
    if (error && error_capacity) error[0] = '\0';
    if (module_out) *module_out = NULL;
    if (!bytes || !module_out || size < BYTECODE_HEADER_SIZE ||
        size > MILENA_BYTECODE_MAX_SIZE)
        return bytecode_error(error, error_capacity,
                              "typed bytecode size is invalid or outside format limits");
    reader.data = bytes;
    reader.length = size;
    reader.error = error;
    reader.error_capacity = error_capacity;
    if (!reader_bytes(&reader, magic, sizeof(magic)) ||
        memcmp(magic, bytecode_magic, sizeof(magic)) != 0 ||
        !reader_u16(&reader, &major) || !reader_u16(&reader, &minor) ||
        !reader_u32(&reader, &flags) || !reader_u32(&reader, &payload_size))
        return bytecode_error(error, error_capacity,
                              "invalid or truncated typed bytecode header");
    if (major != MILENA_BYTECODE_VERSION_MAJOR ||
        minor != MILENA_BYTECODE_VERSION_MINOR || flags != 0u)
        return bytecode_error(error, error_capacity,
                              "unsupported typed bytecode version or flags");
    if (payload_size != size - BYTECODE_HEADER_SIZE)
        return bytecode_error(error, error_capacity,
                              "typed bytecode payload length does not match file size");
    if (!reader_count(&reader, BYTECODE_MAX_FUNCTIONS, 0u,
                      &function_count, "function") || function_count == 0)
        return bytecode_error(error, error_capacity,
                              "typed bytecode module has no functions");

    module = (MilenaIRModule *)calloc(1, sizeof(*module));
    if (!module)
        return bytecode_error(error, error_capacity,
                              "out of memory allocating typed bytecode module");
    module->functions = (MilenaIRModuleFunction *)calloc(function_count,
                                                          sizeof(*module->functions));
    if (!module->functions) {
        free(module);
        return bytecode_error(error, error_capacity,
                              "out of memory allocating typed bytecode functions");
    }
    module->function_count = function_count;
    for (uint32_t i = 0; i < function_count; ++i) {
        MilenaIRModuleFunction *function = &module->functions[i];
        uint32_t parameter_count;
        if (!reader_u32(&reader, &function->symbol_id) ||
            !read_name(&reader, &function->name) ||
            !reader_count(&reader, BYTECODE_MAX_ITEMS, 1u, &parameter_count,
                          "function parameter")) goto fail;
        function->parameter_count = parameter_count;
        if (!reader_type(&reader, &function->return_type)) goto fail;
        if (parameter_count) {
            function->parameter_types = (MilenaIRType *)calloc(
                parameter_count, sizeof(*function->parameter_types));
            if (!function->parameter_types) {
                bytecode_error(error, error_capacity,
                               "out of memory decoding function signature");
                goto fail;
            }
        }
        for (uint32_t p = 0; p < parameter_count; ++p)
            if (!reader_type(&reader, &function->parameter_types[p])) goto fail;
        function->body = milena_ir_program_create();
        if (!function->body) {
            bytecode_error(error, error_capacity,
                           "out of memory allocating typed IR function body");
            goto fail;
        }
        if (!decode_program(&reader, function->body)) goto fail;
        if (function->body->signature.parameter_count != function->parameter_count ||
            function->body->signature.return_type != function->return_type) {
            bytecode_error(error, error_capacity,
                           "function and body signatures disagree in typed bytecode");
            goto fail;
        }
        for (uint32_t p = 0; p < parameter_count; ++p)
            if (function->body->signature.parameter_types[p] !=
                function->parameter_types[p]) {
                bytecode_error(error, error_capacity,
                               "function parameter signature disagrees with body");
                goto fail;
            }
    }
    if (reader.position != reader.length) {
        bytecode_error(error, error_capacity,
                       "trailing bytes after typed bytecode module");
        goto fail;
    }
    for (size_t i = 0; i < module->function_count; ++i)
        module->functions[i].body->module_context = module;
    if (!milena_ir_module_validate(module, error, error_capacity)) goto fail;
    *module_out = module;
    return true;
fail:
    milena_ir_module_destroy(module);
    return false;
}

bool milena_bytecode_verify(const uint8_t *bytes, size_t size,
                            char *error, size_t error_capacity) {
    MilenaIRModule *module = NULL;
    if (!milena_bytecode_decode_module(bytes, size, &module, error, error_capacity))
        return false;
    milena_ir_module_destroy(module);
    return true;
}
