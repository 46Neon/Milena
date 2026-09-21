#include "canonical_compiler.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FALLO: %s\n", (message)); \
            return 1; \
        } \
    } while (0)

int main(void) {
    MilenaError error;
    MilenaCanonicalProgram program;
    milena_canonical_program_init(&program);

    const char *source =
        ".analisis seguridad { #perfil_avanzado(\"riesgo\") }\n";
    CHECK(milena_canonical_program_parse(&program, source, &error) == MILENA_OK,
          error.message);
    CHECK(program.ast != NULL, "el adaptador no conservó el AST canónico");

    MilenaArray values = {0};
    size_t shape[] = {3};
    int64_t data[] = {1, 2, 3};
    CHECK(milena_array_from_i64(&values, 1, shape, data, &error) == MILENA_OK,
          error.message);
    MilenaTable table;
    milena_table_init(&table);
    CHECK(milena_table_add_column_copy(&table, "riesgo", &values, NULL,
                                       &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_bind_table(&program, &table, &error) == MILENA_OK,
          error.message);

    MilenaCanonicalCompilerInput input = {0};
    CHECK(milena_canonical_compiler_input(&program, &input, &error) == MILENA_OK,
          error.message);
    CHECK(input.ast == program.ast && input.table == &table,
          "la vista del compilador no coincide con AST/MilenaTable");

    /* The table is borrowed; releasing the program must not destroy it. */
    milena_canonical_program_release(&program);
    CHECK(milena_table_validate(&table, &error) == MILENA_OK,
          "el adaptador destruyó una tabla prestada");
    milena_table_destroy(&table);
    milena_array_release(&values);

    /* A missing SST column is rejected at the canonical boundary. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program, source, &error) == MILENA_OK,
          error.message);
    MilenaTable wrong;
    milena_table_init(&wrong);
    CHECK(milena_canonical_program_bind_table(&program, &wrong, &error) != MILENA_OK,
          "se aceptó una tabla sin la columna SST requerida");
    CHECK(strstr(error.message, "columna SST") != NULL,
          "el diagnóstico de columna SST no cruzó la frontera");
    milena_table_destroy(&wrong);
    milena_canonical_program_release(&program);

    puts("OK: canonical compiler boundary with borrowed MilenaTable");
    return 0;
}
