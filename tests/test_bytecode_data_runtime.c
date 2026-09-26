#define _POSIX_C_SOURCE 200809L
#include "analysis.h"
#include "bytecode.h"
#include "dataset.h"
#include "schema.h"
#include "table.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define INPUT_PATH "tests/bytecode_data_runtime_input.csv"
#define CONTEXT_PATH "tests/bytecode_data_runtime_context.milena"
#define LIMIT_PATH "tests/bytecode_data_runtime_limits.csv"
#define BLOCKED_PATH "tests/bytecode_data_runtime_blocked"
#define CHECK(condition, message)                                                \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static void remove_if_exists(const char *path) {
    (void)remove(path);
}

static int write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(bytes, 1u, length, file) == length;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int file_equals(const char *left, const char *right) {
    FILE *a = fopen(left, "rb"), *b = fopen(right, "rb");
    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return 0;
    }
    int equal = 1;
    for (;;) {
        int ca = fgetc(a), cb = fgetc(b);
        if (ca != cb) { equal = 0; break; }
        if (ca == EOF) break;
    }
    if (fclose(a) != 0 || fclose(b) != 0) equal = 0;
    return equal;
}

static int file_absent(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 1;
    fclose(file);
    return 0;
}

static int no_temp_left(const char *prefix) {
    DIR *directory = opendir("tests");
    if (!directory) return 0;
    int clean = 1;
    struct dirent *entry;
    size_t prefix_length = strlen(prefix);
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_length) == 0) {
            clean = 0;
            break;
        }
    }
    if (closedir(directory) != 0) clean = 0;
    return clean;
}

static int file_contains(const char *path, const char *needle) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char buffer[4096];
    size_t length = fread(buffer, 1u, sizeof(buffer) - 1u, file);
    buffer[length] = '\0';
    int ok = strstr(buffer, needle) != NULL;
    fclose(file);
    return ok;
}

static int compile_plan(const char *input, const char *output,
                        const char *column, uint8_t operation,
                        uint8_t **bytes_out, size_t *length_out) {
    MilenaBytecodeDataPlan plan = {0};
    MilenaBytecodeDiagnostic diagnostic;
    plan.source_path = (MilenaBytecodeDataStringView){
        (const uint8_t *)input, strlen(input)};
    plan.export_path = (MilenaBytecodeDataStringView){
        (const uint8_t *)output, strlen(output)};
    plan.input_column = (MilenaBytecodeDataStringView){
        (const uint8_t *)column, strlen(column)};
    plan.operation = operation;
    plan.span_present = true;
    plan.source_line = 4u;
    plan.source_column = 3u;
    plan.limits.max_input_file_bytes = MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES;
    plan.limits.max_input_data_rows = MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS;
    plan.limits.max_input_columns = MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS;
    plan.limits.max_csv_field_bytes = MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES;
    size_t length = 0u;
    if (!milena_bytecode_data_encoded_size(&plan, &length)) return 0;
    uint8_t *bytes = (uint8_t *)malloc(length);
    if (!bytes) return 0;
    size_t written = 0u;
    if (milena_bytecode_data_encode(&plan, bytes, length, &written,
                                    &diagnostic) != MILENA_BC_OK ||
        written != length) {
        free(bytes);
        return 0;
    }
    *bytes_out = bytes;
    *length_out = length;
    return 1;
}

static int write_canonical_reference(const char *input, const char *output,
                                     const char *column, const char *result,
                                     MilenaAggregateOp operation) {
    Dataset dataset;
    dataset_init(&dataset);
    MilenaSchema schema;
    schema_init(&schema);
    MilenaTable input_table = {0}, summary = {0};
    milena_table_init(&input_table);
    milena_table_init(&summary);
    MilenaError error;
    MilenaStatus status = dataset_load_csv(&dataset, input, ',', &error);
    if (status == MILENA_OK)
        status = schema_add(&schema, column, MILENA_VAR_NUMERIC,
                            MILENA_ROLE_FEATURE, &error);
    if (status == MILENA_OK)
        status = milena_table_from_dataset(&input_table, &dataset, &schema, &error);
    if (status == MILENA_OK) {
        MilenaAggregateSpec aggregate = {column, operation, result};
        status = milena_table_summarize(&summary, &input_table,
                                        &aggregate, 1u, &error);
    }
    if (status == MILENA_OK)
        status = analysis_table_report(&summary, &schema, output, &error);
    milena_table_destroy(&summary);
    milena_table_destroy(&input_table);
    schema_destroy(&schema);
    dataset_destroy(&dataset);
    if (status != MILENA_OK)
        fprintf(stderr, "canonical comparison failed: %s\n", error.message);
    return status == MILENA_OK;
}

static int test_sum_count_and_canonical_json(void) {
    static const char csv[] = "valor\n1.5\n2.5\n\n";
    CHECK(write_bytes(INPUT_PATH, csv, sizeof(csv) - 1u), "write fixed CSV");
    struct {
        uint8_t operation;
        const char *result;
        MilenaAggregateOp canonical_operation;
        const char *output;
        const char *published;
        const char *reference;
        const char *value;
    } cases[] = {
        {MILENA_BYTECODE_DATA_SUM, "valor_suma", MILENA_AGG_SUM,
         "bytecode_data_runtime_sum.json",
         "tests/bytecode_data_runtime_sum.json",
         "tests/bytecode_data_runtime_sum_canonical.json", "\"valor_suma\":4"},
        {MILENA_BYTECODE_DATA_COUNT, "valor_conteo", MILENA_AGG_COUNT,
         "bytecode_data_runtime_count.json",
         "tests/bytecode_data_runtime_count.json",
         "tests/bytecode_data_runtime_count_canonical.json", "\"valor_conteo\":2"}
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t *bytes = NULL;
        size_t length = 0u;
        CHECK(compile_plan("bytecode_data_runtime_input.csv", cases[i].output,
                           "valor", cases[i].operation, &bytes, &length),
              "encode fixed data plan");
        MilenaError error;
        CHECK(milena_bytecode_run_data(bytes, length, CONTEXT_PATH, NULL,
                                       &error) == MILENA_OK,
              error.message);
        CHECK(write_canonical_reference(INPUT_PATH, cases[i].reference, "valor",
                                        cases[i].result,
                                        cases[i].canonical_operation),
              "write canonical table-kernel reference report");
        CHECK(file_equals(cases[i].published, cases[i].reference),
              "data bytecode report must exactly match canonical typed report");
        CHECK(file_contains(cases[i].published, cases[i].value),
              "typed summary report must contain the expected canonical value");
        free(bytes);
    }
    return 0;
}

static int test_bad_module_has_no_file_effect(void) {
    uint8_t *bytes = NULL;
    size_t length = 0u;
    CHECK(compile_plan("missing-runtime-input.csv", "bad-module-output.json",
                       "valor", MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode bad-module fixture");
    bytes[0] ^= 0x01u;
    remove_if_exists("bad-module-output.json");
    MilenaError error;
    CHECK(milena_bytecode_run_data(bytes, length, NULL, NULL, &error) ==
              MILENA_ERR_DATA,
          "invalid module must fail verification");
    CHECK(error.code == MILENA_ERR_DATA,
          "invalid module must return structured data error");
    CHECK(file_absent("bad-module-output.json"),
          "verification failure must not create an output file");
    CHECK(file_absent("missing-runtime-input.csv"),
          "verification failure must not access the input path");
    free(bytes);
    return 0;
}

static int test_header_and_numeric_binding_failures(void) {
    uint8_t *bytes = NULL;
    size_t length = 0u;
    MilenaError error;
    CHECK(compile_plan("bytecode_data_runtime_input.csv", "missing-header.json",
                       "valor", MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode missing-header plan");
    CHECK(write_bytes(INPUT_PATH, "different\n1\n", sizeof("different\n1\n") - 1u),
                   "write mismatched header CSV");
    remove_if_exists("tests/missing-header.json");
    CHECK(milena_bytecode_run_data(bytes, length, CONTEXT_PATH, NULL, &error) ==
              MILENA_ERR_DATA,
          "missing declared CSV header must be rejected");
    CHECK(file_absent("tests/missing-header.json"),
          "missing header must not publish output");
    free(bytes);

    CHECK(compile_plan("bytecode_data_runtime_input.csv", "non-numeric.json",
                       "valor", MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode numeric-binding plan");
    CHECK(write_bytes(INPUT_PATH, "valor\nnot-a-number\n",
                      sizeof("valor\nnot-a-number\n") - 1u),
          "write nonnumeric CSV");
    remove_if_exists("tests/non-numeric.json");
    MilenaStatus numeric_status = milena_bytecode_run_data(
        bytes, length, CONTEXT_PATH, NULL, &error);
    free(bytes);
    bytes = NULL;
    CHECK(numeric_status == MILENA_ERR_TYPE,
          "declared numeric column must reject nonnumeric values");
    CHECK(file_absent("tests/non-numeric.json"),
          "numeric conversion failure must not publish output");
    return 0;
}

static int run_limit_case(const char *output,
                          const MilenaBytecodeDataRunLimits *limits) {
    uint8_t *bytes = NULL;
    size_t length = 0u;
    if (!compile_plan("bytecode_data_runtime_limits.csv", output, "a",
                      MILENA_BYTECODE_DATA_SUM, &bytes, &length)) return 0;
    MilenaError error;
    MilenaStatus status = milena_bytecode_run_data(bytes, length, CONTEXT_PATH,
                                                   limits, &error);
    free(bytes);
    char published[512];
    int written = snprintf(published, sizeof(published), "tests/%s", output);
    return written >= 0 && (size_t)written < sizeof(published) &&
           status == MILENA_ERR_LIMIT && error.code == MILENA_ERR_LIMIT &&
           file_absent(published);
}

static int test_lowered_byte_row_column_and_field_limits(void) {
    static const char csv[] = "a,b\n12345,2\n6,3\n";
    CHECK(write_bytes(LIMIT_PATH, csv, sizeof(csv) - 1u), "write limit CSV");
    MilenaBytecodeDataRunLimits limits = {0};
    limits.max_input_file_bytes = 4u;
    CHECK(run_limit_case("limit-bytes.json", &limits),
          "lowered byte budget must stop the bounded reader");
    memset(&limits, 0, sizeof(limits));
    limits.max_input_data_rows = 1u;
    CHECK(run_limit_case("limit-rows.json", &limits),
          "lowered row budget must stop the bounded reader");
    memset(&limits, 0, sizeof(limits));
    limits.max_input_columns = 1u;
    CHECK(run_limit_case("limit-columns.json", &limits),
          "lowered column budget must stop the bounded reader");
    memset(&limits, 0, sizeof(limits));
    limits.max_csv_field_bytes = 4u;
    CHECK(run_limit_case("limit-field.json", &limits),
          "lowered field budget must stop the bounded reader");
    return 0;
}

static int test_alias_rejection_and_existing_output_preservation(void) {
    static const char csv[] = "valor\n3\n";
    CHECK(write_bytes(INPUT_PATH, csv, sizeof(csv) - 1u), "write alias CSV");
    uint8_t *bytes = NULL;
    size_t length = 0u;
    MilenaError error;
    CHECK(compile_plan("bytecode_data_runtime_input.csv",
                       "./bytecode_data_runtime_input.csv", "valor",
                       MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode lexical alias plan");
    CHECK(milena_bytecode_run_data(bytes, length, CONTEXT_PATH, NULL, &error) ==
              MILENA_ERR_ARGUMENT,
          "lexical input/output alias must be refused");
    free(bytes);

    remove_if_exists("tests/bytecode_data_runtime_hardlink.csv");
    CHECK(link(INPUT_PATH, "tests/bytecode_data_runtime_hardlink.csv") == 0,
          "create hard-link alias fixture");
    CHECK(compile_plan("bytecode_data_runtime_input.csv",
                       "bytecode_data_runtime_hardlink.csv", "valor",
                       MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode inode alias plan");
    CHECK(milena_bytecode_run_data(bytes, length, CONTEXT_PATH, NULL, &error) ==
              MILENA_ERR_ARGUMENT,
          "same-device/inode output alias must be refused");
    free(bytes);
    remove_if_exists("tests/bytecode_data_runtime_hardlink.csv");

    CHECK(write_bytes("tests/preserved.json", "original\n", sizeof("original\n") - 1u),
          "write existing destination");
    CHECK(compile_plan("bytecode_data_runtime_input.csv", "preserved.json",
                       "valor", MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode failure-preservation plan");
    CHECK(write_bytes(INPUT_PATH, "other\n2\n", sizeof("other\n2\n") - 1u),
          "write failing input");
    CHECK(milena_bytecode_run_data(bytes, length, CONTEXT_PATH, NULL, &error) ==
              MILENA_ERR_DATA,
          "mismatched header must fail before export");
    CHECK(file_contains("tests/preserved.json", "original"),
          "existing output must remain untouched on execution failure");
    free(bytes);
    return 0;
}

static int test_report_write_failure_preserves_destination(void) {
#if defined(RLIMIT_FSIZE)
    static const char csv[] = "valor\n5\n";
    CHECK(write_bytes(INPUT_PATH, csv, sizeof(csv) - 1u), "write report-error CSV");
    CHECK(write_bytes("tests/report-preserved.json", "keep-me\n",
                      sizeof("keep-me\n") - 1u), "write report destination");
    uint8_t *bytes = NULL;
    size_t length = 0u;
    CHECK(compile_plan("bytecode_data_runtime_input.csv", "report-preserved.json",
                       "valor", MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode report-error plan");
    struct rlimit old_limit, tiny_limit = {0u, 0u};
    struct sigaction old_action, ignored_action;
    memset(&ignored_action, 0, sizeof(ignored_action));
    ignored_action.sa_handler = SIG_IGN;
    sigemptyset(&ignored_action.sa_mask);
    CHECK(getrlimit(RLIMIT_FSIZE, &old_limit) == 0,
          "read file-size limit before injected report failure");
    tiny_limit.rlim_max = old_limit.rlim_max;
    CHECK(sigaction(SIGXFSZ, &ignored_action, &old_action) == 0,
          "ignore file-size signal during injected report failure");
    CHECK(setrlimit(RLIMIT_FSIZE, &tiny_limit) == 0,
          "inject report write limit");
    MilenaError error;
    MilenaStatus status = milena_bytecode_run_data(bytes, length, CONTEXT_PATH,
                                                   NULL, &error);
    int restore_limit = setrlimit(RLIMIT_FSIZE, &old_limit);
    int restore_signal = sigaction(SIGXFSZ, &old_action, NULL);
    free(bytes);
    CHECK(restore_limit == 0 && restore_signal == 0,
          "restore injected process limits");
    CHECK(status == MILENA_ERR_IO && error.code == MILENA_ERR_IO,
          "typed reporter write failure must return structured I/O error");
    CHECK(file_contains("tests/report-preserved.json", "keep-me"),
          "report write failure must preserve existing destination bytes");
    CHECK(no_temp_left("report-preserved.json.milena-tmp-"),
          "report write failure must remove its temporary file");
#endif
    return 0;
}

static int test_rename_failure_cleanup(void) {
    static const char csv[] = "valor\n1\n";
    CHECK(write_bytes(INPUT_PATH, csv, sizeof(csv) - 1u), "write rename-failure CSV");
    (void)rmdir(BLOCKED_PATH);
    CHECK(mkdir(BLOCKED_PATH, 0700) == 0, "create directory destination");
    uint8_t *bytes = NULL;
    size_t length = 0u;
    CHECK(compile_plan("bytecode_data_runtime_input.csv",
                       "bytecode_data_runtime_blocked", "valor",
                       MILENA_BYTECODE_DATA_SUM, &bytes, &length),
          "encode rename-failure plan");
    MilenaError error;
    CHECK(milena_bytecode_run_data(bytes, length, CONTEXT_PATH, NULL, &error) ==
              MILENA_ERR_IO,
          "rename onto directory must fail atomically");
    struct stat info;
    CHECK(stat(BLOCKED_PATH, &info) == 0 && S_ISDIR(info.st_mode),
          "failed rename must preserve the existing destination");
    CHECK(no_temp_left("bytecode_data_runtime_blocked.milena-tmp-"),
          "failed publication must remove its temporary report");
    free(bytes);
    CHECK(rmdir(BLOCKED_PATH) == 0, "remove test directory destination");
    return 0;
}

int main(void) {
    remove_if_exists(INPUT_PATH);
    remove_if_exists(LIMIT_PATH);
    remove_if_exists("tests/bytecode_data_runtime_sum.json");
    remove_if_exists("tests/bytecode_data_runtime_sum_canonical.json");
    remove_if_exists("tests/bytecode_data_runtime_count.json");
    remove_if_exists("tests/bytecode_data_runtime_count_canonical.json");
    if (test_sum_count_and_canonical_json() != 0 ||
        test_bad_module_has_no_file_effect() != 0 ||
        test_header_and_numeric_binding_failures() != 0 ||
        test_lowered_byte_row_column_and_field_limits() != 0 ||
        test_alias_rejection_and_existing_output_preservation() != 0 ||
        test_report_write_failure_preserves_destination() != 0 ||
        test_rename_failure_cleanup() != 0) return 1;
    remove_if_exists(INPUT_PATH);
    remove_if_exists(LIMIT_PATH);
    remove_if_exists("tests/bytecode_data_runtime_sum.json");
    remove_if_exists("tests/bytecode_data_runtime_sum_canonical.json");
    remove_if_exists("tests/bytecode_data_runtime_count.json");
    remove_if_exists("tests/bytecode_data_runtime_count_canonical.json");
    remove_if_exists("tests/missing-header.json");
    remove_if_exists("tests/non-numeric.json");
    remove_if_exists("tests/limit-bytes.json");
    remove_if_exists("tests/limit-rows.json");
    remove_if_exists("tests/limit-columns.json");
    remove_if_exists("tests/limit-field.json");
    remove_if_exists("tests/preserved.json");
    remove_if_exists("tests/report-preserved.json");
    puts("bytecode data runtime tests passed");
    return 0;
}
