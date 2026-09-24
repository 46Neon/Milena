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

    /* Canonical parse annotates scalar expression types and structured operands. */
    milena_canonical_program_init(&program);
    const char *typed_source =
        "funcion doble(x) { retornar x * 2; } "
        "variable resultado = doble(5);";
    CHECK(milena_canonical_program_parse(&program, typed_source, &error) == MILENA_OK,
          error.message);
    ASTNode *function = program.ast->children[0];
    ASTNode *return_node = function->children[1]->children[0];
    ASTNode *multiply = return_node->children[0];
    CHECK(multiply->type == AST_EXPRESION_OPERACION &&
          multiply->operator_kind == AST_OPERATOR_MULTIPLY,
          "la operación AST conserva un operador tipado");
    CHECK(multiply->left_operand == multiply->children[0] &&
          multiply->right_operand == multiply->children[1],
          "los operandos estructurados deben ser alias coherentes de los hijos propios");
    CHECK(multiply->value_type == AST_VALUE_NUMBER &&
          multiply->left_operand->value_type == AST_VALUE_NUMBER &&
          multiply->right_operand->value_type == AST_VALUE_NUMBER,
          "la semántica debe anotar tipos escalares numéricos");
    CHECK(program.ast->children[1]->children[0]->type == AST_EXPRESION_LLAMADA &&
          program.ast->children[1]->children[0]->value_type == AST_VALUE_NUMBER,
          "las llamadas de función deben recibir anotación de tipo de retorno");
    milena_canonical_program_release(&program);

    /* Boolean arithmetic is rejected with a type error carrying the operator span. */
    milena_canonical_program_init(&program);
    const char *bad_types =
        "funcion invalida() { retornar verdadero + 1; }";
    CHECK(milena_canonical_program_parse(&program, bad_types, &error) == MILENA_ERR_TYPE,
          "la suma de booleano y número debe rechazarse semánticamente");
    CHECK(error.line == 1 && error.column > 0 && strstr(error.message, "aritmética") != NULL,
          "el error tipado debe conservar mensaje y posición de fuente");
    CHECK(program.ast == NULL,
          "un fallo semántico debe liberar el AST no publicado");
    milena_canonical_program_release(&program);

    puts("OK: canonical compiler boundary, typed scalar AST and source diagnostics");
    return 0;
}
