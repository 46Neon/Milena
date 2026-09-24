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
    CHECK(milena_canonical_compiler_input(&program, &input, &error) ==
              MILENA_ERR_UNSUPPORTED,
          "la entrada predeterminada del compilador debe fallar cerrado sin HIR completa");
    CHECK(input.ast == NULL && input.table == NULL && input.hir == NULL,
          "un rechazo del límite predeterminado no debe publicar una vista parcial");
    CHECK(error.code == MILENA_ERR_UNSUPPORTED && error.line == 1 &&
          error.column == 1 && strstr(error.message, "BLOQUE_ANALISIS") != NULL,
          "el rechazo HIR debe nombrar el nodo no representado y conservar su span");
    CHECK(milena_canonical_hir_input(&program, &input, &error) ==
              MILENA_ERR_UNSUPPORTED &&
          input.ast == NULL && input.table == NULL && input.hir == NULL,
          "el alias explícito HIR también debe fallar cerrado");
    CHECK(program.ast != NULL &&
          milena_canonical_compatibility_input(&program, &input, &error) == MILENA_OK &&
          input.ast == program.ast && input.table == &table && input.hir == NULL,
          "la compatibilidad debe requerir opt-in y preservar la vista AST completa");
    CHECK(program.ast != NULL &&
          milena_canonical_compatibility_input(&program, &input, &error) == MILENA_OK &&
          input.ast == program.ast && input.hir == NULL,
          "el rechazo HIR no debe destruir la vista AST de compatibilidad");

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

    /* Canonical function scripts now resolve bindings instead of assuming that
       every identifier or call denotes a numeric value. */
    milena_canonical_program_init(&program);
    const char *resolved_source =
        "funcion principal() { retornar siguiente(4); } "
        "funcion siguiente(x) { variable local = x + 1; retornar local; }";
    CHECK(milena_canonical_program_parse(&program, resolved_source, &error) == MILENA_OK,
          error.message);
    ASTNode *main_function = program.ast->children[0];
    ASTNode *forward_call = main_function->children[1]->children[0]->children[0];
    ASTNode *next_function = program.ast->children[1];
    ASTNode *parameter = next_function->children[0]->children[0];
    ASTNode *local_declaration = next_function->children[1]->children[0];
    ASTNode *parameter_use = local_declaration->children[0]->left_operand;
    ASTNode *local_use = next_function->children[1]->children[1]->children[0];
    CHECK(main_function->resolved_symbol_id != 0 &&
          forward_call->resolved_symbol_id == next_function->resolved_symbol_id,
          "la llamada adelantada debe enlazar a la declaración de función");
    CHECK(parameter->resolved_symbol_id != 0 &&
          parameter_use->resolved_symbol_id == parameter->resolved_symbol_id,
          "el uso debe enlazar al parámetro numérico visible");
    CHECK(local_declaration->resolved_symbol_id != 0 &&
          local_use->resolved_symbol_id == local_declaration->resolved_symbol_id,
          "el uso local debe enlazar a su declaración dentro de la función");

    /* The scalar HIR is program-owned, typed, binding-resolved, and keeps spans. */
    CHECK(program.hir != NULL && program.hir->function_count == 2,
          "la ruta canónica debe construir HIR para el subconjunto numérico resuelto");
    const MilenaHIRFunction *hir_main = &program.hir->functions[0];
    const MilenaHIRFunction *hir_next = &program.hir->functions[1];
    CHECK(strcmp(hir_main->name, "principal") == 0 &&
          hir_main->resolved_symbol_id == main_function->resolved_symbol_id,
          "la HIR debe poseer el nombre y binding de la función");
    CHECK(hir_main->body_count == 1 &&
          hir_main->body[0]->kind == MILENA_HIR_STMT_RETURN &&
          hir_main->body[0]->as.expression->kind == MILENA_HIR_EXPR_CALL &&
          hir_main->body[0]->as.expression->resolved_symbol_id ==
              next_function->resolved_symbol_id,
          "la llamada adelantada debe preservarse como llamada HIR tipada y enlazada");
    CHECK(hir_next->parameter_count == 1 &&
          hir_next->parameters[0].resolved_symbol_id == parameter->resolved_symbol_id &&
          hir_next->body_count == 2 &&
          hir_next->body[0]->kind == MILENA_HIR_STMT_DECLARE &&
          hir_next->body[0]->as.expression->kind == MILENA_HIR_EXPR_BINARY &&
          hir_next->body[0]->as.expression->value_type == MILENA_HIR_NUMBER &&
          hir_next->body[0]->as.expression->as.binary.left->resolved_symbol_id ==
              parameter->resolved_symbol_id &&
          hir_next->body[1]->as.expression->resolved_symbol_id ==
              local_declaration->resolved_symbol_id,
          "las declaraciones, operaciones y usos deben conservar tipos y bindings en HIR");
    CHECK(hir_main->span.has_source_span &&
          hir_main->span.start_offset == main_function->start_offset &&
          hir_main->body[0]->span.end_offset ==
              main_function->children[1]->children[0]->end_offset,
          "la HIR debe conservar spans originales por función y sentencia");
    MilenaCanonicalCompilerInput scalar_input = {0};
    CHECK(milena_canonical_compiler_input(&program, &scalar_input, &error) == MILENA_OK &&
          scalar_input.hir == program.hir,
          "la vista canónica debe exponer la HIR poseída por el programa");
    CHECK(milena_canonical_hir_input(&program, &scalar_input, &error) == MILENA_OK &&
          scalar_input.ast == program.ast && scalar_input.hir == program.hir,
          "la entrada HIR fail-closed debe admitir y preservar el subconjunto tipado");
    milena_canonical_program_release(&program);
    CHECK(program.hir == NULL && program.ast == NULL,
          "liberar el programa debe destruir la HIR y el AST poseídos");

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion elegir(x) { si (x > 0) { retornar x; } sino { retornar 0; } }",
          &error) == MILENA_OK, error.message);
    CHECK(program.hir && program.hir->function_count == 1 &&
          program.hir->functions[0].body_count == 1 &&
          program.hir->functions[0].body[0]->kind == MILENA_HIR_STMT_IF &&
          program.hir->functions[0].body[0]->as.conditional.condition->value_type ==
              MILENA_HIR_BOOLEAN &&
          program.hir->functions[0].body[0]->as.conditional.then_count == 1 &&
          program.hir->functions[0].body[0]->as.conditional.else_count == 1,
          "la HIR debe conservar condición booleana y ramas de si/sino");
    milena_canonical_program_release(&program);

    /* An unresolved variable is an error with the original identifier span. */
    milena_canonical_program_init(&program);
    const char *unknown_name =
        "funcion invalida() { retornar perdida; }";
    CHECK(milena_canonical_program_parse(&program, unknown_name, &error) == MILENA_ERR_TYPE,
          "un identificador libre no debe asumirse numérico");
    CHECK(strstr(error.message, "no declarado") != NULL && error.line == 1 &&
          error.column == (size_t)(strstr(unknown_name, "perdida") - unknown_name + 1),
          "el error de nombre debe señalar el identificador de origen");
    CHECK(program.ast == NULL, "el AST debe liberarse si falla la resolución");
    milena_canonical_program_release(&program);

    /* Unknown function names and call arity are checked before publication. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion invalida() { retornar fantasma(1); }", &error) == MILENA_ERR_TYPE &&
          strstr(error.message, "Función no declarada") != NULL,
          "la llamada a una función inexistente debe rechazarse");
    CHECK(program.ast == NULL, "la falla de llamada debe limpiar el AST parcial");
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion uno(x) { retornar x; } "
          "funcion invalida() { retornar uno(1, 2); }", &error) == MILENA_ERR_TYPE &&
          strstr(error.message, "Cantidad de argumentos") != NULL,
          "la aridad de una llamada debe coincidir con la firma resuelta");
    CHECK(program.ast == NULL, "la falla de aridad debe limpiar el AST parcial");
    milena_canonical_program_release(&program);

    /* Lexical scopes and assignment types are enforced for numeric scripts. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion invalida() { si (verdadero) { variable temporal = 1; } "
          "retornar temporal; }", &error) == MILENA_ERR_TYPE,
          "una variable local del bloque no debe escapar de su ámbito");
    CHECK(program.ast == NULL, "la falla de ámbito debe limpiar el AST parcial");
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion invalida() { variable x = 1; x = verdadero; retornar x; }",
          &error) == MILENA_ERR_TYPE &&
          strstr(error.message, "asignación incompatible") != NULL,
          "una asignación no debe cambiar silenciosamente el tipo de la variable");
    CHECK(program.ast == NULL, "la falla de tipo de asignación debe limpiar el AST parcial");
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion f(x) { retornar x; } "
          "funcion invalida() { retornar f(verdadero); }", &error) == MILENA_ERR_TYPE,
          "la ABI numérica de parámetros no debe aceptar booleanos sin coerción definida");
    CHECK(program.ast == NULL, "la falla de tipo de argumento debe limpiar el AST parcial");
    milena_canonical_program_release(&program);

    puts("OK: canonical compiler boundary, typed AST, binding resolution and source diagnostics");
    return 0;
}
