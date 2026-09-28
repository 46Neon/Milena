#include "bytecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define DATA_BUFFER_CAP 20000u
#define SECTION_START MILENA_BYTECODE_HEADER_SIZE
#define SECTION_LENGTH_OFFSET (SECTION_START + 8u)
#define STRING_TABLE_LENGTH_OFFSET (SECTION_START + 48u)
#define COLUMN_RECORD_OFFSET (SECTION_START + 56u)
#define OPERATION_RECORD_OFFSET (COLUMN_RECORD_OFFSET + 8u)
#define STRING_TABLE_OFFSET (OPERATION_RECORD_OFFSET + 24u)

static const uint8_t source_path[] = "rows.csv";
static const uint8_t export_path[] = "summary.json";
static const uint8_t input_column[] = "valor";

typedef struct {
    uint8_t bytes[DATA_BUFFER_CAP];
    size_t length;
} EncodedData;

static MilenaBytecodeDataPlan make_plan(uint8_t operation) {
    MilenaBytecodeDataPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.source_path.data = source_path;
    plan.source_path.length = sizeof(source_path) - 1u;
    plan.export_path.data = export_path;
    plan.export_path.length = sizeof(export_path) - 1u;
    plan.input_column.data = input_column;
    plan.input_column.length = sizeof(input_column) - 1u;
    plan.operation = operation;
    plan.span_present = true;
    plan.source_line = 7u;
    plan.source_column = 9u;
    plan.limits.max_input_file_bytes = MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES;
    plan.limits.max_input_data_rows = MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS;
    plan.limits.max_input_columns = MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS;
    plan.limits.max_csv_field_bytes = MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES;
    return plan;
}

static void write_u16(uint8_t *bytes, size_t offset, uint16_t value) {
    bytes[offset] = (uint8_t)(value & UINT16_C(0xff));
    bytes[offset + 1u] = (uint8_t)((value >> 8) & UINT16_C(0xff));
}

static void write_u32(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)(value & UINT32_C(0xff));
    bytes[offset + 1u] = (uint8_t)((value >> 8) & UINT32_C(0xff));
    bytes[offset + 2u] = (uint8_t)((value >> 16) & UINT32_C(0xff));
    bytes[offset + 3u] = (uint8_t)((value >> 24) & UINT32_C(0xff));
}

static MilenaBytecodeStatus verify_status(const uint8_t *bytes, size_t length) {
    MilenaBytecodeDiagnostic diagnostic;
    return milena_bytecode_verify_data(bytes, length, NULL, &diagnostic);
}

static int encode_plan(const MilenaBytecodeDataPlan *plan, EncodedData *encoded) {
    MilenaBytecodeDiagnostic diagnostic;
    size_t written = 0u;
    MilenaBytecodeStatus status = milena_bytecode_data_encode(
        plan, encoded->bytes, sizeof(encoded->bytes), &written, &diagnostic);
    if (status != MILENA_BC_OK) {
        fprintf(stderr, "data encode failed: %s (%s)\n",
                milena_bytecode_status_name(status), diagnostic.message);
        return 1;
    }
    encoded->length = written;
    return 0;
}

static int expect_mutation_rejected(const EncodedData *valid,
                                    size_t offset,
                                    uint8_t replacement,
                                    MilenaBytecodeStatus expected) {
    uint8_t *mutated = (uint8_t *)malloc(valid->length + 1u);
    MilenaBytecodeStatus status;
    if (!mutated) return 1;
    memcpy(mutated, valid->bytes, valid->length);
    mutated[offset] = replacement;
    status = verify_status(mutated, valid->length);
    if (status != expected) {
        fprintf(stderr, "mutation at %zu: got %s, expected %s\n", offset,
                milena_bytecode_status_name(status),
                milena_bytecode_status_name(expected));
        free(mutated);
        return 1;
    }
    free(mutated);
    return 0;
}

static int expect_u16_mutation_rejected(const EncodedData *valid,
                                        size_t offset,
                                        uint16_t replacement,
                                        MilenaBytecodeStatus expected) {
    uint8_t *mutated = (uint8_t *)malloc(valid->length + 1u);
    MilenaBytecodeStatus status;
    if (!mutated) return 1;
    memcpy(mutated, valid->bytes, valid->length);
    write_u16(mutated, offset, replacement);
    status = verify_status(mutated, valid->length);
    if (status != expected) {
        fprintf(stderr, "u16 mutation at %zu: got %s, expected %s\n", offset,
                milena_bytecode_status_name(status),
                milena_bytecode_status_name(expected));
        free(mutated);
        return 1;
    }
    free(mutated);
    return 0;
}

static int expect_u32_mutation_rejected(const EncodedData *valid,
                                        size_t offset,
                                        uint32_t replacement,
                                        MilenaBytecodeStatus expected) {
    uint8_t *mutated = (uint8_t *)malloc(valid->length + 1u);
    MilenaBytecodeStatus status;
    if (!mutated) return 1;
    memcpy(mutated, valid->bytes, valid->length);
    write_u32(mutated, offset, replacement);
    status = verify_status(mutated, valid->length);
    if (status != expected) {
        fprintf(stderr, "u32 mutation at %zu: got %s, expected %s\n", offset,
                milena_bytecode_status_name(status),
                milena_bytecode_status_name(expected));
        free(mutated);
        return 1;
    }
    free(mutated);
    return 0;
}

static int test_valid_sum_count_and_roundtrip(void) {
    const uint8_t operations[] = {MILENA_BYTECODE_DATA_SUM,
                                  MILENA_BYTECODE_DATA_COUNT};
    for (size_t i = 0u; i < sizeof(operations); ++i) {
        MilenaBytecodeDataPlan plan = make_plan(operations[i]);
        MilenaBytecodeDataPlanView view;
        MilenaBytecodeDiagnostic diagnostic;
        EncodedData encoded;
        size_t queried = 0u;
        size_t written = 0u;
        double ignored = 123.0;

        CHECK(milena_bytecode_data_encode(&plan, NULL, 0u, &queried,
                                          &diagnostic) ==
              MILENA_BC_BUFFER_TOO_SMALL);
        CHECK(queried > STRING_TABLE_OFFSET);
        CHECK(encode_plan(&plan, &encoded) == 0);
        CHECK(encoded.length == queried);
        CHECK(memcmp(encoded.bytes, "MLBC", 4u) == 0);
        CHECK(encoded.bytes[4] == 1u && encoded.bytes[5] == 0u);
        CHECK(encoded.bytes[6] == 3u && encoded.bytes[7] == 0u);
        CHECK(encoded.bytes[8] == 0u && encoded.bytes[9] == 0u);
        CHECK(encoded.bytes[10] == 1u && encoded.bytes[11] == 0u);
        CHECK(encoded.bytes[12] == 0u && encoded.bytes[13] == 0u &&
              encoded.bytes[14] == 0u && encoded.bytes[15] == 0u);
        CHECK(milena_bytecode_verify_data(encoded.bytes, encoded.length, &view,
                                          &diagnostic) == MILENA_BC_OK);
        CHECK(view.operation == operations[i]);
        CHECK(view.span_present && view.source_line == 7u &&
              view.source_column == 9u);
        CHECK(view.source_path.data == encoded.bytes + STRING_TABLE_OFFSET + 4u);
        CHECK(view.input_column.length == sizeof(input_column) - 1u);
        CHECK(memcmp(view.input_column.data, "valor", 5u) == 0);
        CHECK(view.result_column.length ==
              (operations[i] == MILENA_BYTECODE_DATA_SUM ? 10u : 12u));
        CHECK(memcmp(view.result_column.data,
                     operations[i] == MILENA_BYTECODE_DATA_SUM
                         ? "valor_suma" : "valor_conteo",
                     view.result_column.length) == 0);
        CHECK(milena_bytecode_data_encode(&plan, encoded.bytes,
                    sizeof(encoded.bytes), &written, &diagnostic) == MILENA_BC_OK);
        CHECK(written == encoded.length);
        CHECK(milena_bytecode_verify(encoded.bytes, encoded.length, NULL,
                                     &diagnostic) == MILENA_BC_BAD_VERSION);
        CHECK(milena_bytecode_run(encoded.bytes, encoded.length, NULL, &ignored,
                                  &diagnostic) == MILENA_BC_BAD_VERSION);
    }
    {
        MilenaBytecodeDataPlan plan = make_plan(MILENA_BYTECODE_DATA_SUM);
        MilenaBytecodeDataPlanView view;
        MilenaBytecodeDiagnostic diagnostic;
        EncodedData encoded;
        plan.span_present = false;
        plan.source_line = 0u;
        plan.source_column = 0u;
        CHECK(encode_plan(&plan, &encoded) == 0);
        CHECK(milena_bytecode_verify_data(encoded.bytes, encoded.length, &view,
                                          &diagnostic) == MILENA_BC_OK);
        CHECK(!view.span_present && view.source_line == 0u &&
              view.source_column == 0u);
    }
    return 0;
}

static int test_boundary_valid_caps_and_strings(void) {
    uint8_t long_path_a[MILENA_BYTECODE_DATA_MAX_STRING_BYTES];
    uint8_t long_path_b[MILENA_BYTECODE_DATA_MAX_STRING_BYTES];
    uint8_t long_name[250u];
    MilenaBytecodeDataPlan plan = make_plan(MILENA_BYTECODE_DATA_SUM);
    MilenaBytecodeDataPlanView view;
    MilenaBytecodeDiagnostic diagnostic;
    EncodedData encoded;

    memset(long_path_a, 'a', sizeof(long_path_a));
    memset(long_path_b, 'b', sizeof(long_path_b));
    long_name[0] = 'x';
    memset(long_name + 1u, 'n', sizeof(long_name) - 1u);
    plan.source_path.data = long_path_a;
    plan.source_path.length = sizeof(long_path_a);
    plan.export_path.data = long_path_b;
    plan.export_path.length = sizeof(long_path_b);
    plan.input_column.data = long_name;
    plan.input_column.length = sizeof(long_name);
    plan.limits.max_input_file_bytes = MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES;
    plan.limits.max_input_data_rows = MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS;
    plan.limits.max_input_columns = MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS;
    plan.limits.max_csv_field_bytes = MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES;

    CHECK(encode_plan(&plan, &encoded) == 0);
    CHECK(encoded.length <= MILENA_BYTECODE_DATA_MAX_MODULE_BYTES);
    CHECK(milena_bytecode_verify_data(encoded.bytes, encoded.length, &view,
                                      &diagnostic) == MILENA_BC_OK);
    CHECK(view.source_path.length == MILENA_BYTECODE_DATA_MAX_STRING_BYTES);
    CHECK(view.export_path.length == MILENA_BYTECODE_DATA_MAX_STRING_BYTES);
    CHECK(view.input_column.length == 250u);
    CHECK(view.result_column.length == 255u);
    CHECK(view.limits.max_input_file_bytes ==
          MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES);
    CHECK(view.limits.max_input_data_rows ==
          MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS);
    CHECK(view.limits.max_input_columns ==
          MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS);
    CHECK(view.limits.max_csv_field_bytes ==
          MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES);
    return 0;
}

static int test_negative_mutations(void) {
    MilenaBytecodeDataPlan plan = make_plan(MILENA_BYTECODE_DATA_SUM);
    EncodedData valid;
    uint8_t *mutated;
    size_t source_data = STRING_TABLE_OFFSET + 4u;
    size_t export_length_offset = STRING_TABLE_OFFSET + 4u +
                                  plan.source_path.length;
    size_t export_data = export_length_offset + 4u;
    size_t input_length_offset = export_data + plan.export_path.length;
    size_t input_data = input_length_offset + 4u;
    size_t result_length_offset = input_data + plan.input_column.length;

    CHECK(encode_plan(&plan, &valid) == 0);

    /* Every fixed count and every serialized identifier reference is checked. */
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 12u, 2u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 16u, 2u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 20u, 2u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 40u, 2u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 44u, 3u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, COLUMN_RECORD_OFFSET, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 4u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 8u, 2u,
                                       MILENA_BC_BAD_FORMAT) == 0);

    /* Unknown versions, flags, section magic/revisions and reserved fields. */
    CHECK(expect_mutation_rejected(&valid, 0u, 'X', MILENA_BC_BAD_MAGIC) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, 4u, 2u,
                                       MILENA_BC_BAD_VERSION) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, 6u, 4u,
                                       MILENA_BC_BAD_VERSION) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, 8u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, 10u, 0u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, 10u, 3u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, 12u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, SECTION_START + 4u, 2u,
                                       MILENA_BC_BAD_VERSION) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, SECTION_START + 6u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 52u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_mutation_rejected(&valid, COLUMN_RECORD_OFFSET + 5u, 1u,
                                   MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, COLUMN_RECORD_OFFSET + 6u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 2u, 2u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 20u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);

    /* Enum/type IDs, nonzero span requirements and the derived suffix are strict. */
    CHECK(expect_mutation_rejected(&valid, COLUMN_RECORD_OFFSET + 4u, 2u,
                                   MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_mutation_rejected(&valid, OPERATION_RECORD_OFFSET, 99u,
                                   MILENA_BC_BAD_OPCODE) == 0);
    CHECK(expect_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 1u, 2u,
                                   MILENA_BC_BAD_TYPE) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 12u, 0u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 16u, 0u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u16_mutation_rejected(&valid, OPERATION_RECORD_OFFSET + 2u, 0u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_mutation_rejected(&valid, valid.length - 1u,
                                   (uint8_t)(valid.bytes[valid.length - 1u] ^ 1u),
                                   MILENA_BC_BAD_FORMAT) == 0);

    /* Exact section and table sizes, each record length, truncation and trailing data. */
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_LENGTH_OFFSET,
                                       (uint32_t)(valid.length - SECTION_START - 1u),
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_LENGTH_OFFSET,
                                       (uint32_t)(valid.length - SECTION_START + 1u),
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, STRING_TABLE_LENGTH_OFFSET,
                                       (uint32_t)(valid.length - STRING_TABLE_OFFSET - 1u),
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, STRING_TABLE_LENGTH_OFFSET,
                                       (uint32_t)(valid.length - STRING_TABLE_OFFSET + 1u),
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, STRING_TABLE_OFFSET, 0u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, export_length_offset,
                                       MILENA_BYTECODE_DATA_MAX_STRING_BYTES + 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, input_length_offset,
                                       (uint32_t)(plan.input_column.length + 1u),
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, result_length_offset, 0u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(verify_status(valid.bytes, valid.length - 1u) == MILENA_BC_BAD_FORMAT);
    CHECK(verify_status(valid.bytes, MILENA_BYTECODE_HEADER_SIZE - 1u) ==
          MILENA_BC_BAD_FORMAT);
    mutated = (uint8_t *)malloc(valid.length + 1u);
    CHECK(mutated != NULL);
    memcpy(mutated, valid.bytes, valid.length);
    mutated[valid.length] = 0u;
    CHECK(verify_status(mutated, valid.length + 1u) == MILENA_BC_BAD_FORMAT);
    free(mutated);

    /* Invalid UTF-8, embedded NUL and invalid identifier grammar fail closed. */
    CHECK(expect_mutation_rejected(&valid, source_data, 0xffu,
                                   MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_mutation_rejected(&valid, source_data, 0u,
                                   MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_mutation_rejected(&valid, input_data, (uint8_t)'%',
                                   MILENA_BC_BAD_FORMAT) == 0);
    /* Zero and raised values for every encoded resource cap are rejected. */
    for (size_t i = 0u; i < 4u; ++i) {
        const size_t offsets[] = {SECTION_START + 24u, SECTION_START + 28u,
                                  SECTION_START + 32u, SECTION_START + 36u};
        const uint32_t maxima[] = {
            MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES,
            MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS,
            MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS,
            MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES
        };
        CHECK(expect_u32_mutation_rejected(&valid, offsets[i], 0u,
                                           MILENA_BC_LIMIT_EXCEEDED) == 0);
        CHECK(expect_u32_mutation_rejected(&valid, offsets[i], maxima[i] + 1u,
                                           MILENA_BC_LIMIT_EXCEEDED) == 0);
    }
    CHECK(verify_status(valid.bytes,
                        MILENA_BYTECODE_DATA_MAX_MODULE_BYTES + 1u) ==
          MILENA_BC_LIMIT_EXCEEDED);

    /* Bad DPLN magic and explicit-data version checks are also covered. */
    CHECK(expect_mutation_rejected(&valid, SECTION_START, 'X',
                                   MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 8u, 1u,
                                       MILENA_BC_BAD_FORMAT) == 0);
    CHECK(expect_u32_mutation_rejected(&valid, SECTION_START + 48u,
                                       UINT32_MAX, MILENA_BC_BAD_FORMAT) == 0);
    return 0;
}

static int test_encoder_rejects_invalid_inputs(void) {
    MilenaBytecodeDataPlan plan = make_plan(MILENA_BYTECODE_DATA_SUM);
    MilenaBytecodeDiagnostic diagnostic;
    size_t size = 0u;
    EncodedData encoded;
    uint8_t small_output[1] = {0xa5u};

    plan.limits.max_input_data_rows = 0u;
    CHECK(!milena_bytecode_data_encoded_size(&plan, &size));
    CHECK(milena_bytecode_data_encode(&plan, encoded.bytes,
              sizeof(encoded.bytes), &size, &diagnostic) == MILENA_BC_BAD_FORMAT);
    plan = make_plan(MILENA_BYTECODE_DATA_SUM);
    CHECK(milena_bytecode_data_encode(&plan, small_output,
              sizeof(small_output), &size, &diagnostic) ==
          MILENA_BC_BUFFER_TOO_SMALL);
    CHECK(size > sizeof(small_output) && small_output[0] == 0xa5u);
    plan = make_plan(88u);
    CHECK(!milena_bytecode_data_encoded_size(&plan, &size));
    plan = make_plan(MILENA_BYTECODE_DATA_SUM);
    plan.source_line = 0u;
    CHECK(milena_bytecode_data_encode(&plan, encoded.bytes,
              sizeof(encoded.bytes), &size, &diagnostic) == MILENA_BC_BAD_FORMAT);
    return 0;
}

int main(void) {
    CHECK(test_valid_sum_count_and_roundtrip() == 0);
    CHECK(test_boundary_valid_caps_and_strings() == 0);
    CHECK(test_negative_mutations() == 0);
    CHECK(test_encoder_rejects_invalid_inputs() == 0);
    puts("bytecode data-plan wire tests passed");
    return 0;
}
