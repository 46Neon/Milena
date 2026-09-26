#ifndef MILENA_BYTECODE_H
#define MILENA_BYTECODE_H

/*
 * Portable scalar Milena bytecode v1.0/v1.1/v1.2 plus an explicit v1.3
 * data-plan wire API. This is a versioned, little-endian format, not native
 * machine code. Scalar verification and execution remain limited to v1.2.
 * v1.0 is single-function; v1.1 adds bounded non-recursive scalar calls;
 * v1.2 adds explicit static numeric/boolean register and function-return types.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MILENA_BYTECODE_VERSION_MAJOR 1u
/* v1.0 legacy; v1.1 adds FUNC/CALL; v1.2 adds typed metadata. */
#define MILENA_BYTECODE_VERSION_MINOR 0u
#define MILENA_BYTECODE_V1_MINOR 0u
#define MILENA_BYTECODE_VERSION_CALL_MINOR 1u
#define MILENA_BYTECODE_VERSION_TYPED_MINOR 2u
/* Explicit wire-only data-plan schema; scalar verifier/run stay at v1.2. */
#define MILENA_BYTECODE_VERSION_DATA_MINOR 3u
#define MILENA_BYTECODE_MAX_MINOR MILENA_BYTECODE_VERSION_TYPED_MINOR
#define MILENA_BYTECODE_HEADER_SIZE 16u
#define MILENA_BYTECODE_DATA_MAX_MODULE_BYTES 65536u
#define MILENA_BYTECODE_DATA_MAX_STRING_BYTES 4096u
#define MILENA_BYTECODE_DATA_MAX_TOTAL_STRING_BYTES 16384u
#define MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES 67108864u
#define MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS 5000u
#define MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS 70u
#define MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES 1048576u
#define MILENA_BYTECODE_DATA_MAX_OUTPUT_ROWS 1u
#define MILENA_BYTECODE_INSTRUCTION_SIZE 24u
#define MILENA_BYTECODE_MAX_INSTRUCTIONS 65536u
#define MILENA_BYTECODE_MAX_REGISTERS 256u
#define MILENA_BYTECODE_MAX_FUNCTIONS 64u
#define MILENA_BYTECODE_MAX_CALL_DEPTH 64u
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
    MILENA_BC_RETURN = 16,
    /* v1.1: a=function id, b=parameter base, c=arity; entry marker. */
    MILENA_BC_FUNCTION = 17,
    /* v1.1: a=destination, b=function id, c=argument base, immediate=arity. */
    MILENA_BC_CALL = 18,
    /* v1.2: a=destination, immediate is exactly 0.0 or 1.0. */
    MILENA_BC_CONST_BOOL = 19
} MilenaBytecodeOpcode;

typedef enum {
    MILENA_BC_TYPE_NUMBER = 1,
    MILENA_BC_TYPE_BOOLEAN = 2
} MilenaBytecodeType;

typedef struct {
    uint8_t opcode;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    /* CONST_F64/CONST_BOOL carry a value; v1.1+ CALL carries exact arity. */
    double immediate;
} MilenaBytecodeInstruction;

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t register_count;
    size_t instruction_count;
    const MilenaBytecodeInstruction *instructions;
    /* v1.2 only: one type per register and one return type per function. */
    const uint8_t *register_types;
    size_t register_type_count;
    const uint8_t *function_return_types;
    size_t function_return_type_count;
} MilenaBytecodeProgram;

typedef struct {
    /* Zero selects the default for that limit. Values above hard caps fail. */
    uint32_t max_instructions;
    uint16_t max_registers;
    size_t max_bytecode_bytes;
    uint64_t max_steps;
    /* Zero selects the 64-frame hard default; applies to v1.1+ calls. */
    uint16_t max_call_depth;
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
    MILENA_BC_STEP_LIMIT,
    MILENA_BC_BAD_TYPE
} MilenaBytecodeStatus;

typedef struct {
    MilenaBytecodeStatus status;
    size_t byte_offset;
    char message[128];
} MilenaBytecodeDiagnostic;

/* v1.3 data-only summary operation IDs and logical type IDs. */
enum {
    MILENA_BYTECODE_DATA_SUM = 1,
    MILENA_BYTECODE_DATA_COUNT = 2,
    MILENA_BYTECODE_DATA_TYPE_NUMBER = 1
};

/* A length-delimited UTF-8 byte span. It is never required to be NUL-terminated. */
typedef struct {
    const uint8_t *data;
    size_t length;
} MilenaBytecodeDataStringView;

/* Each encoded input cap must be nonzero and no greater than its hard maximum. */
typedef struct {
    uint32_t max_input_file_bytes;
    uint32_t max_input_data_rows;
    uint32_t max_input_columns;
    uint32_t max_csv_field_bytes;
} MilenaBytecodeDataLimits;

/*
 * Encoder input for the fixed v1.3 DATA_PLAN_ONLY shape. The result column is
 * derived as input_column + "_suma" or "_conteo"; the fixed dataset,
 * declaration, operation, output-row and string counts are not caller-settable.
 */
typedef struct {
    MilenaBytecodeDataStringView source_path;
    MilenaBytecodeDataStringView export_path;
    MilenaBytecodeDataStringView input_column;
    uint8_t operation;
    bool span_present;
    uint32_t source_line;
    uint32_t source_column;
    MilenaBytecodeDataLimits limits;
} MilenaBytecodeDataPlan;

/*
 * Verified data-plan view. All string pointers borrow the input bytecode
 * buffer; keep that buffer alive and unmodified for as long as this view is
 * used. `view_out` is assigned only after complete verification and must not
 * overlap `bytes`. The verifier allocates nothing and retains no pointer after
 * return.
 */
typedef struct {
    MilenaBytecodeDataStringView source_path;
    MilenaBytecodeDataStringView export_path;
    MilenaBytecodeDataStringView input_column;
    MilenaBytecodeDataStringView result_column;
    uint8_t operation;
    bool span_present;
    uint32_t source_line;
    uint32_t source_column;
    MilenaBytecodeDataLimits limits;
} MilenaBytecodeDataPlanView;

/*
 * v1.3 wire-only APIs. They accept/produce precisely DATA_PLAN_ONLY modules;
 * the scalar verifier and milena_bytecode_run continue to reject v1.3.
 * `data_encoded_size` is false for an invalid plan or unrepresentable size.
 * Encoder input spans must remain readable through the call and must not
 * overlap the output buffer. A null output and zero capacity on encode is a
 * size query: BUFFER_TOO_SMALL is returned and *written receives the size.
 */
bool milena_bytecode_data_encoded_size(const MilenaBytecodeDataPlan *plan,
                                       size_t *size_out);
MilenaBytecodeStatus milena_bytecode_data_encode(
    const MilenaBytecodeDataPlan *plan,
    uint8_t *out,
    size_t capacity,
    size_t *written,
    MilenaBytecodeDiagnostic *diagnostic);
MilenaBytecodeStatus milena_bytecode_verify_data(
    const uint8_t *bytes,
    size_t length,
    MilenaBytecodeDataPlanView *view_out,
    MilenaBytecodeDiagnostic *diagnostic);

/* Return header + fixed-width record bytes (v1.0/v1.1 base), or false on overflow/cap. */
bool milena_bytecode_encoded_size(size_t instruction_count, size_t *size_out);
/* Return full v1.2 size including its register-type and function-return tables. */
bool milena_bytecode_typed_encoded_size(size_t instruction_count,
                                        uint16_t register_count,
                                        uint16_t function_count,
                                        size_t *size_out);

/*
 * Encode into caller-owned storage. Set out=NULL/capacity=0 to query the
 * required version-specific size; BUFFER_TOO_SMALL is returned and *written
 * receives the size. Invalid programs do not modify the output buffer.
 */
MilenaBytecodeStatus milena_bytecode_encode(
    const MilenaBytecodeProgram *program,
    uint8_t *out,
    size_t capacity,
    size_t *written,
    const MilenaBytecodeLimits *limits,
    MilenaBytecodeDiagnostic *diagnostic);

/* Validate complete wire representation, operands, types, per-function CFG and call graph. */
MilenaBytecodeStatus milena_bytecode_verify(
    const uint8_t *bytes,
    size_t length,
    const MilenaBytecodeLimits *limits,
    MilenaBytecodeDiagnostic *diagnostic);

/* Execute verified bytecode entry function with bounded fuel and call frames. */
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
