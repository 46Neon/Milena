#include "bytecode_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message)                                                \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const char *sum_source =
    ".analisis resumen {\n"
    "  variable valor numerica\n"
    "  dataset cargar datos(\"missing-input.csv\")\n"
    "  .resumir dataset { #suma(\"valor\") }\n"
    "  .exportar { (\"summary.json\") }\n"
    "}\n";

static const char *count_source =
    ".analisis resumen {\n"
    "  variable valor numerica\n"
    "  dataset cargar datos(\"count-input.csv\")\n"
    "  .resumir dataset { #conteo(\"valor\") }\n"
    "  .exportar { (\"count.json\") }\n"
    "}\n";

static bool view_string_equals(MilenaBytecodeDataStringView view,
                               const char *expected) {
    size_t expected_length = strlen(expected);
    return view.data && view.length == expected_length &&
           memcmp(view.data, expected, expected_length) == 0;
}

static int verify_expected(const uint8_t *bytes, size_t length,
                           uint8_t expected_operation,
                           const char *expected_source,
                           const char *expected_export,
                           const char *expected_column,
                           const char *expected_result) {
    MilenaBytecodeDataPlanView view;
    MilenaBytecodeDiagnostic diagnostic;
    MilenaBytecodeStatus status = milena_bytecode_verify_data(
        bytes, length, &view, &diagnostic);
    if (status != MILENA_BC_OK) {
        fprintf(stderr, "data verifier rejected compiled bytes: %s (%s)\n",
                milena_bytecode_status_name(status), diagnostic.message);
        return 1;
    }
    if (view.operation != expected_operation ||
        !view_string_equals(view.source_path, expected_source) ||
        !view_string_equals(view.export_path, expected_export) ||
        !view_string_equals(view.input_column, expected_column) ||
        !view_string_equals(view.result_column, expected_result) ||
        view.limits.max_input_file_bytes !=
            MILENA_BYTECODE_DATA_MAX_INPUT_FILE_BYTES ||
        view.limits.max_input_data_rows !=
            MILENA_BYTECODE_DATA_MAX_INPUT_DATA_ROWS ||
        view.limits.max_input_columns !=
            MILENA_BYTECODE_DATA_MAX_INPUT_COLUMNS ||
        view.limits.max_csv_field_bytes !=
            MILENA_BYTECODE_DATA_MAX_CSV_FIELD_BYTES ||
        !view.span_present || view.source_line == 0u ||
        view.source_column == 0u) {
        fprintf(stderr, "compiled data plan did not preserve its canonical fields\n");
        return 1;
    }
    return 0;
}

static int test_source_sum_count_and_source_independence(void) {
    uint8_t *bytes = NULL;
    size_t length = 0u;
    MilenaError error;
    char mutable_source[sizeof(".analisis resumen {\n"
        "  variable valor numerica\n"
        "  dataset cargar datos(\"missing-input.csv\")\n"
        "  .resumir dataset { #suma(\"valor\") }\n"
        "  .exportar { (\"summary.json\") }\n"
        "}\n")];
    memcpy(mutable_source, sum_source, sizeof(mutable_source));

    CHECK(milena_bytecode_compile_source(mutable_source, &bytes, &length,
                                          &error) == MILENA_OK,
          error.message);
    CHECK(bytes != NULL && length > MILENA_BYTECODE_HEADER_SIZE,
          "canonical source must produce a nonempty encoded plan");
    memset(mutable_source, 'x', sizeof(mutable_source) - 1u);
    CHECK(verify_expected(bytes, length, MILENA_BYTECODE_DATA_SUM,
                          "missing-input.csv", "summary.json", "valor",
                          "valor_suma") == 0,
          "SUM source plan must own copied path/name bytes and decode canonically");
    free(bytes);
    bytes = NULL;
    length = 0u;

    CHECK(milena_bytecode_compile_data_source(count_source, &bytes, &length,
                                              &error) == MILENA_OK,
          error.message);
    CHECK(verify_expected(bytes, length, MILENA_BYTECODE_DATA_COUNT,
                          "count-input.csv", "count.json", "valor",
                          "valor_conteo") == 0,
          "COUNT source plan must decode to its canonical result name");
    free(bytes);
    return 0;
}

static int test_hir_strings_are_copied(void) {
    MilenaCanonicalProgram program;
    MilenaError error;
    uint8_t *bytes = NULL;
    size_t length = 0u;
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program, sum_source, &error) ==
              MILENA_OK,
          error.message);
    CHECK(program.data_hir != NULL,
          "canonical parser must build the complete data HIR for the fixture");
    CHECK(milena_bytecode_compile_data_hir(program.data_hir, &bytes, &length,
                                           &error) == MILENA_OK,
          error.message);
    CHECK(bytes != NULL && length > MILENA_BYTECODE_HEADER_SIZE,
          "direct complete-HIR lowering must return encoded bytes");
    program.data_hir->source.path[0] = 'X';
    program.data_hir->export_path[0] = 'X';
    program.data_hir->declared_schema[0].name[0] = 'X';
    program.data_hir->operations[0].as.summarize.aggregates[0].input.name[0] = 'X';
    program.data_hir->operations[0].as.summarize.aggregates[0].output_name[0] = 'X';
    milena_canonical_program_release(&program);
    CHECK(verify_expected(bytes, length, MILENA_BYTECODE_DATA_SUM,
                          "missing-input.csv", "summary.json", "valor",
                          "valor_suma") == 0,
          "serialized plan must not retain source, HIR, or name pointers");
    free(bytes);
    return 0;
}

static int reject_source(const char *source, bool expect_parse_error) {
    MilenaError error;
    uint8_t *bytes = (uint8_t *)malloc(1u);
    size_t length = 123u;
    if (!bytes) return 1;
    MilenaStatus status = milena_bytecode_compile_data_source(
        source, &bytes, &length, &error);
    if (status == MILENA_OK || bytes != NULL || length != 0u) {
        fprintf(stderr, "unsupported/malformed source returned bytecode\n");
        free(bytes);
        return 1;
    }
    if (expect_parse_error) {
        if (status != MILENA_ERR_PARSE && status != MILENA_ERR_TYPE &&
            status != MILENA_ERR_UNSUPPORTED) {
            fprintf(stderr, "malformed source returned an unrelated diagnostic\n");
            return 1;
        }
    } else if (status != MILENA_ERR_UNSUPPORTED &&
               status != MILENA_ERR_TYPE && status != MILENA_ERR_PARSE) {
        fprintf(stderr, "unsupported data shape lacked a structured diagnostic: %s\n",
                milena_status_name(status));
        return 1;
    }
    return 0;
}

static int test_closed_shape_failures(void) {
    static const char *const cases[] = {
        /* Two CSV sources. */
        ".analisis resumen { variable valor numerica "
        "dataset cargar datos(\"one.csv\") dataset cargar datos(\"two.csv\") "
        ".resumir dataset { #suma(\"valor\") } .exportar { (\"x.json\") } }",
        /* Streaming CSV source. */
        ".analisis resumen { variable valor numerica "
        "dataset cargar flujo(\"one.csv\", 128) "
        ".resumir dataset { #suma(\"valor\") } .exportar { (\"x.json\") } }",
        /* Missing declaration. */
        ".analisis resumen { dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") } .exportar { (\"x.json\") } }",
        /* Non-numeric declaration. */
        ".analisis resumen { variable valor texto dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") } .exportar { (\"x.json\") } }",
        /* Multiple declarations. */
        ".analisis resumen { variable valor numerica variable extra numerica "
        "dataset cargar datos(\"one.csv\") .resumir dataset { #suma(\"valor\") } "
        ".exportar { (\"x.json\") } }",
        /* Aggregate refers to an unrelated name. */
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"otro\") } .exportar { (\"x.json\") } }",
        /* More than one aggregate in the only summary operation. */
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") #conteo(\"valor\") } "
        ".exportar { (\"x.json\") } }",
        /* A second operation block is not represented. */
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") } "
        ".agrupar dataset { #por(\"grupo\") #suma(\"valor\") } "
        ".exportar { (\"x.json\") } }",
        /* Missing required JSON export. */
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") } }",
        /* Duplicate exports. */
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") } .exportar { (\"x.json\") } "
        ".exportar { (\"y.json\") } }",
        /* The existing HIR builder represents mean, but v1.3 does not. */
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #media(\"valor\") } .exportar { (\"x.json\") } }"
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i)
        CHECK(reject_source(cases[i], false) == 0,
              "every unsupported or incomplete HIR must fail closed with no bytes");
    CHECK(reject_source(
        ".analisis resumen { variable valor numerica dataset cargar datos(\"one.csv\") "
        ".resumir dataset { #suma(\"valor\") } .exportar { (\"x.json\") }",
        true) == 0,
        "malformed canonical source must fail without returning bytes");
    return 0;
}

int main(void) {
    CHECK(test_source_sum_count_and_source_independence() == 0,
          "source lowerer SUM/COUNT tests failed");
    CHECK(test_hir_strings_are_copied() == 0,
          "complete-HIR ownership test failed");
    CHECK(test_closed_shape_failures() == 0,
          "closed-shape fail-closed tests failed");
    puts("bytecode data compiler tests: canonical source/HIR to verified MLBC v1.3 plans OK");
    return 0;
}
