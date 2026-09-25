#include "canonical_compiler.h"
#include "typed_ir.h"

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

    puts("OK: canonical compiler boundary, typed scalar/data HIR, binding, execution and source diagnostics");
    return 0;
}
