#ifndef MILENA_BYTECODE_H
#define MILENA_BYTECODE_H

/*
 * Portable Milena bytecode v1.
 *
 * This is a versioned, little-endian bytecode format. It is not native machine
 * code and does not yet define the compiler's canonical HIR lowering.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MILENA_BYTECODE_VERSION_MAJOR 1u
#define MILENA_BYTECODE_VERSION_MINOR 0u
#define MILENA_BYTECODE_HEADER_SIZE 16u
#define MILENA_BYTECODE_INSTRUCTION_SIZE 24u
#define MILENA_BYTECODE_MAX_INSTRUCTIONS 65536u
#define MILENA_BYTECODE_MAX_REGISTERS 256u
#define MILENA_BYTECODE_DEFAULT_STEP_LIMIT 1000000u
#define MILENA_BYTECODE_MAX_STEP_LIMIT UINT64_C(10000000)

typedef enum {
    MILENA_BC_CONST_F64 = 1,
    MILENA_BC_MOVE = 2,
    MILENA_BC_ADD = 3,
    MILENA_BC_SUB = 4,
    MILENA_BC_MUL = 5,
    MILENA_BC_DIV = 6,
    MILENA_BC_NEG = 7,
    MILENA_BC_EQ = 8,
    MILENA_BC_NE = 9,
    MILENA_BC_LT = 10,
    MILENA_BC_LE = 11,
    MILENA_BC_GT = 12,
    MILENA_BC_GE = 13,
    MILENA_BC_JUMP = 14,
    MILENA_BC_JUMP_IF_FALSE = 15,
    MILENA_BC_RETURN = 16
} MilenaBytecodeOpcode;

typedef struct {
    uint8_t opcode;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    /* Used only by CONST_F64; all other opcodes require 0.0. */
    double immediate;
} MilenaBytecodeInstruction;

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t register_count;
    size_t instruction_count;
    const MilenaBytecodeInstruction *instructions;
} MilenaBytecodeProgram;

typedef struct {
    /* Zero selects the default for that limit. Values above hard caps fail. */
    uint32_t max_instructions;
    uint16_t max_registers;
    size_t max_bytecode_bytes;
    uint64_t max_steps;
} MilenaBytecodeLimits;

typedef enum {
    MILENA_BC_OK = 0,
    MILENA_BC_ARGUMENT,
    MILENA_BC_BUFFER_TOO_SMALL,
    MILENA_BC_BAD_MAGIC,
    MILENA_BC_BAD_VERSION,
    MILENA_BC_BAD_FORMAT,
    MILENA_BC_LIMIT_EXCEEDED,
    MILENA_BC_BAD_OPCODE,
    MILENA_BC_BAD_OPERAND,
    MILENA_BC_BAD_CONTROL_FLOW,
    MILENA_BC_OUT_OF_MEMORY,
    MILENA_BC_RUNTIME_ERROR,
    MILENA_BC_STEP_LIMIT
} MilenaBytecodeStatus;

typedef struct {
    MilenaBytecodeStatus status;
    size_t byte_offset;
    char message[128];
} MilenaBytecodeDiagnostic;

/* Return the canonical encoded size, or false if count overflows/is over cap. */
bool milena_bytecode_encoded_size(size_t instruction_count, size_t *size_out);

/*
 * Encode into caller-owned storage. Set out=NULL/capacity=0 to query the
 * required size; BUFFER_TOO_SMALL is returned and *written receives the size.
 * Invalid programs do not modify the output buffer.
 */
MilenaBytecodeStatus milena_bytecode_encode(
    const MilenaBytecodeProgram *program,
    uint8_t *out,
    size_t capacity,
    size_t *written,
    const MilenaBytecodeLimits *limits,
    MilenaBytecodeDiagnostic *diagnostic);

/* Validate the complete wire representation, instruction operands and CFG. */
MilenaBytecodeStatus milena_bytecode_verify(
    const uint8_t *bytes,
    size_t length,
    const MilenaBytecodeLimits *limits,
    MilenaBytecodeDiagnostic *diagnostic);

/* Execute one verified, single-entry numeric function with bounded fuel. */
MilenaBytecodeStatus milena_bytecode_run(
    const uint8_t *bytes,
    size_t length,
    const MilenaBytecodeLimits *limits,
    double *result,
    MilenaBytecodeDiagnostic *diagnostic);

const char *milena_bytecode_status_name(MilenaBytecodeStatus status);

#ifdef __cplusplus
}
#endif
#endif
