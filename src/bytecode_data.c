#include "bytecode.h"

#include <stdio.h>
#include <string.h>

#define DATA_HEADER_FLAGS UINT16_C(1)
#define DATA_SECTION_HEADER_SIZE ((size_t)56u)
#define DATA_COLUMN_RECORD_SIZE ((size_t)8u)
#define DATA_OPERATION_RECORD_SIZE ((size_t)24u)
#define DATA_FIXED_SECTION_SIZE (DATA_SECTION_HEADER_SIZE + DATA_COLUMN_RECORD_SIZE + DATA_OPERATION_RECORD_SIZE)
#define DATA_STRING_COUNT 4u
#define DATA_COLUMN_NAME_ID 2u
#define DATA_RESULT_NAME_ID 3u
#define DATA_SPAN_PRESENT UINT16_C(1)

static const uint8_t data_module_magic[4] = {'M', 'L', 'B', 'C'};
static const uint8_t data_section_magic[4] = {'D', 'P', 'L', 'N'};

static MilenaBytecodeStatus data_fail(MilenaBytecodeDiagnostic *diagnostic,
                                      MilenaBytecodeStatus status,
                                      size_t offset,
                                      const char *message) {
    if (diagnostic) {
        diagnostic->status = status;
        diagnostic->byte_offset = offset;
        (void)snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
                       message);
    }
    return status;
}

static void data_clear_diagnostic(MilenaBytecodeDiagnostic *diagnostic) {
    if (diagnostic) {
        diagnostic->status = MILENA_BC_OK;
        diagnostic->byte_offset = 0;
        diagnostic->message[0] = '\0';
    }
}

static uint16_t data_read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t data_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void data_write_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & UINT16_C(0xff));
    p[1] = (uint8_t)((value >> 8) & UINT16_C(0xff));
}

static void data_write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & UINT32_C(0xff));
    p[1] = (uint8_t)((value >> 8) & UINT32_C(0xff));
    p[2] = (uint8_t)((value >> 16) & UINT32_C(0xff));
    p[3] = (uint8_t)((value >> 24) & UINT32_C(0xff));
}

static void data_write_string(uint8_t *out, size_t *cursor,
                              MilenaBytecodeDataStringView string) {
    data_write_u32(out + *cursor, (uint32_t)string.length);
    *cursor += 4u;
    memcpy(out + *cursor, string.data, string.length);
    *cursor += string.length;
}

static bool data_add_size(size_t a, size_t b, size_t *sum) {
    if (!sum || b > SIZE_MAX - a) return false;
    *sum = a + b;
    return true;
}

static bool data_utf8_next(const uint8_t *s, size_t n, size_t *cursor,
                           uint32_t *codepoint) {
    size_t i;
    uint8_t b0;
    uint32_t cp;
    size_t width;

    if (!s || !cursor || !codepoint || *cursor >= n) return false;
    i = *cursor;
    b0 = s[i];
    if (b0 <= 0x7fu) {
        cp = b0;
        width = 1u;
    } else if (b0 >= 0xc2u && b0 <= 0xdfu) {
        cp = (uint32_t)(b0 & 0x1fu);
        width = 2u;
    } else if (b0 >= 0xe0u && b0 <= 0xefu) {
        cp = (uint32_t)(b0 & 0x0fu);
        width = 3u;
    } else if (b0 >= 0xf0u && b0 <= 0xf4u) {
        cp = (uint32_t)(b0 & 0x07u);
        width = 4u;
    } else {
        return false;
    }
    if (width > n - i) return false;
    for (size_t k = 1u; k < width; ++k) {
        uint8_t next = s[i + k];
        if ((next & 0xc0u) != 0x80u) return false;
        if (k == 1u) {
            if (b0 == 0xe0u && next < 0xa0u) return false;
            if (b0 == 0xedu && next >= 0xa0u) return false;
            if (b0 == 0xf0u && next < 0x90u) return false;
            if (b0 == 0xf4u && next >= 0x90u) return false;
        }
        cp = (cp << 6) | (uint32_t)(next & 0x3fu);
    }
    *cursor = i + width;
    *codepoint = cp;
    return true;
}

static bool data_valid_string(MilenaBytecodeDataStringView string) {
    size_t cursor = 0;
    if (!string.data || string.length == 0u ||
        string.length > MILENA_BYTECODE_DATA_MAX_STRING_BYTES) return false;
    while (cursor < string.length) {
        uint32_t cp = 0;
        if (!data_utf8_next(string.data, string.length, &cursor, &cp) || cp == 0u)
            return false;
    }
    return true;
}

static bool data_ascii_identifier_start(uint32_t cp) {
    return cp == (uint32_t)'_' ||
           (cp >= (uint32_t)'A' && cp <= (uint32_t)'Z') ||
           (cp >= (uint32_t)'a' && cp <= (uint32_t)'z');
}

static bool data_ascii_identifier_continue(uint32_t cp) {
    return data_ascii_identifier_start(cp) ||
           (cp >= (uint32_t)'0' && cp <= (uint32_t)'9');
}

/* Mirrors the language lexer's ASCII identifier rules and its UTF-8 acceptance. */
static bool data_valid_identifier(MilenaBytecodeDataStringView string) {
    size_t cursor = 0;
    bool first = true;
    if (!data_valid_string(string) || string.length > 255u) return false;
    while (cursor < string.length) {
        uint32_t cp = 0;
        if (!data_utf8_next(string.data, string.length, &cursor, &cp)) return false;
        if (cp >= 0x80u) {
            first = false;
            continue;
        }
        if (first ? !data_ascii_identifier_start(cp)
                  : !data_ascii_identifier_continue(cp)) return false;
        first = false;
    }
    return !first;
}

static const char *data_suffix(uint8_t operation) {
    if (operation == MILENA_BYTECODE_DATA_SUM) return "_suma";
    if (operation == MILENA_BYTECODE_DATA_COUNT) return "_conteo";
    return NULL;
}

static size_t data_suffix_length(uint8_t operation) {
    const char *suffix = data_suffix(operation);
    if (!suffix) return 0u;
    return operation == MILENA_BYTECODE_DATA_SUM ? 5u : 7u;
}

static bool data_valid_caps(const MilenaBytecodeDataLimits *limits) {
    return limits && limits->max_input_file_bytes != 0u &&
           limits->max_input_file_bytes <= MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES &&
           limits->max_input_data_rows != 0u &&
           limits->max_input_data_rows <= MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS &&
           limits->max_input_columns != 0u &&
           limits->max_input_columns <= MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS &&
           limits->max_csv_field_bytes != 0u &&
           limits->max_csv_field_bytes <= MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES;
}

static bool data_plan_sizes(const MilenaBytecodeDataPlan *plan,
                            size_t *size_out,
                            size_t *table_bytes_out,
                            size_t *result_name_bytes_out) {
    size_t table_bytes = 0u;
    size_t result_length;
    size_t section_bytes;
    size_t module_bytes;
    size_t aggregate_string_bytes;
    const size_t suffix_length = plan ? data_suffix_length(plan->operation) : 0u;

    if (!plan || !size_out || !data_suffix(plan->operation) ||
        !data_valid_string(plan->source_path) ||
        !data_valid_string(plan->export_path) ||
        !data_valid_identifier(plan->input_column) ||
        !data_valid_caps(&plan->limits) ||
        (plan->span_present &&
         (plan->source_line == 0u || plan->source_column == 0u)) ||
        (!plan->span_present &&
         (plan->source_line != 0u || plan->source_column != 0u)) ||
        !data_add_size(plan->input_column.length, suffix_length, &result_length) ||
        result_length > 255u || result_length > MILENA_BYTECODE_DATA_MAX_STRING_BYTES) {
        return false;
    }
    if (plan->source_path.length > MILENA_BYTECODE_DATA_MAX_STRING_BYTES ||
        plan->export_path.length > MILENA_BYTECODE_DATA_MAX_STRING_BYTES ||
        plan->source_path.length > SIZE_MAX - plan->export_path.length ||
        plan->source_path.length + plan->export_path.length > SIZE_MAX -
            plan->input_column.length ||
        plan->source_path.length + plan->export_path.length +
            plan->input_column.length > SIZE_MAX - result_length) return false;
    aggregate_string_bytes = plan->source_path.length + plan->export_path.length +
                             plan->input_column.length + result_length;
    if (aggregate_string_bytes > MILENA_BYTECODE_DATA_MAX_TOTAL_STRING_BYTES) return false;

    if (!data_add_size(table_bytes, 4u, &table_bytes) ||
        !data_add_size(table_bytes, plan->source_path.length, &table_bytes) ||
        !data_add_size(table_bytes, 4u, &table_bytes) ||
        !data_add_size(table_bytes, plan->export_path.length, &table_bytes) ||
        !data_add_size(table_bytes, 4u, &table_bytes) ||
        !data_add_size(table_bytes, plan->input_column.length, &table_bytes) ||
        !data_add_size(table_bytes, 4u, &table_bytes) ||
        !data_add_size(table_bytes, result_length, &table_bytes) ||
        !data_add_size(DATA_FIXED_SECTION_SIZE, table_bytes, &section_bytes) ||
        !data_add_size(MILENA_BYTECODE_HEADER_SIZE, section_bytes, &module_bytes) ||
        module_bytes > MILENA_BYTECODE_DATA_MAX_MODULE_BYTES ||
        section_bytes > UINT32_MAX || table_bytes > UINT32_MAX) return false;

    *size_out = module_bytes;
    if (table_bytes_out) *table_bytes_out = table_bytes;
    if (result_name_bytes_out) *result_name_bytes_out = result_length;
    return true;
}

bool milena_bytecode_data_encoded_size(const MilenaBytecodeDataPlan *plan,
                                       size_t *size_out) {
    return data_plan_sizes(plan, size_out, NULL, NULL);
}

MilenaBytecodeStatus milena_bytecode_data_encode(
    const MilenaBytecodeDataPlan *plan,
    uint8_t *out,
    size_t capacity,
    size_t *written,
    MilenaBytecodeDiagnostic *diagnostic) {
    size_t required = 0u;
    size_t string_table_bytes = 0u;
    size_t result_name_length = 0u;
    size_t cursor;
    size_t section_bytes;
    const char *suffix;

    data_clear_diagnostic(diagnostic);
    if (written) *written = 0u;
    if (!written) return data_fail(diagnostic, MILENA_BC_ARGUMENT, 0u,
                                   "missing encoded-size output");
    if (!data_plan_sizes(plan, &required, &string_table_bytes,
                         &result_name_length))
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, 0u,
                         "invalid data-plan fields or resource caps");
    *written = required;
    if (!out && capacity == 0u)
        return data_fail(diagnostic, MILENA_BC_BUFFER_TOO_SMALL, 0u,
                         "output buffer size query");
    if (!out)
        return data_fail(diagnostic, MILENA_BC_ARGUMENT, 0u,
                         "null output with nonzero capacity");
    if (capacity < required)
        return data_fail(diagnostic, MILENA_BC_BUFFER_TOO_SMALL, 0u,
                         "output buffer is too small");

    section_bytes = required - MILENA_BYTECODE_HEADER_SIZE;
    memset(out, 0, required);
    memcpy(out, data_module_magic, sizeof(data_module_magic));
    data_write_u16(out + 4u, MILENA_BYTECODE_VERSION_MAJOR);
    data_write_u16(out + 6u, MILENA_BYTECODE_VERSION_DATA_MINOR);
    data_write_u16(out + 8u, 0u);
    data_write_u16(out + 10u, DATA_HEADER_FLAGS);
    data_write_u32(out + 12u, 0u);

    cursor = MILENA_BYTECODE_HEADER_SIZE;
    memcpy(out + cursor, data_section_magic, sizeof(data_section_magic));
    data_write_u16(out + cursor + 4u, 1u);
    data_write_u16(out + cursor + 6u, 0u);
    data_write_u32(out + cursor + 8u, (uint32_t)section_bytes);
    data_write_u32(out + cursor + 12u, 1u);
    data_write_u32(out + cursor + 16u, 1u);
    data_write_u32(out + cursor + 20u, 1u);
    data_write_u32(out + cursor + 24u, plan->limits.max_input_file_bytes);
    data_write_u32(out + cursor + 28u, plan->limits.max_input_data_rows);
    data_write_u32(out + cursor + 32u, plan->limits.max_input_columns);
    data_write_u32(out + cursor + 36u, plan->limits.max_csv_field_bytes);
    data_write_u32(out + cursor + 40u, 1u);
    data_write_u32(out + cursor + 44u, DATA_STRING_COUNT);
    data_write_u32(out + cursor + 48u, (uint32_t)string_table_bytes);

    cursor += DATA_SECTION_HEADER_SIZE;
    data_write_u32(out + cursor, DATA_COLUMN_NAME_ID);
    out[cursor + 4u] = MILENA_BYTECODE_DATA_TYPE_NUMBER;
    cursor += DATA_COLUMN_RECORD_SIZE;

    out[cursor] = plan->operation;
    out[cursor + 1u] = MILENA_BYTECODE_DATA_TYPE_NUMBER;
    data_write_u16(out + cursor + 2u,
                   plan->span_present ? DATA_SPAN_PRESENT : 0u);
    data_write_u32(out + cursor + 4u, DATA_COLUMN_NAME_ID);
    data_write_u32(out + cursor + 8u, DATA_RESULT_NAME_ID);
    data_write_u32(out + cursor + 12u,
                   plan->span_present ? plan->source_line : 0u);
    data_write_u32(out + cursor + 16u,
                   plan->span_present ? plan->source_column : 0u);
    cursor += DATA_OPERATION_RECORD_SIZE;

    data_write_string(out, &cursor, plan->source_path);
    data_write_string(out, &cursor, plan->export_path);
    data_write_string(out, &cursor, plan->input_column);
    data_write_u32(out + cursor, (uint32_t)result_name_length);
    cursor += 4u;
    memcpy(out + cursor, plan->input_column.data, plan->input_column.length);
    cursor += plan->input_column.length;
    suffix = data_suffix(plan->operation);
    memcpy(out + cursor, suffix, data_suffix_length(plan->operation));
    cursor += data_suffix_length(plan->operation);

    if (cursor != required)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor,
                         "internal data-plan length mismatch");
    return MILENA_BC_OK;
}

static bool data_read_string(const uint8_t *bytes, size_t *cursor,
                             size_t table_end,
                             MilenaBytecodeDataStringView *string,
                             size_t *total_string_bytes) {
    uint32_t encoded_length;
    size_t length;
    if (!bytes || !cursor || !string || !total_string_bytes ||
        *cursor > table_end || table_end - *cursor < 4u) return false;
    encoded_length = data_read_u32(bytes + *cursor);
    *cursor += 4u;
    if (encoded_length == 0u ||
        encoded_length > MILENA_BYTECODE_DATA_MAX_STRING_BYTES) return false;
    length = (size_t)encoded_length;
    if (length > MILENA_BYTECODE_DATA_MAX_STRING_BYTES ||
        *cursor > table_end || length > table_end - *cursor ||
        length > MILENA_BYTECODE_DATA_MAX_TOTAL_STRING_BYTES - *total_string_bytes)
        return false;
    string->data = bytes + *cursor;
    string->length = length;
    if (!data_valid_string(*string)) return false;
    *cursor += length;
    *total_string_bytes += length;
    return true;
}

MilenaBytecodeStatus milena_bytecode_verify_data(
    const uint8_t *bytes,
    size_t length,
    MilenaBytecodeDataPlanView *view_out,
    MilenaBytecodeDiagnostic *diagnostic) {
    MilenaBytecodeDataPlanView parsed;
    uint32_t section_bytes;
    uint32_t string_table_bytes;
    size_t module_section_length;
    size_t section_start = MILENA_BYTECODE_HEADER_SIZE;
    size_t string_start;
    size_t string_end;
    size_t cursor;
    size_t total_string_bytes = 0u;
    uint16_t operation_flags;
    uint8_t operation;

    data_clear_diagnostic(diagnostic);
    if (!bytes)
        return data_fail(diagnostic, MILENA_BC_ARGUMENT, 0u,
                         "bytecode is null");
    if (length < MILENA_BYTECODE_HEADER_SIZE)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, length,
                         "truncated data module header");
    if (length > MILENA_BYTECODE_DATA_MAX_MODULE_BYTES)
        return data_fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 0u,
                         "data module exceeds its hard byte limit");
    if (memcmp(bytes, data_module_magic, sizeof(data_module_magic)) != 0)
        return data_fail(diagnostic, MILENA_BC_BAD_MAGIC, 0u,
                         "invalid bytecode magic");
    if (data_read_u16(bytes + 4u) != MILENA_BYTECODE_VERSION_MAJOR ||
        data_read_u16(bytes + 6u) != MILENA_BYTECODE_VERSION_DATA_MINOR)
        return data_fail(diagnostic, MILENA_BC_BAD_VERSION, 4u,
                         "unsupported data bytecode version");
    if (data_read_u16(bytes + 8u) != 0u ||
        data_read_u16(bytes + 10u) != DATA_HEADER_FLAGS ||
        data_read_u32(bytes + 12u) != 0u)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, 8u,
                         "invalid data-only header fields");
    if (length < section_start + DATA_FIXED_SECTION_SIZE)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, length,
                         "truncated data plan section");
    if (memcmp(bytes + section_start, data_section_magic,
               sizeof(data_section_magic)) != 0)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start,
                         "invalid DPLN section magic");
    if (data_read_u16(bytes + section_start + 4u) != 1u)
        return data_fail(diagnostic, MILENA_BC_BAD_VERSION, section_start + 4u,
                         "unsupported DPLN schema revision");
    if (data_read_u16(bytes + section_start + 6u) != 0u)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start + 6u,
                         "DPLN flags must be zero");

    section_bytes = data_read_u32(bytes + section_start + 8u);
    string_table_bytes = data_read_u32(bytes + section_start + 48u);
    if (section_bytes > SIZE_MAX - MILENA_BYTECODE_HEADER_SIZE)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start + 8u,
                         "DPLN section length overflows host size");
    if ((uint64_t)section_bytes !=
        (uint64_t)(length - MILENA_BYTECODE_HEADER_SIZE))
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start + 8u,
                         "DPLN section length does not match module length");
    module_section_length = MILENA_BYTECODE_HEADER_SIZE + (size_t)section_bytes;
    if (module_section_length != length ||
        section_bytes < DATA_FIXED_SECTION_SIZE ||
        (uint64_t)string_table_bytes !=
            (uint64_t)section_bytes - (uint64_t)DATA_FIXED_SECTION_SIZE)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start + 8u,
                         "DPLN section or string-table length is not exact");

    if (data_read_u32(bytes + section_start + 12u) != 1u ||
        data_read_u32(bytes + section_start + 16u) != 1u ||
        data_read_u32(bytes + section_start + 20u) != 1u ||
        data_read_u32(bytes + section_start + 40u) != 1u ||
        data_read_u32(bytes + section_start + 44u) != DATA_STRING_COUNT)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start + 12u,
                         "DPLN dataset, record, output, or string count is invalid");
    if (data_read_u32(bytes + section_start + 52u) != 0u)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, section_start + 52u,
                         "DPLN reserved field must be zero");

    parsed.limits.max_input_file_bytes = data_read_u32(bytes + section_start + 24u);
    parsed.limits.max_input_data_rows = data_read_u32(bytes + section_start + 28u);
    parsed.limits.max_input_columns = data_read_u32(bytes + section_start + 32u);
    parsed.limits.max_csv_field_bytes = data_read_u32(bytes + section_start + 36u);
    if (!data_valid_caps(&parsed.limits))
        return data_fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED,
                         section_start + 24u,
                         "DPLN input resource cap is zero or exceeds its hard maximum");

    cursor = section_start + DATA_SECTION_HEADER_SIZE;
    if (data_read_u32(bytes + cursor) != DATA_COLUMN_NAME_ID ||
        bytes[cursor + 4u] != MILENA_BYTECODE_DATA_TYPE_NUMBER ||
        bytes[cursor + 5u] != 0u || data_read_u16(bytes + cursor + 6u) != 0u)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor,
                         "invalid declared numeric column record");
    cursor += DATA_COLUMN_RECORD_SIZE;
    operation = bytes[cursor];
    if (!data_suffix(operation))
        return data_fail(diagnostic, MILENA_BC_BAD_OPCODE, cursor,
                         "unknown data summary operation");
    if (bytes[cursor + 1u] != MILENA_BYTECODE_DATA_TYPE_NUMBER)
        return data_fail(diagnostic, MILENA_BC_BAD_TYPE, cursor + 1u,
                         "summary input type must be numeric");
    operation_flags = data_read_u16(bytes + cursor + 2u);
    if ((operation_flags & (uint16_t)~DATA_SPAN_PRESENT) != 0u)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor + 2u,
                         "unknown summary operation flags");
    if (data_read_u32(bytes + cursor + 4u) != DATA_COLUMN_NAME_ID ||
        data_read_u32(bytes + cursor + 8u) != DATA_RESULT_NAME_ID)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor + 4u,
                         "summary string references are invalid");
    parsed.source_line = data_read_u32(bytes + cursor + 12u);
    parsed.source_column = data_read_u32(bytes + cursor + 16u);
    if (data_read_u32(bytes + cursor + 20u) != 0u)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor + 20u,
                         "summary reserved field must be zero");
    parsed.span_present = (operation_flags & DATA_SPAN_PRESENT) != 0u;
    if ((parsed.span_present &&
         (parsed.source_line == 0u || parsed.source_column == 0u)) ||
        (!parsed.span_present &&
         (parsed.source_line != 0u || parsed.source_column != 0u)))
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor + 12u,
                         "source span coordinates do not match the span flag");
    parsed.operation = operation;
    cursor += DATA_OPERATION_RECORD_SIZE;

    string_start = section_start + DATA_FIXED_SECTION_SIZE;
    string_end = length;
    cursor = string_start;
    if (!data_read_string(bytes, &cursor, string_end, &parsed.source_path,
                          &total_string_bytes) ||
        !data_read_string(bytes, &cursor, string_end, &parsed.export_path,
                          &total_string_bytes) ||
        !data_read_string(bytes, &cursor, string_end, &parsed.input_column,
                          &total_string_bytes) ||
        !data_read_string(bytes, &cursor, string_end, &parsed.result_column,
                          &total_string_bytes))
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor,
                         "invalid data-plan string length, UTF-8, or NUL byte");
    if (cursor != string_end)
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, cursor,
                         "trailing bytes in DPLN string table");
    if (total_string_bytes > MILENA_BYTECODE_DATA_MAX_TOTAL_STRING_BYTES)
        return data_fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, string_start,
                         "DPLN strings exceed their aggregate byte limit");
    if (!data_valid_identifier(parsed.input_column) ||
        !data_valid_identifier(parsed.result_column))
        return data_fail(diagnostic, MILENA_BC_BAD_FORMAT, string_start,
                         "column name is not a canonical identifier");

    {
        const char *suffix = data_suffix(operation);
        size_t suffix_length = data_suffix_length(operation);
        size_t expected_result_length;
        if (!data_add_size(parsed.input_column.length, suffix_length,
                           &expected_result_length) ||
            expected_result_length > 255u ||
            parsed.result_column.length != expected_result_length ||
            memcmp(parsed.result_column.data, parsed.input_column.data,
                   parsed.input_column.length) != 0 ||
            memcmp(parsed.result_column.data + parsed.input_column.length,
                   suffix, suffix_length) != 0)
            return data_fail(diagnostic, MILENA_BC_BAD_FORMAT,
                             string_start,
                             "result column name does not use the canonical suffix");
    }
    if (view_out) *view_out = parsed;
    return MILENA_BC_OK;
}
