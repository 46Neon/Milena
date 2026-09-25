#include "canonical_compiler.h"
#include "typed_ir.h"
#include "typed_bytecode.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FALLO: %s\n", (message)); \
            return 1; \
        } \
    } while (0)

static bool run_vm_case(const MilenaCanonicalProgram *program,
                        const uint8_t *bytecode, size_t bytecode_size,
                        const char *function_name, const double *numeric_arguments,
                        size_t argument_count, MilenaIRType expected_type,
                        double expected_number, bool expected_boolean,
                        char *diagnostic, size_t diagnostic_capacity) {
    MilenaVMValue arguments[3] = {{0}};
    MilenaVMValue result = {0};
    uint32_t entry_symbol = 0;
    if (!program || !program->typed_module || !bytecode || !function_name ||
        (argument_count && !numeric_arguments) ||
        argument_count > sizeof(arguments) / sizeof(arguments[0])) {
        (void)snprintf(diagnostic, diagnostic_capacity,
                       "invalid source-to-VM test fixture");
        return false;
    }
    for (size_t i = 0; i < program->typed_module->function_count; ++i)
        if (strcmp(program->typed_module->functions[i].name, function_name) == 0) {
            entry_symbol = program->typed_module->functions[i].symbol_id;
            break;
        }
    if (!entry_symbol) {
        (void)snprintf(diagnostic, diagnostic_capacity,
                       "VM fixture function %s was not found", function_name);
        return false;
    }
    for (size_t i = 0; i < argument_count; ++i) {
        arguments[i].type = MILENA_IR_TYPE_F64;
        arguments[i].as.f64 = numeric_arguments[i];
    }
    if (!vm_run(bytecode, bytecode_size, entry_symbol,
                argument_count ? arguments : NULL, argument_count,
                NULL, &result, diagnostic, diagnostic_capacity)) return false;
    if (result.type != expected_type) {
        (void)snprintf(diagnostic, diagnostic_capacity,
                       "%s returned type %d, expected %d", function_name,
                       (int)result.type, (int)expected_type);
        return false;
    }
    if (expected_type == MILENA_IR_TYPE_F64 && result.as.f64 != expected_number) {
        (void)snprintf(diagnostic, diagnostic_capacity,
                       "%s returned %.17g, expected %.17g", function_name,
                       result.as.f64, expected_number);
        return false;
    }
    if (expected_type == MILENA_IR_TYPE_BOOL &&
        result.as.boolean != expected_boolean) {
        (void)snprintf(diagnostic, diagnostic_capacity,
                       "%s returned %s, expected %s", function_name,
                       result.as.boolean ? "true" : "false",
                       expected_boolean ? "true" : "false");
        return false;
    }
    return true;
}

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
          error.column == 1 && strstr(error.message, "COMANDO_SST") != NULL,
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

    /* End-to-end canonical compile slice: source passes the official lexer,
       parser and semantic frontend, then the program owns verified typed IR. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion calcular() { variable x = 10; x = x * 2; "
          "retornar x + 3; }", &error) == MILENA_OK, error.message);
    CHECK(program.hir && program.hir->function_count == 1 &&
          program.hir->functions[0].body_count == 3,
          "el frontend debe entregar una función tipada con declaración, asignación y retorno");
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK, error.message);
    MilenaIRProgram *typed_body = program.typed_ir;
    CHECK(typed_body != NULL &&
          milena_ir_program_validate(typed_body, error.message, sizeof(error.message)),
          "la entrada de compilación debe publicar IR tipada verificada");
    CHECK(typed_body->block_count == 1 && typed_body->count == 6 &&
          typed_body->instructions[2].opcode == MILENA_IR_MUL_F64 &&
          typed_body->instructions[4].opcode == MILENA_IR_ADD_F64 &&
          typed_body->instructions[5].opcode == MILENA_IR_RETURN &&
          typed_body->instructions[5].result_type == MILENA_IR_TYPE_F64,
          "la compilación canónica debe producir SSA aritmético tipado y retorno verificado");
    CHECK(typed_body->instructions[2].operand1_id ==
              typed_body->instructions[0].result_id &&
          typed_body->instructions[4].operand1_id ==
              typed_body->instructions[2].result_id,
          "reasignación y usos posteriores deben referenciar el valor SSA vigente");
    typed_body = NULL; /* The prior pointer is invalidated by successful replacement. */
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL &&
          milena_ir_program_validate(program.typed_ir, error.message,
                                     sizeof(error.message)),
          "recompilar debe sustituir por una IR poseída y verificada");
    milena_canonical_program_release(&program);
    CHECK(program.typed_ir == NULL,
          "liberar el programa canónico debe liberar su IR tipada poseída");

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion calcular() { retornar (10 - 2) * 3 / 4; }", &error) ==
              MILENA_OK, error.message);
    typed_body = milena_ir_program_create();
    CHECK(typed_body != NULL, "no se pudo reservar IR tipada para operadores");
    CHECK(milena_ir_program_lower_scalar_function_body(typed_body,
          &program.hir->functions[0], error.message, sizeof(error.message)),
          error.message);
    CHECK(typed_body->count == 8 &&
          typed_body->instructions[2].opcode == MILENA_IR_SUB_F64 &&
          typed_body->instructions[4].opcode == MILENA_IR_MUL_F64 &&
          typed_body->instructions[6].opcode == MILENA_IR_DIV_F64 &&
          typed_body->instructions[7].opcode == MILENA_IR_RETURN,
          "resta, multiplicación y división deben preservar el AST tipado");
    milena_ir_program_destroy(typed_body);
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion elegir() { variable base = 1; "
          "si (base > 0) { retornar base; } sino { retornar 0; } }",
          &error) == MILENA_OK, error.message);
    typed_body = milena_ir_program_create();
    CHECK(typed_body != NULL, "no se pudo reservar IR tipada para si/sino");
    CHECK(milena_ir_program_lower_scalar_function_body(typed_body,
          &program.hir->functions[0], error.message, sizeof(error.message)),
          error.message);
    CHECK(typed_body->block_count == 3 && typed_body->count == 7 &&
          typed_body->instructions[2].opcode == MILENA_IR_GT_F64 &&
          typed_body->instructions[3].opcode == MILENA_IR_COND_BRANCH &&
          typed_body->instructions[3].target_true == 2 &&
          typed_body->instructions[3].target_false == 3 &&
          typed_body->blocks[0].successor_true == 2 &&
          typed_body->blocks[0].successor_false == 3 &&
          typed_body->instructions[4].opcode == MILENA_IR_RETURN &&
          typed_body->instructions[6].opcode == MILENA_IR_RETURN,
          "si/sino debe bajar a CFG tipado con retornos en ambas ramas");
    milena_ir_program_destroy(typed_body);
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion sin_retorno_falso(x) { si (x > 0) { retornar x; } }",
          &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && program.typed_ir == NULL,
          "un si terminal sin sino no debe aparentar un retorno en el camino falso");
    milena_canonical_program_release(&program);

    /* Nonterminal source-level si/sino assignment branches lower to a CFG
       merge with typed block parameters and per-edge SSA arguments. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion abs_like(x) { variable y = 0; "
          "si (x > 0) { y = x; } sino { y = 0 - x; } retornar y; }",
          &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir,
          error.message);
    typed_body = program.typed_ir;
    CHECK(typed_body->block_count == 4 && typed_body->parameter_count == 2 &&
          typed_body->edge_argument_count == 2 &&
          typed_body->blocks[0].successor_true == 2 &&
          typed_body->blocks[0].successor_false == 3 &&
          typed_body->blocks[1].successor_true == 4 &&
          typed_body->blocks[2].successor_true == 4 &&
          typed_body->parameters[1].block_id == 4 &&
          typed_body->parameters[1].type == MILENA_IR_TYPE_F64 &&
          typed_body->instructions[typed_body->count - 1].opcode ==
              MILENA_IR_RETURN &&
          typed_body->instructions[typed_body->count - 1].operand1_id ==
              typed_body->parameters[1].value_id &&
          milena_ir_program_validate(typed_body, error.message,
                                     sizeof(error.message)),
          "el si/sino con asignaciones debe fusionar el valor vigente con un parámetro de bloque verificado");
    CHECK(typed_body->edge_arguments[0].source_block_id == 2 &&
          typed_body->edge_arguments[0].target_block_id == 4 &&
          typed_body->edge_arguments[1].source_block_id == 3 &&
          typed_body->edge_arguments[1].target_block_id == 4,
          "cada rama debe suministrar su valor al bloque de merge correspondiente");
    milena_canonical_program_release(&program);

    /* Nested assignment conditionals stay on the canonical typed-IR route and
       use inner/outer CFG merges with explicit edge arguments. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion nested_merge(x, y) { variable z = 0; "
          "si (x > 0) { si (y > 0) { z = x; } sino { z = y; } } "
          "sino { z = 0 - x; } retornar z; }", &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL,
          error.message);
    typed_body = program.typed_ir;
    CHECK(typed_body->block_count == 7 &&
          typed_body->parameter_count == 4 &&
          typed_body->edge_argument_count == 4 &&
          milena_ir_program_validate(typed_body, error.message,
                                     sizeof(error.message)),
          "las condiciones anidadas deben bajar a CFG tipado con merges SSA verificados");
    {
        size_t conditional_branches = 0;
        bool inner_merge = false, outer_merge = false;
        for (size_t i = 0; i < typed_body->count; ++i)
            if (typed_body->instructions[i].opcode == MILENA_IR_COND_BRANCH)
                ++conditional_branches;
        for (size_t i = 0; i < typed_body->parameter_count; ++i) {
            if (typed_body->parameters[i].block_id == 7) inner_merge = true;
            if (typed_body->parameters[i].block_id == 4) outer_merge = true;
        }
        CHECK(conditional_branches == 2 && inner_merge && outer_merge &&
              typed_body->instructions[typed_body->count - 1].opcode ==
                  MILENA_IR_RETURN &&
              typed_body->instructions[typed_body->count - 1].operand1_id ==
                  typed_body->parameters[typed_body->parameter_count - 1].value_id,
              "los dos niveles deben preservar condición, parámetro de merge y retorno final");
    }
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion nested_no_else(x, y) { variable z = 0; "
          "si (x > 0) { si (y > 0) { z = x; } z = y; } "
          "sino { z = 0; } retornar z; }", &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL, error.message);
    {
        char validation_error[256] = {0};
        CHECK(milena_ir_program_validate(program.typed_ir, validation_error,
                                         sizeof(validation_error)),
              validation_error);
    }
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion nested_local(x, y) { variable z = 0; "
          "si (x > 0) { variable valido = y > 0; "
          "si (valido) { variable temporal = x; z = temporal; } "
          "sino { variable temporal = y; z = temporal; } } "
          "sino { variable temporal = 0; z = temporal; } retornar z; }",
          &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL,
          error.message);
    {
        char validation_error[256] = {0};
        CHECK(milena_ir_program_validate(program.typed_ir, validation_error,
                                         sizeof(validation_error)),
              validation_error);
    }
    milena_canonical_program_release(&program);

    /* Branch-local declarations lower into branch-scoped SSA bindings and do
       not escape the conditional merge. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion sin_sino(x) { variable y = 0; "
          "si (x > 0) { y = x; } retornar y; }", &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL, error.message);
    {
        char validation_error[256] = {0};
        CHECK(milena_ir_program_validate(program.typed_ir, validation_error,
                                         sizeof(validation_error)),
              validation_error);
    }
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion local_branch(x) { variable y = 0; "
          "si (x > 0) { variable temporal = x; y = temporal; } "
          "sino { variable temporal = 0; y = temporal; } retornar y; }",
          &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL, error.message);
    {
        char validation_error[256] = {0};
        CHECK(milena_ir_program_validate(program.typed_ir, validation_error,
                                         sizeof(validation_error)),
              validation_error);
    }
    /* Poison a post-merge reference with a branch-local symbol ID. Lowering
       must reject it instead of turning a lexical local into a phi input. */
    {
        size_t branch_local_id = program.hir->functions[0].body[1]
            ->as.conditional.then_body[0]->resolved_symbol_id;
        MilenaHIRExpression *returned = program.hir->functions[0]
            .body[2]->as.expression;
        CHECK(branch_local_id != 0 && returned != NULL,
              "la prueba debe localizar el binding local y el retorno");
        returned->resolved_symbol_id = branch_local_id;
        milena_ir_program_destroy(program.typed_ir);
        program.typed_ir = NULL;
        CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
                  MILENA_ERR_UNSUPPORTED && program.typed_ir == NULL &&
              strstr(error.message, "binding") != NULL,
              "un binding local de rama no debe escapar al bloque merge");
    }
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion comparar() { variable a = 1 == 2; variable b = 1 != 2; "
          "variable c = 1 < 2; variable d = 1 <= 2; "
          "variable e = 1 > 2; variable f = 1 >= 2; "
          "variable g = verdadero; retornar 0; }",
          &error) == MILENA_OK, error.message);
    typed_body = milena_ir_program_create();
    CHECK(typed_body != NULL, "no se pudo reservar IR tipada para comparaciones");
    CHECK(milena_ir_program_lower_scalar_function_body(typed_body,
          &program.hir->functions[0], error.message, sizeof(error.message)),
          error.message);
    CHECK(typed_body->count == 21 &&
          typed_body->instructions[2].opcode == MILENA_IR_EQ_F64 &&
          typed_body->instructions[5].opcode == MILENA_IR_NE_F64 &&
          typed_body->instructions[8].opcode == MILENA_IR_LT_F64 &&
          typed_body->instructions[11].opcode == MILENA_IR_LE_F64 &&
          typed_body->instructions[14].opcode == MILENA_IR_GT_F64 &&
          typed_body->instructions[17].opcode == MILENA_IR_GE_F64 &&
          typed_body->instructions[18].opcode == MILENA_IR_CONST_BOOL &&
          typed_body->instructions[18].result_type == MILENA_IR_TYPE_BOOL &&
          typed_body->instructions[2].result_type == MILENA_IR_TYPE_BOOL &&
          typed_body->instructions[20].opcode == MILENA_IR_RETURN,
          "las comparaciones numéricas deben bajar a valores bool explícitos");
    milena_ir_program_destroy(typed_body);
    milena_canonical_program_release(&program);

    /* Parameterized source -> typed HIR -> canonical IR keeps its function
       signature and defines every input as an entry-block SSA value. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion identidad(n) { retornar n; }", &error) == MILENA_OK,
          error.message);
    CHECK(program.hir && program.hir->function_count == 1 &&
          program.hir->functions[0].parameter_count == 1 &&
          program.hir->functions[0].parameters[0].value_type == MILENA_HIR_NUMBER &&
          program.hir->functions[0].parameters[0].resolved_symbol_id != 0,
          "la HIR debe conservar la declaración tipada y enlazada del parámetro");
    program.hir->functions[0].parameters[0].value_type = MILENA_HIR_BOOLEAN;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && program.typed_ir == NULL &&
          program.ast != NULL && program.hir != NULL,
          "un tipo de parámetro no admitido debe fallar cerrado sin publicar IR");
    program.hir->functions[0].parameters[0].value_type = MILENA_HIR_NUMBER;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir != NULL,
          error.message);
    typed_body = program.typed_ir;
    CHECK(typed_body->has_function_signature &&
          typed_body->signature.parameter_count == 1 &&
          typed_body->signature.parameter_types[0] == MILENA_IR_TYPE_F64 &&
          typed_body->signature.return_type == MILENA_IR_TYPE_F64 &&
          typed_body->parameter_count == 1 &&
          typed_body->parameters[0].block_id == 1 &&
          typed_body->parameters[0].value_id == 1 &&
          typed_body->parameters[0].type == MILENA_IR_TYPE_F64 &&
          typed_body->instructions[0].opcode == MILENA_IR_RETURN &&
          typed_body->instructions[0].operand1_id == 1 &&
          milena_ir_program_validate(typed_body, error.message,
                                    sizeof(error.message)),
          "el parámetro debe ser definición SSA de entrada usada por el retorno");
    MilenaIRType saved_parameter_type = typed_body->signature.parameter_types[0];
    typed_body->parameters[0].type = MILENA_IR_TYPE_BOOL;
    CHECK(!milena_ir_program_validate(typed_body, error.message,
                                      sizeof(error.message)) &&
          strstr(error.message, "function signature") != NULL,
          "el verificador debe rechazar tipo de entrada distinto a la firma");
    typed_body->parameters[0].type = MILENA_IR_TYPE_F64;
    typed_body->signature.parameter_types[0] = MILENA_IR_TYPE_BOOL;
    CHECK(!milena_ir_program_validate(typed_body, error.message,
                                      sizeof(error.message)),
          "el verificador debe rechazar firma incompatible con el valor SSA de entrada");
    typed_body->signature.parameter_types[0] = saved_parameter_type;
    MilenaIRType *saved_parameter_types = typed_body->signature.parameter_types;
    typed_body->signature.parameter_types = NULL;
    typed_body->signature.parameter_count = 0;
    CHECK(!milena_ir_program_validate(typed_body, error.message,
                                      sizeof(error.message)) &&
          strstr(error.message, "function signature") != NULL,
          "el verificador debe rechazar una firma con aridad distinta a la entrada");
    typed_body->signature.parameter_types = saved_parameter_types;
    typed_body->signature.parameter_count = 1;
    CHECK(milena_ir_program_validate(typed_body, error.message,
                                     sizeof(error.message)),
          error.message);
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion combinar(a, b) { retornar a + b; }", &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir &&
          program.typed_ir->signature.parameter_count == 2 &&
          program.typed_ir->parameter_count == 2 &&
          program.typed_ir->parameters[0].value_id == 1 &&
          program.typed_ir->parameters[1].value_id == 2 &&
          program.typed_ir->instructions[0].opcode == MILENA_IR_ADD_F64 &&
          program.typed_ir->instructions[0].operand1_id == 1 &&
          program.typed_ir->instructions[0].operand2_id == 2,
          "la lowering debe preservar orden de firma y lecturas de dos parámetros");
    milena_canonical_program_release(&program);

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
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_ir &&
          program.typed_ir->signature.parameter_count == 1 &&
          program.typed_ir->parameter_count == 1 &&
          program.typed_ir->blocks[0].successor_true == 2 &&
          program.typed_ir->blocks[0].successor_false == 3 &&
          milena_ir_program_validate(program.typed_ir, error.message,
                                     sizeof(error.message)),
          "los parámetros de función deben dominar la condición y retornos de ambas ramas");
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

    /* Canonical data HIR: source identity, typed product/filter/projection,
       schema binding, strict consumption, execution and transactional cleanup. */
    milena_canonical_program_init(&program);
    const char *data_source =
        ".analisis ventas {\n"
        " dataset cargar datos(\"entrada.csv\")\n"
        " .transformar dataset { #total(\"precio * cantidad\") }\n"
        " .filtrar { #condicion(\"total >= 10\") }\n"
        " .seleccionar { #columnas(\"id,total,ciudad\") }\n"
        " .exportar { (\"salida.json\") }\n"
        "}\n";
    CHECK(milena_canonical_program_parse(&program, data_source, &error) == MILENA_OK,
          error.message);
    CHECK(program.data_hir != NULL && program.hir == NULL &&
          strcmp(program.data_hir->source.path, "entrada.csv") == 0 &&
          program.data_hir->source.resolved_dataset_id != 0 &&
          strcmp(program.data_hir->export_path, "salida.json") == 0 &&
          program.data_hir->operation_count == 3 &&
          program.data_hir->operations[0].resolved_dataset_id ==
              program.data_hir->source.resolved_dataset_id,
          "la HIR debe poseer fuente, transformación, filtro, proyección y destino de exportación");
    MilenaCanonicalCompilerInput data_input = {0};
    CHECK(milena_canonical_compiler_input(&program, &data_input, &error) == MILENA_ERR_DATA &&
          data_input.ast == NULL && data_input.data_hir == NULL,
          "la HIR de datos no debe consumirse antes de enlazar el esquema");

    MilenaArray ids = {0}, prices = {0}, quantities = {0};
    size_t data_shape[] = {3};
    int64_t id_values[] = {1, 2, 3};
    int64_t quantity_values[] = {3, 2, 1};
    double price_values[] = {2.0, 5.0, 4.0};
    const char *city_values[] = {"Caracas", "Maracaibo", "Mérida"};
    CHECK(milena_array_from_i64(&ids, 1, data_shape, id_values, &error) == MILENA_OK &&
          milena_array_from_f64(&prices, 1, data_shape, price_values, &error) == MILENA_OK &&
          milena_array_from_i64(&quantities, 1, data_shape, quantity_values, &error) == MILENA_OK,
          error.message);
    MilenaTable data_table;
    milena_table_init(&data_table);
    CHECK(milena_table_add_column_copy(&data_table, "id", &ids, NULL, &error) == MILENA_OK &&
          milena_table_add_column_copy(&data_table, "precio", &prices, NULL, &error) == MILENA_OK &&
          milena_table_add_column_copy(&data_table, "cantidad", &quantities, NULL, &error) == MILENA_OK &&
          milena_table_add_string_column_copy(&data_table, "ciudad", city_values, 3, NULL,
                                              &error) == MILENA_OK,
          error.message);
    CHECK(milena_table_set_metadata(&data_table, MILENA_HIR_DATASET_PATH_METADATA,
                                    "entrada.csv", &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) == MILENA_OK,
          error.message);
    CHECK(program.data_hir->schema_bound &&
          program.data_hir->operations[0].as.product.left.resolved_column_index == 1 &&
          program.data_hir->operations[0].as.product.left.type == MILENA_HIR_COLUMN_NUMERIC &&
          program.data_hir->operations[2].as.select.columns[1].resolved_column_index == 4 &&
          program.data_hir->operations[2].as.select.columns[2].type == MILENA_HIR_COLUMN_TEXT,
          "el binder debe resolver columnas, tipos, formas e identidades estables");
    CHECK(milena_canonical_compiler_input(&program, &data_input, &error) == MILENA_OK &&
          data_input.data_hir == program.data_hir && data_input.table == &data_table,
          "la entrada estricta debe exponer la HIR de datos ligada");
    MilenaTable data_output;
    milena_table_init(&data_output);
    CHECK(milena_canonical_program_execute_data(&program, NULL, &data_output, &error) == MILENA_OK,
          error.message);
    CHECK(data_output.row_count == 1 && data_output.column_count == 3 &&
          milena_table_column_index(&data_output, "ciudad") == 2 &&
          milena_table_column_index(&data_output, "total") == 1,
          "la ruta de ejecución HIR debe ejecutar producto, filtro y selección");
    const void *cell = NULL;
    CHECK(milena_table_get_array_value(&data_output,
          (size_t)milena_table_column_index(&data_output, "id"), 0, &cell, &error) == MILENA_OK &&
          *(const int64_t *)cell == 2,
          "el filtro HIR debe conservar solo la fila que cumple la condición enlazada");
    CHECK(milena_table_get_array_value(&data_output,
          (size_t)milena_table_column_index(&data_output, "total"), 0, &cell, &error) == MILENA_OK &&
          *(const double *)cell == 10.0,
          "el HIR debe materializar el producto numérico en la tabla canónica");

    MilenaTable sentinel;
    milena_table_init(&sentinel);
    CHECK(milena_table_clone(&sentinel, &data_output, &error) == MILENA_OK,
          error.message);
    MilenaHIRResourcePolicy tiny_policy = {
        .max_input_rows = 3, .max_output_rows = 0, .max_columns = 5
    };
    CHECK(milena_canonical_program_execute_data(&program, &tiny_policy,
          &sentinel, &error) == MILENA_ERR_OVERFLOW && sentinel.row_count == 1 &&
          milena_table_column_index(&sentinel, "total") >= 0,
          "el límite de filas debe fallar sin publicar ni filtrar parcialmente la salida previa");

    milena_canonical_program_release(&program);
    CHECK(milena_table_validate(&data_table, &error) == MILENA_OK,
          "liberar la HIR no debe destruir la tabla prestada");
    milena_table_destroy(&sentinel);
    milena_table_destroy(&data_output);

    /* Schema binding rejects unknown columns and type mismatches with spans. */
    milena_canonical_program_init(&program);
    const char *unknown_column_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".filtrar { #condicion(\"inexistente > 0\") } }";
    CHECK(milena_canonical_program_parse(&program, unknown_column_source, &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) == MILENA_ERR_TYPE &&
          error.line > 0 && error.column > 0 && strstr(error.message, "Columna no declarada") != NULL &&
          program.table == NULL,
          "el enlace debe rechazar nombres de columna desconocidos con ubicación y limpieza");
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    const char *wrong_type_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".filtrar { #condicion(\"ciudad > 0\") } }";
    CHECK(milena_canonical_program_parse(&program, wrong_type_source, &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) == MILENA_ERR_TYPE &&
          error.line > 0 && strstr(error.message, "columna numérica") != NULL,
          "el enlace debe rechazar tipos de filtro incompatibles con span");
    milena_canonical_program_release(&program);

    /* Group and summary lower to typed aggregate HIR and execute transactionally. */
    milena_canonical_program_init(&program);
    const char *group_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".agrupar dataset { #por(\"ciudad\") #suma(\"precio\") } }";
    CHECK(milena_canonical_program_parse(&program, group_source, &error) == MILENA_OK,
          error.message);
    CHECK(program.data_hir && program.data_hir->operation_count == 1 &&
          program.data_hir->operations[0].kind == MILENA_HIR_DATA_GROUP &&
          program.data_hir->operations[0].as.group.aggregate_count == 1,
          "la agrupación debe bajar a claves y agregados HIR tipados");
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) == MILENA_OK,
          error.message);
    MilenaTable grouped_output;
    milena_table_init(&grouped_output);
    CHECK(milena_canonical_program_execute_data(&program, NULL, &grouped_output,
                                                &error) == MILENA_OK,
          error.message);
    int grouped_sum = milena_table_column_index(&grouped_output, "precio_suma");
    CHECK(grouped_output.row_count == 3 && grouped_sum >= 0,
          "la agrupación HIR debe materializar todas las claves y la suma");
    CHECK(milena_table_get_array_value(&grouped_output, (size_t)grouped_sum, 0,
          &cell, &error) == MILENA_OK && *(const double *)cell == 2.0,
          "el agregado HIR debe conservar el resultado numérico del grupo inicial");
    MilenaTable grouped_sentinel;
    milena_table_init(&grouped_sentinel);
    CHECK(milena_table_clone(&grouped_sentinel, &grouped_output, &error) == MILENA_OK,
          error.message);
    MilenaHIRResourcePolicy group_policy = {
        .max_input_rows = 3, .max_output_rows = 2, .max_columns = 4
    };
    CHECK(milena_canonical_program_execute_data(&program, &group_policy,
          &grouped_sentinel, &error) == MILENA_ERR_OVERFLOW &&
          grouped_sentinel.row_count == 3 &&
          milena_table_column_index(&grouped_sentinel, "precio_suma") >= 0,
          "un límite HIR de grupos debe preservar la salida previa íntegra");
    milena_canonical_program_release(&program);
    CHECK(milena_table_validate(&data_table, &error) == MILENA_OK,
          "liberar la HIR de agrupación no debe destruir la tabla prestada");
    milena_table_destroy(&grouped_sentinel);
    milena_table_destroy(&grouped_output);

    milena_canonical_program_init(&program);
    const char *summary_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".resumir dataset { #suma(\"precio\") #conteo(\"ciudad\") } }";
    CHECK(milena_canonical_program_parse(&program, summary_source, &error) == MILENA_OK,
          error.message);
    CHECK(program.data_hir && program.data_hir->operation_count == 1 &&
          program.data_hir->operations[0].kind == MILENA_HIR_DATA_SUMMARIZE,
          "el resumen debe bajar a agregados HIR tipados");
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) == MILENA_OK,
          error.message);
    MilenaTable summary_output;
    milena_table_init(&summary_output);
    CHECK(milena_canonical_program_execute_data(&program, NULL, &summary_output,
                                                &error) == MILENA_OK,
          error.message);
    int summary_sum = milena_table_column_index(&summary_output, "precio_suma");
    int summary_count = milena_table_column_index(&summary_output, "ciudad_conteo");
    CHECK(summary_output.row_count == 1 && summary_sum >= 0 && summary_count >= 0,
          "el resumen HIR debe materializar una fila con nombres estables");
    CHECK(milena_table_get_array_value(&summary_output, (size_t)summary_sum, 0,
          &cell, &error) == MILENA_OK && *(const double *)cell == 11.0,
          "la suma HIR debe calcular el total de la columna");
    CHECK(milena_table_get_array_value(&summary_output, (size_t)summary_count, 0,
          &cell, &error) == MILENA_OK && *(const int64_t *)cell == 3,
          "el conteo HIR debe aceptar columnas de texto enlazadas");
    milena_canonical_program_release(&program);
    milena_table_destroy(&summary_output);

    milena_canonical_program_init(&program);
    const char *unknown_group_column =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".agrupar dataset { #por(\"inexistente\") #suma(\"precio\") } }";
    CHECK(milena_canonical_program_parse(&program, unknown_group_column, &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) ==
          MILENA_ERR_TYPE && strstr(error.message, "Columna no declarada") != NULL,
          "la clave de agrupación debe resolverse contra el esquema y rechazar faltantes");
    milena_canonical_program_release(&program);

    milena_canonical_program_init(&program);
    const char *mismatched_dataset_source =
        ".analisis ventas { dataset cargar datos(\"otro.csv\") "
        ".filtrar { #condicion(\"precio > 0\") } }";
    CHECK(milena_canonical_program_parse(&program, mismatched_dataset_source, &error) ==
          MILENA_OK, error.message);
    CHECK(milena_canonical_program_bind_table(&program, &data_table, &error) ==
          MILENA_ERR_DATA && program.table == NULL &&
          !program.data_hir->schema_bound &&
          strstr(error.message, "no acredita la ruta") != NULL,
          "el binder debe rechazar tablas cuya provenance no coincide con el dataset HIR");
    milena_canonical_program_release(&program);

    /* A bounded join resolves a second dataset and both key bindings. */
    MilenaArray right_ids = {0};
    int64_t right_id_values[] = {1, 2, 3};
    const char *segment_values[] = {"A", "B", "C"};
    CHECK(milena_array_from_i64(&right_ids, 1, data_shape, right_id_values,
                                &error) == MILENA_OK, error.message);
    MilenaTable catalog_table;
    milena_table_init(&catalog_table);
    CHECK(milena_table_add_column_copy(&catalog_table, "id", &right_ids, NULL,
                                        &error) == MILENA_OK &&
          milena_table_add_string_column_copy(&catalog_table, "segment",
                segment_values, 3, NULL, &error) == MILENA_OK &&
          milena_table_set_metadata(&catalog_table,
                MILENA_HIR_DATASET_PATH_METADATA, "catalogo.csv",
                &error) == MILENA_OK, error.message);
    milena_canonical_program_init(&program);
    const char *join_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".unir { #derecha(\"catalogo.csv\") #clave(\"id\") } }";
    CHECK(milena_canonical_program_parse(&program, join_source, &error) == MILENA_OK,
          error.message);
    CHECK(program.data_hir && program.data_hir->operation_count == 1 &&
          program.data_hir->operations[0].kind == MILENA_HIR_DATA_JOIN &&
          program.data_hir->operations[0].span.has_source_span &&
          program.data_hir->operations[0].as.join.left_key.span.has_source_span &&
          program.data_hir->operations[0].as.join.right_key.span.has_source_span &&
          program.data_hir->operations[0].as.join.right_dataset_id !=
              program.data_hir->source.resolved_dataset_id,
          "el join debe bajar con identidad separada para el dataset derecho");
    CHECK(milena_canonical_program_bind_tables(&program, &data_table,
          &catalog_table, &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_compiler_input(&program, &data_input, &error) == MILENA_OK &&
          data_input.right_table == &catalog_table,
          "la entrada HIR estricta debe exponer ambos datasets ligados");
    MilenaTable joined_output;
    milena_table_init(&joined_output);
    CHECK(milena_canonical_program_execute_data(&program, NULL, &joined_output,
                                                &error) == MILENA_OK,
          error.message);
    int segment_column = milena_table_column_index(&joined_output, "segment");
    const char *segment = NULL;
    CHECK(joined_output.row_count == 3 && segment_column >= 0 &&
          milena_table_get_string(&joined_output, (size_t)segment_column, 2,
                                  &segment, &error) == MILENA_OK &&
          strcmp(segment, "C") == 0,
          "el join HIR debe materializar filas y columnas del dataset derecho");
    MilenaTable join_sentinel;
    milena_table_init(&join_sentinel);
    CHECK(milena_table_clone(&join_sentinel, &joined_output, &error) == MILENA_OK,
          error.message);
    MilenaHIRResourcePolicy join_limit = {
        .max_input_rows = 3, .max_output_rows = 2, .max_columns = 6
    };
    CHECK(milena_canonical_program_execute_data(&program, &join_limit,
          &join_sentinel, &error) == MILENA_ERR_OVERFLOW &&
          join_sentinel.row_count == 3 &&
          milena_table_column_index(&join_sentinel, "segment") >= 0,
          "el límite de salida join debe preservar la tabla anterior");
    milena_canonical_program_release(&program);
    milena_table_destroy(&join_sentinel);
    milena_table_destroy(&joined_output);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program, join_source, &error) == MILENA_OK,
          error.message);
    MilenaTable wrong_path_table;
    milena_table_init(&wrong_path_table);
    CHECK(milena_table_clone(&wrong_path_table, &catalog_table, &error) == MILENA_OK &&
          milena_table_set_metadata(&wrong_path_table,
                MILENA_HIR_DATASET_PATH_METADATA, "otro.csv", &error) == MILENA_OK,
          error.message);
    CHECK(milena_canonical_program_bind_tables(&program, &data_table,
          &wrong_path_table, &error) == MILENA_ERR_DATA &&
          program.table == NULL && program.right_table == NULL &&
          !program.data_hir->schema_bound,
          "el binder debe rechazar provenance de ruta derecha discordante");
    milena_canonical_program_release(&program);
    milena_table_destroy(&wrong_path_table);

    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program, join_source, &error) == MILENA_OK,
          error.message);
    MilenaTable wrong_key_table;
    milena_table_init(&wrong_key_table);
    const char *text_ids[] = {"1", "2", "3"};
    CHECK(milena_table_add_string_column_copy(&wrong_key_table, "id", text_ids,
                3, NULL, &error) == MILENA_OK &&
          milena_table_add_string_column_copy(&wrong_key_table, "segment",
                segment_values, 3, NULL, &error) == MILENA_OK &&
          milena_table_set_metadata(&wrong_key_table,
                MILENA_HIR_DATASET_PATH_METADATA, "catalogo.csv",
                &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_bind_tables(&program, &data_table,
          &wrong_key_table, &error) == MILENA_ERR_TYPE &&
          program.table == NULL && program.right_table == NULL,
          "el binder debe rechazar tipos incompatibles entre claves de join");
    milena_canonical_program_release(&program);
    milena_table_destroy(&wrong_key_table);
    milena_table_destroy(&catalog_table);
    milena_array_release(&right_ids);

    /* Canonical cleaning preserves command order and executes transactionally. */
    MilenaArray cleanup_ids = {0};
    int64_t cleanup_id_values[] = {1, 1, 2};
    const char *cleanup_names[] = {"A", "A", NULL};
    bool cleanup_name_valid[] = {true, true, false};
    CHECK(milena_array_from_i64(&cleanup_ids, 1, data_shape,
                                cleanup_id_values, &error) == MILENA_OK,
          error.message);
    MilenaTable cleanup_table;
    milena_table_init(&cleanup_table);
    CHECK(milena_table_add_column_copy(&cleanup_table, "id", &cleanup_ids,
                NULL, &error) == MILENA_OK &&
          milena_table_add_string_column_copy(&cleanup_table, "name",
                cleanup_names, 3, cleanup_name_valid, &error) == MILENA_OK &&
          milena_table_set_metadata(&cleanup_table,
                MILENA_HIR_DATASET_PATH_METADATA, "limpieza.csv",
                &error) == MILENA_OK, error.message);
    milena_canonical_program_init(&program);
    const char *cleanup_source =
        ".analisis limpieza { dataset cargar datos(\"limpieza.csv\") "
        ".limpiar dataset { #nulos(\"eliminar\") #duplicados(\"eliminar\") } }";
    CHECK(milena_canonical_program_parse(&program, cleanup_source, &error) ==
          MILENA_OK, error.message);
    CHECK(program.data_hir && program.data_hir->operation_count == 2 &&
          program.data_hir->operations[0].kind == MILENA_HIR_DATA_DROP_NULLS &&
          program.data_hir->operations[1].kind == MILENA_HIR_DATA_DROP_DUPLICATES &&
          program.data_hir->operations[0].span.has_source_span &&
          program.data_hir->operations[1].span.has_source_span,
          "la limpieza debe bajar comandos ordenados con spans locales a HIR tipada");
    CHECK(milena_canonical_program_bind_table(&program, &cleanup_table,
                                               &error) == MILENA_OK,
          error.message);
    MilenaTable cleanup_output;
    milena_table_init(&cleanup_output);
    CHECK(milena_canonical_program_execute_data(&program, NULL, &cleanup_output,
                                                 &error) == MILENA_OK,
          error.message);
    CHECK(cleanup_output.row_count == 1 && cleanup_output.column_count == 2,
          "el HIR debe eliminar primero nulos y luego duplicados");
    MilenaTable cleanup_sentinel;
    milena_table_init(&cleanup_sentinel);
    CHECK(milena_table_clone(&cleanup_sentinel, &cleanup_output, &error) ==
          MILENA_OK, error.message);
    MilenaHIRResourcePolicy cleanup_limit = {
        .max_input_rows = 3, .max_output_rows = 0, .max_columns = 2
    };
    CHECK(milena_canonical_program_execute_data(&program, &cleanup_limit,
          &cleanup_sentinel, &error) == MILENA_ERR_OVERFLOW &&
          cleanup_sentinel.row_count == 1,
          "el error de límite tras limpiar debe conservar intacta la salida previa");
    milena_canonical_program_release(&program);
    milena_table_destroy(&cleanup_sentinel);
    milena_table_destroy(&cleanup_output);
    milena_table_destroy(&cleanup_table);
    milena_array_release(&cleanup_ids);

    milena_table_destroy(&data_table);
    milena_array_release(&ids);
    milena_array_release(&prices);
    milena_array_release(&quantities);

    milena_canonical_program_init(&program);
    const char *unsupported_cleaning_action =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".limpiar dataset { #nulos(\"rellenar\") } }";
    CHECK(milena_canonical_program_parse(&program, unsupported_cleaning_action,
                                         &error) == MILENA_OK, error.message);
    CHECK(program.data_hir == NULL &&
          milena_canonical_compiler_input(&program, &data_input, &error) ==
              MILENA_ERR_UNSUPPORTED && data_input.ast == NULL &&
          data_input.data_hir == NULL,
          "una acción de limpieza no implementada debe fallar cerrado y sin vista parcial");
    milena_canonical_program_release(&program);

    /* A source module with a direct scalar call lowers only on the canonical
       Spanish lexer/parser/semantic -> HIR -> typed-IR path. Calls are
       nonrecursive, numeric-parameter, scalar-returning, and carry owned,
       arbitrary-length SSA argument slices. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion combinar(x, z) { retornar x + z; } "
          "funcion principal(y) { variable listo = verdadero; "
          "retornar combinar(y, y); }", &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_module &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)), error.message);
    CHECK(program.typed_module->function_count == 2 &&
          program.typed_module->functions[0].symbol_id ==
              program.hir->functions[0].resolved_symbol_id &&
          program.typed_module->functions[0].body->signature.parameter_count == 2 &&
          program.typed_module->functions[1].body->signature.parameter_count == 1 &&
          program.typed_module->functions[1].body->signature.return_type ==
              MILENA_IR_TYPE_F64 && program.typed_ir ==
              program.typed_module->functions[0].body,
          "la IR de módulo debe conservar identidad, firma, cuerpo y vista compatible");
    MilenaIRInstruction *direct_call = NULL;
    for (size_t i = 0; i < program.typed_module->functions[1].body->count; ++i)
        if (program.typed_module->functions[1].body->instructions[i].opcode ==
            MILENA_IR_CALL)
            direct_call = &program.typed_module->functions[1].body->instructions[i];
    CHECK(direct_call && direct_call->integer_immediate ==
              (int64_t)program.typed_module->functions[0].symbol_id &&
          direct_call->target_true == 0 && direct_call->operand1_id == 0 &&
          direct_call->operand2_id == 0 && direct_call->call_argument_count == 2 &&
          direct_call->call_argument_offset + direct_call->call_argument_count <=
              program.typed_module->functions[1].body->call_argument_count &&
          program.typed_module->functions[1].body->call_arguments[
              direct_call->call_argument_offset] == 1 &&
          program.typed_module->functions[1].body->call_arguments[
              direct_call->call_argument_offset + 1] == 1 &&
          direct_call->result_type == MILENA_IR_TYPE_F64,
          "una llamada directa debe enlazar symbol ID y todo el vector SSA tipado");
    {
        const int64_t saved_target = direct_call->integer_immediate;
        direct_call->integer_immediate = UINT32_MAX;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe rechazar un symbol ID válido pero desconocido en el módulo");
        direct_call->integer_immediate = 0;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe rechazar una llamada sin target estable");
        direct_call->integer_immediate = saved_target;
        const size_t saved_offset = direct_call->call_argument_offset;
        direct_call->call_argument_offset = program.typed_module->functions[1]
            .body->call_argument_count;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe rechazar segmentos de argumentos fuera de límites");
        direct_call->call_argument_offset = saved_offset;
        const size_t saved_arity = direct_call->call_argument_count;
        direct_call->call_argument_count = 1;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe comparar la aridad completa con la firma del destino");
        direct_call->call_argument_count = saved_arity;
        const size_t first_argument_index = direct_call->call_argument_offset;
        const uint32_t saved_argument = program.typed_module->functions[1]
            .body->call_arguments[first_argument_index];
        program.typed_module->functions[1].body->call_arguments[
            first_argument_index] = 2; /* The caller's BOOL local, not its F64 parameter. */
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe rechazar argumentos con tipo distinto a la firma");
        program.typed_module->functions[1].body->call_arguments[
            first_argument_index] = saved_argument;
        const size_t second_argument_index = first_argument_index + 1;
        const uint32_t saved_second_argument = program.typed_module->functions[1]
            .body->call_arguments[second_argument_index];
        program.typed_module->functions[1].body->call_arguments[
            second_argument_index] = 2; /* The caller's BOOL local as parameter two. */
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe comprobar el tipo de cada argumento del vector");
        program.typed_module->functions[1].body->call_arguments[
            second_argument_index] = saved_second_argument;
        const MilenaIRType saved_return = direct_call->result_type;
        direct_call->result_type = MILENA_IR_TYPE_BOOL;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe rechazar el tipo de salida de llamada incorrecto");
        direct_call->result_type = saved_return;
        const MilenaIRType saved_callee_return = program.typed_module->functions[0]
            .return_type;
        program.typed_module->functions[0].return_type = MILENA_IR_TYPE_BOOL;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "la firma del destino debe coincidir con el output del cuerpo callee");
        program.typed_module->functions[0].return_type = saved_callee_return;
        CHECK(milena_ir_module_validate(program.typed_module, error.message,
                                        sizeof(error.message)), error.message);
    }
    /* Invalid binding and signature annotations are negative source-to-IR
       tests: parsing remains canonical, but no partial IR module is published. */
    milena_canonical_program_release(&program);
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion duplicar(x) { retornar x + x; } "
          "funcion principal(y) { retornar duplicar(y); }", &error) == MILENA_OK,
          error.message);
    MilenaHIRExpression *call_expression = program.hir->functions[1]
        .body[0]->as.expression;
    const size_t saved_symbol = call_expression->resolved_symbol_id;
    call_expression->resolved_symbol_id = UINT32_MAX;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && !program.typed_module && !program.typed_ir &&
          strstr(error.message, "unresolved") != NULL,
          "una llamada con identidad HIR no resuelta debe fallar cerrado");
    call_expression->resolved_symbol_id = saved_symbol;
    const size_t saved_argument_count = call_expression->as.call.argument_count;
    call_expression->as.call.argument_count = 2;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && !program.typed_module && !program.typed_ir,
          "la aridad HIR inválida debe fallar sin publicar módulo parcial");
    call_expression->as.call.argument_count = saved_argument_count;
    call_expression->as.call.arguments[0]->value_type = MILENA_HIR_BOOLEAN;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && !program.typed_module && !program.typed_ir,
          "el lowering debe rechazar argumentos de llamada con tipo incorrecto");
    call_expression->as.call.arguments[0]->value_type = MILENA_HIR_NUMBER;
    /* Turn the direct call into a resolved self-edge to exercise the explicit
       recursion boundary independently of frontend recursion inference. */
    call_expression->resolved_symbol_id = program.hir->functions[1].resolved_symbol_id;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && !program.typed_module && !program.typed_ir &&
          strstr(error.message, "recursive") != NULL,
          "la recursión directa debe rechazarse explícitamente y sin IR parcial");
    milena_canonical_program_release(&program);
    /* Keep the zero-, one-, and two-argument ABI cases working with the new
       vector representation, including zero-length and adjacent slices. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion cero() { retornar 0; } "
          "funcion uno(x) { retornar x + 1; } "
          "funcion dos(x, y) { retornar x + y; } "
          "funcion usar(x, y) { variable a = cero(); variable b = uno(x); "
          "retornar dos(a, b); }", &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_module &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)), error.message);
    {
        MilenaIRProgram *body = program.typed_module->functions[3].body;
        size_t seen = 0;
        const size_t expected_counts[3] = {0, 1, 2};
        for (size_t i = 0; i < body->count; ++i) {
            const MilenaIRInstruction *instruction = &body->instructions[i];
            if (instruction->opcode != MILENA_IR_CALL) continue;
            CHECK(seen < 3 && instruction->call_argument_count ==
                  expected_counts[seen],
                  "las llamadas de aridad cero, uno y dos deben conservar su aridad");
            ++seen;
        }
        CHECK(seen == 3 && body->call_argument_count == 3,
              "los slices de aridad cero/uno/dos deben coexistir sin huecos incorrectos");
    }
    milena_canonical_program_release(&program);

    /* Three-argument direct calls preserve every SSA argument and verify each
       value against the destination's full signature on the canonical route. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion sumar_tres(a, b, c) { retornar a + b + c; } "
          "funcion usar_tres(x, y, z) { variable listo = verdadero; "
          "retornar sumar_tres(x, y, z); }", &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_module &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)), error.message);
    MilenaIRProgram *three_body = program.typed_module->functions[1].body;
    MilenaIRInstruction *three_call = NULL;
    for (size_t i = 0; i < three_body->count; ++i)
        if (three_body->instructions[i].opcode == MILENA_IR_CALL)
            three_call = &three_body->instructions[i];
    CHECK(program.typed_module->functions[0].parameter_count == 3 &&
          three_call && three_call->integer_immediate ==
              (int64_t)program.typed_module->functions[0].symbol_id &&
          three_call->call_argument_count == 3 &&
          three_call->call_argument_offset <= three_body->call_argument_count &&
          three_call->call_argument_count <= three_body->call_argument_count -
              three_call->call_argument_offset &&
          three_body->call_arguments[three_call->call_argument_offset] == 1 &&
          three_body->call_arguments[three_call->call_argument_offset + 1] == 2 &&
          three_body->call_arguments[three_call->call_argument_offset + 2] == 3,
          "una llamada de tres argumentos conserva los tres IDs SSA en orden");
    {
        size_t third = three_call->call_argument_offset + 2;
        uint32_t saved_id = three_body->call_arguments[third];
        three_body->call_arguments[third] = 4; /* The caller's BOOL local. */
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe comprobar SSA y tipo para cada argumento, incluso el tercero");
        three_body->call_arguments[third] = saved_id;
        size_t saved_count = three_call->call_argument_count;
        three_call->call_argument_count = 4;
        CHECK(!milena_ir_module_validate(program.typed_module, error.message,
                                         sizeof(error.message)),
              "el verificador debe rechazar conteos malformados fuera del slice de argumentos");
        three_call->call_argument_count = saved_count;
        CHECK(milena_ir_module_validate(program.typed_module, error.message,
                                        sizeof(error.message)), error.message);
    }
    MilenaHIRExpression *three_call_expression = program.hir->functions[1]
        .body[1]->as.expression;
    MilenaIRModule *previous_valid_module = program.typed_module;
    MilenaIRProgram *previous_valid_ir = program.typed_ir;
    size_t valid_three_count = three_call_expression->as.call.argument_count;
    three_call_expression->as.call.argument_count = 2;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && program.typed_module == previous_valid_module &&
          program.typed_ir == previous_valid_ir &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)),
          "un fallo de aridad no publica IR parcial ni destruye el módulo previo válido");
    three_call_expression->as.call.argument_count = valid_three_count;
    MilenaHIRValueType saved_third_type = three_call_expression
        ->as.call.arguments[2]->value_type;
    three_call_expression->as.call.arguments[2]->value_type = MILENA_HIR_BOOLEAN;
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_ERR_UNSUPPORTED && program.typed_module == previous_valid_module &&
          program.typed_ir == previous_valid_ir &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)),
          "un tipo de argumento inválido no publica ni altera el módulo previo válido");
    three_call_expression->as.call.arguments[2]->value_type = saved_third_type;
    milena_canonical_program_release(&program);

    /* Forward calls use the same full vector and stable resolved symbol ID. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion usar_tres(x, y, z) { retornar sumar_tres(x, y, z); } "
          "funcion sumar_tres(a, b, c) { retornar a + b + c; }",
          &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_module &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)), error.message);
    MilenaIRProgram *forward_body = program.typed_module->functions[0].body;
    MilenaIRInstruction *forward_ir_call = NULL;
    for (size_t i = 0; i < forward_body->count; ++i)
        if (forward_body->instructions[i].opcode == MILENA_IR_CALL)
            forward_ir_call = &forward_body->instructions[i];
    CHECK(forward_ir_call && forward_ir_call->integer_immediate ==
              (int64_t)program.typed_module->functions[1].symbol_id &&
          forward_ir_call->call_argument_count == 3 &&
          forward_body->call_arguments[forward_ir_call->call_argument_offset] == 1 &&
          forward_body->call_arguments[forward_ir_call->call_argument_offset + 1] == 2 &&
          forward_body->call_arguments[forward_ir_call->call_argument_offset + 2] == 3,
          "la llamada adelantada enlaza symbol ID y todos los argumentos SSA");
    CHECK(milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)), error.message);
    milena_canonical_program_release(&program);

    /* The sole canonical Spanish frontend route lowers, serializes and executes
       only its verified typed bytecode in the internal reference VM. */
    milena_canonical_program_init(&program);
    CHECK(milena_canonical_program_parse(&program,
          "funcion sumar_tres(a, b, c) { retornar a + b + c; } "
          "funcion principal() { retornar sumar_tres(1, 2, 3); }",
          &error) == MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_module &&
          milena_ir_module_validate(program.typed_module, error.message,
                                    sizeof(error.message)), error.message);
    uint32_t entry_symbol = 0;
    for (size_t i = 0; i < program.typed_module->function_count; ++i)
        if (strcmp(program.typed_module->functions[i].name, "principal") == 0)
            entry_symbol = program.typed_module->functions[i].symbol_id;
    CHECK(entry_symbol != 0, "el frontend español debe conservar la identidad de principal");
    uint8_t *canonical_bytecode = NULL;
    size_t canonical_bytecode_size = 0;
    CHECK(milena_bytecode_encode_module(program.typed_module,
          &canonical_bytecode, &canonical_bytecode_size, error.message,
          sizeof(error.message)), error.message);
    MilenaVMValue vm_result = {0};
    CHECK(vm_run(canonical_bytecode, canonical_bytecode_size,
          entry_symbol, NULL, 0, NULL, &vm_result, error.message,
          sizeof(error.message)), error.message);
    CHECK(vm_result.type == MILENA_IR_TYPE_F64 && vm_result.as.f64 == 6.0,
          "fuente española→IR tipada→bytecode MLBC→VM debe devolver 6");
    free(canonical_bytecode);
    milena_canonical_program_release(&program);

    /* Exercise every currently lowered scalar arithmetic/comparison operator,
       assignment merges with and without sino, nested conditionals, and terminal
       returns through the canonical source -> typed IR -> MLBC -> verified VM path. */
    milena_canonical_program_init(&program);
    const char *scalar_vm_source =
        "funcion sumar(a, b) { retornar a + b; } "
        "funcion restar(a, b) { retornar a - b; } "
        "funcion multiplicar(a, b) { retornar a * b; } "
        "funcion dividir(a, b) { retornar a / b; } "
        "funcion igual(a, b) { si (a == b) { retornar 1; } "
        "sino { retornar 0; } } "
        "funcion distinto(a, b) { si (a != b) { retornar 1; } "
        "sino { retornar 0; } } "
        "funcion menor(a, b) { si (a < b) { retornar 1; } "
        "sino { retornar 0; } } "
        "funcion menor_igual(a, b) { si (a <= b) { retornar 1; } "
        "sino { retornar 0; } } "
        "funcion mayor(a, b) { si (a > b) { retornar 1; } "
        "sino { retornar 0; } } "
        "funcion mayor_igual(a, b) { si (a >= b) { retornar 1; } "
        "sino { retornar 0; } } "
        "funcion elegir(x, y) { variable resultado = 0; "
        "si (x > y) { resultado = x; } sino { resultado = y; } "
        "retornar resultado; } "
        "funcion sin_sino(x, y) { variable resultado = y; "
        "si (x > 0) { resultado = x; } retornar resultado; } "
        "funcion clasificar(x) { variable nivel = 0; "
        "si (x > 0) { si (x > 10) { nivel = 2; } sino { nivel = 1; } } "
        "sino { nivel = 0; } retornar nivel; } "
        "funcion positivo(x) { si (x > 0) { retornar x; } "
        "sino { retornar 0; } } "
        "funcion booleano_local(x) { variable listo = x > 0; "
        "si (listo) { retornar 1; } sino { retornar 0; } }";
    CHECK(milena_canonical_program_parse(&program, scalar_vm_source, &error) ==
              MILENA_OK, error.message);
    CHECK(milena_canonical_program_compile_scalar_ir(&program, &error) ==
              MILENA_OK && program.typed_module, error.message);
    uint8_t *scalar_bytecode = NULL;
    size_t scalar_bytecode_size = 0;
    CHECK(milena_bytecode_encode_module(program.typed_module, &scalar_bytecode,
          &scalar_bytecode_size, error.message, sizeof(error.message)),
          error.message);
    const struct {
        const char *name;
        double arguments[2];
        size_t argument_count;
        MilenaIRType result_type;
        double expected_number;
        bool expected_boolean;
    } scalar_cases[] = {
        {"sumar", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 9.0, false},
        {"restar", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 3.0, false},
        {"multiplicar", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 18.0, false},
        {"dividir", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 2.0, false},
        {"igual", {6.0, 6.0}, 2u, MILENA_IR_TYPE_F64, 1.0, false},
        {"igual", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 0.0, false},
        {"distinto", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 1.0, false},
        {"menor", {3.0, 6.0}, 2u, MILENA_IR_TYPE_F64, 1.0, false},
        {"menor_igual", {3.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 1.0, false},
        {"mayor", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 1.0, false},
        {"mayor_igual", {6.0, 6.0}, 2u, MILENA_IR_TYPE_F64, 1.0, false},
        {"elegir", {6.0, 3.0}, 2u, MILENA_IR_TYPE_F64, 6.0, false},
        {"elegir", {2.0, 9.0}, 2u, MILENA_IR_TYPE_F64, 9.0, false},
        {"sin_sino", {5.0, 9.0}, 2u, MILENA_IR_TYPE_F64, 5.0, false},
        {"sin_sino", {-1.0, 9.0}, 2u, MILENA_IR_TYPE_F64, 9.0, false},
        {"clasificar", {11.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 2.0, false},
        {"clasificar", {5.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 1.0, false},
        {"clasificar", {-1.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 0.0, false},
        {"positivo", {2.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 2.0, false},
        {"positivo", {-2.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 0.0, false},
        {"booleano_local", {1.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 1.0, false},
        {"booleano_local", {-1.0, 0.0}, 1u, MILENA_IR_TYPE_F64, 0.0, false}
    };
    char scalar_vm_error[256] = {0};
    for (size_t i = 0; i < sizeof(scalar_cases) / sizeof(scalar_cases[0]); ++i)
        CHECK(run_vm_case(&program, scalar_bytecode, scalar_bytecode_size,
              scalar_cases[i].name, scalar_cases[i].arguments,
              scalar_cases[i].argument_count, scalar_cases[i].result_type,
              scalar_cases[i].expected_number, scalar_cases[i].expected_boolean,
              scalar_vm_error, sizeof(scalar_vm_error)), scalar_vm_error);
    free(scalar_bytecode);
    milena_canonical_program_release(&program);

    puts("OK: canonical compiler boundary, interprocedural scalar typed IR, typed data HIR, binding, execution and diagnostics");
    return 0;
}
