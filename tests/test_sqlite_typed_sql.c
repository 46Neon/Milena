#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "query_plan.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", (message)); \
            return 1; \
        } \
    } while (0)

static ASTNode *parse_sql(const char *source) {
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    bool failed = parser.has_error;
    if (failed)
        fprintf(stderr, "SQL parser: %s\n", parser.error.message);
    parser_release(&parser);
    if (failed) {
        ast_destroy(program);
        return NULL;
    }
    return program;
}

static bool database_was_not_created(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return true;
    fclose(file);
    return false;
}

static int test_valid_ast_and_plan(void) {
    (void)remove("sqlite-typed-no-open.db");
    const char *source =
        "sql desde \"sqlite-typed-no-open.db\" {\n"
        "  tabla personas (id entero, nombre texto, activo booleano);\n"
        "  insertar en personas (id, nombre, activo) valores (7, \"x'); DROP TABLE personas; --\", verdadero);\n"
        "  seleccionar id, nombre de personas donde id = 7;\n"
        "}";
    ASTNode *root = parse_sql(source);
    CHECK(root && root->type == AST_PROGRAMA && root->child_count == 1,
          "parser rejected the bounded typed SQL example");
    ASTNode *program = root->children[0];
    CHECK(program->type == AST_SQL_PROGRAM && program->child_count == 3,
          "SQL root/schema/insert/select AST shape is wrong");
    ASTNode *schema = program->children[0];
    ASTNode *insert = program->children[1];
    ASTNode *select = program->children[2];
    CHECK(schema->type == AST_SQL_TABLE_SCHEMA &&
          strcmp(schema->value, "personas") == 0 && schema->child_count == 3 &&
          schema->children[0]->sql_type == AST_SQL_TYPE_INTEGER &&
          schema->children[1]->sql_type == AST_SQL_TYPE_TEXT &&
          schema->children[2]->sql_type == AST_SQL_TYPE_BOOLEAN,
          "schema AST does not explicitly preserve table/column types");
    CHECK(insert->type == AST_SQL_TYPED_INSERT && insert->child_count == 3 &&
          insert->children[0]->type == AST_SQL_TABLE_REFERENCE &&
          insert->children[1]->type == AST_SQL_INSERT_COLUMN_LIST &&
          insert->children[1]->child_count == 3 &&
          insert->children[1]->children[0]->type == AST_SQL_INSERT_COLUMN &&
          insert->children[2]->type == AST_SQL_INSERT_VALUE_LIST &&
          insert->children[2]->child_count == 3 &&
          insert->children[2]->children[0]->sql_type == AST_SQL_TYPE_INTEGER &&
          insert->children[2]->children[1]->sql_type == AST_SQL_TYPE_TEXT &&
          insert->children[2]->children[2]->sql_type == AST_SQL_TYPE_BOOLEAN &&
          strcmp(insert->children[2]->children[1]->value,
                 "x'); DROP TABLE personas; --") == 0,
          "typed INSERT AST omits its explicit table, columns, values or types");
    CHECK(select->type == AST_SQL_TYPED_SELECT && select->child_count == 3 &&
          select->children[0]->type == AST_SQL_TABLE_REFERENCE &&
          select->children[1]->type == AST_SQL_PROJECTION_LIST &&
          select->children[1]->child_count == 2 &&
          select->children[2]->type == AST_SQL_FILTER &&
          select->children[2]->children[0]->type == AST_SQL_FILTER_COLUMN &&
          select->children[2]->children[1]->sql_operator == AST_SQL_OPERATOR_EQUAL &&
          select->children[2]->children[2]->type == AST_SQL_PARAMETER &&
          select->children[2]->children[2]->sql_type == AST_SQL_TYPE_INTEGER,
          "typed SELECT AST omits its explicit table/projection/filter/operator/parameter");

    MilenaError error;
    CHECK(milena_sql_semantic_validate(program, &error) == MILENA_OK,
          error.message);
    MilenaSqlExecutionPlan plan = {0};
    CHECK(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_OK,
          error.message);
    CHECK(plan.operation_count == 3 &&
          plan.operations[0].kind == MILENA_SQL_PLAN_SCHEMA &&
          plan.operations[1].kind == MILENA_SQL_PLAN_TYPED_INSERT &&
          plan.operations[1].typed_table == insert->children[0] &&
          plan.operations[1].typed_schema == schema &&
          plan.operations[1].insert_column_count == 3 &&
          plan.operations[1].insert_value_count == 3 &&
          plan.operations[1].insert_columns[1].type == AST_SQL_TYPE_TEXT &&
          plan.operations[1].insert_values[2].type == AST_SQL_TYPE_BOOLEAN &&
          strcmp(plan.operations[1].statement,
                 "INSERT INTO \"personas\" (\"id\", \"nombre\", \"activo\") VALUES (?, ?, ?)") == 0 &&
          plan.operations[1].parameter_count == 3 &&
          plan.operations[1].parameters[0].kind == MILENA_SQL_PLAN_INT64 &&
          plan.operations[1].parameters[0].value.i64 == 7 &&
          plan.operations[1].parameters[1].kind == MILENA_SQL_PLAN_TEXT &&
          strcmp(plan.operations[1].parameters[1].value.text.data,
                 "x'); DROP TABLE personas; --") == 0 &&
          plan.operations[1].parameters[2].kind == MILENA_SQL_PLAN_INT64 &&
          plan.operations[1].parameters[2].value.i64 == 1 &&
          plan.operations[2].kind == MILENA_SQL_PLAN_TYPED_SELECT &&
          plan.operations[2].typed_table == select->children[0] &&
          plan.operations[2].typed_schema == schema &&
          plan.operations[2].projection_count == 2 &&
          plan.operations[2].filter_column == select->children[2]->children[0] &&
          plan.operations[2].filter_operator == select->children[2]->children[1] &&
          plan.operations[2].typed_parameter == select->children[2]->children[2] &&
          strcmp(plan.operations[2].statement,
                 "SELECT \"id\", \"nombre\" FROM \"personas\" WHERE \"id\" = ?") == 0 &&
          plan.operations[2].parameter_count == 1 &&
          plan.operations[2].parameters[0].kind == MILENA_SQL_PLAN_INT64 &&
          plan.operations[2].parameters[0].value.i64 == 7,
          "validated plan did not generate typed INSERT bindings and SELECT plan");
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(root);
    CHECK(database_was_not_created("sqlite-typed-no-open.db"),
          "parser/semantic/planner unexpectedly opened the declared DB path");
    return 0;
}

static int test_update_ast_and_plan(void) {
    const char *path = "sqlite-typed-update-no-open.db";
    (void)remove(path);
    const char *source =
        "sql desde \"sqlite-typed-update-no-open.db\" {\n"
        "  tabla personas (id entero, nombre texto, activo booleano);\n"
        "  actualizar personas establecer nombre = \"nuevo'); DROP TABLE personas; --\", activo = falso donde id >= 7;\n"
        "}";
    ASTNode *root = parse_sql(source);
    CHECK(root && root->child_count == 1,
          "parser rejected the typed UPDATE example");
    ASTNode *program = root->children[0];
    CHECK(program->child_count == 2 &&
          program->children[1]->type == AST_SQL_TYPED_UPDATE,
          "typed UPDATE is not explicit in the SQL AST");
    ASTNode *update = program->children[1];
    CHECK(update->child_count == 3 &&
          update->children[0]->type == AST_SQL_TABLE_REFERENCE &&
          update->children[1]->type == AST_SQL_UPDATE_ASSIGNMENT_LIST &&
          update->children[1]->child_count == 2 &&
          update->children[1]->children[0]->type == AST_SQL_UPDATE_ASSIGNMENT &&
          update->children[1]->children[0]->child_count == 2 &&
          update->children[1]->children[0]->children[0]->type == AST_SQL_UPDATE_COLUMN &&
          update->children[1]->children[0]->children[1]->sql_type == AST_SQL_TYPE_TEXT &&
          strcmp(update->children[1]->children[0]->children[1]->value,
                 "nuevo'); DROP TABLE personas; --") == 0 &&
          update->children[2]->type == AST_SQL_UPDATE_FILTER &&
          update->children[2]->children[0]->type == AST_SQL_FILTER_COLUMN &&
          update->children[2]->children[1]->sql_operator == AST_SQL_OPERATOR_GREATER_EQUAL &&
          update->children[2]->children[2]->sql_type == AST_SQL_TYPE_INTEGER,
          "typed UPDATE AST omits explicit assignments, values, or filter fields");
    MilenaError error;
    CHECK(milena_sql_semantic_validate(program, &error) == MILENA_OK,
          error.message);
    MilenaSqlExecutionPlan plan = {0};
    CHECK(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_OK,
          error.message);
    MilenaSqlPlanOperation *op = &plan.operations[1];
    CHECK(op->kind == MILENA_SQL_PLAN_TYPED_UPDATE &&
          op->typed_table == update->children[0] && op->typed_schema == program->children[0] &&
          op->update_assignment_count == 2 && op->update_assignments[0].type == AST_SQL_TYPE_TEXT &&
          op->filter_column == update->children[2]->children[0] &&
          op->filter_operator == update->children[2]->children[1] &&
          op->typed_parameter == update->children[2]->children[2] &&
          strcmp(op->statement,
                 "UPDATE \"personas\" SET \"nombre\" = ?, \"activo\" = ? WHERE \"id\" >= ?") == 0 &&
          op->parameter_count == 3 && op->parameters[0].kind == MILENA_SQL_PLAN_TEXT &&
          strcmp(op->parameters[0].value.text.data,
                 "nuevo'); DROP TABLE personas; --") == 0 &&
          op->parameters[1].kind == MILENA_SQL_PLAN_INT64 &&
          op->parameters[1].value.i64 == 0 &&
          op->parameters[2].kind == MILENA_SQL_PLAN_INT64 &&
          op->parameters[2].value.i64 == 7,
          "typed UPDATE planner did not produce fixed SQL and ordered typed bindings");
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(root);
    CHECK(database_was_not_created(path),
          "parser/semantic/planner unexpectedly opened the UPDATE database path");
    return 0;
}

static int test_typed_null_bindings(void) {
    const char *path = "sqlite-typed-null-no-open.db";
    (void)remove(path);
    const char *source =
        "sql desde \"sqlite-typed-null-no-open.db\" {\n"
        "  tabla personas (id entero, nombre texto, activo booleano);\n"
        "  insertar en personas (id, activo) valores (8, nulo);\n"
        "  actualizar personas establecer activo = nulo donde id = 8;\n"
        "}";
    ASTNode *root = parse_sql(source);
    CHECK(root && root->child_count == 1,
          "parser rejected typed NULL INSERT/UPDATE");
    ASTNode *program = root->children[0];
    MilenaError error;
    CHECK(milena_sql_semantic_validate(program, &error) == MILENA_OK,
          error.message);
    MilenaSqlExecutionPlan plan = {0};
    CHECK(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_OK,
          error.message);
    CHECK(plan.operation_count == 3 &&
          plan.operations[1].kind == MILENA_SQL_PLAN_TYPED_INSERT &&
          plan.operations[1].insert_values[1].type == AST_SQL_TYPE_BOOLEAN &&
          plan.operations[1].parameters[1].kind == MILENA_SQL_PLAN_NULL &&
          plan.operations[2].kind == MILENA_SQL_PLAN_TYPED_UPDATE &&
          plan.operations[2].update_assignments[0].type == AST_SQL_TYPE_BOOLEAN &&
          plan.operations[2].parameters[0].kind == MILENA_SQL_PLAN_NULL &&
          plan.operations[2].parameters[1].kind == MILENA_SQL_PLAN_INT64,
          "typed NULL did not retain the column type and null binding in the plan");
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(root);
    CHECK(database_was_not_created(path),
          "typed NULL parsing/semantic/planning unexpectedly opened SQLite");
    (void)remove(path);
    return 0;
}

static int test_semantic_rejection_before_open(void) {
    struct Case {
        const char *name;
        const char *statement;
        MilenaStatus expected;
    } cases[] = {
        {"undeclared table", "seleccionar id de fantasma donde id = 7;", MILENA_ERR_TYPE},
        {"unknown projection", "seleccionar ausente de personas donde id = 7;", MILENA_ERR_TYPE},
        {"unknown filter column", "seleccionar id de personas donde ausente = 7;", MILENA_ERR_TYPE},
        {"parameter type mismatch", "seleccionar id de personas donde id = \"7\";", MILENA_ERR_TYPE},
        {"unsupported comparison operator", "seleccionar id de personas donde id > 7;", MILENA_ERR_UNSUPPORTED},
        {"INSERT undeclared table", "insertar en fantasma (id) valores (7);", MILENA_ERR_TYPE},
        {"INSERT unknown column", "insertar en personas (ausente) valores (7);", MILENA_ERR_TYPE},
        {"INSERT type mismatch", "insertar en personas (id) valores (\"7\");", MILENA_ERR_TYPE},
        {"INSERT column/value arity mismatch", "insertar en personas (id) valores (7, 8);", MILENA_ERR_PARSE},
        {"INSERT duplicate column", "insertar en personas (id, id) valores (7, 8);", MILENA_ERR_TYPE},
        {"UPDATE undeclared table", "actualizar fantasma establecer id = 8 donde id = 7;", MILENA_ERR_TYPE},
        {"UPDATE unknown assignment column", "actualizar personas establecer ausente = 8 donde id = 7;", MILENA_ERR_TYPE},
        {"UPDATE unknown filter column", "actualizar personas establecer id = 8 donde ausente = 7;", MILENA_ERR_TYPE},
        {"UPDATE assignment type mismatch", "actualizar personas establecer nombre = 7 donde id = 7;", MILENA_ERR_TYPE},
        {"UPDATE filter type mismatch", "actualizar personas establecer nombre = \"x\" donde id = \"7\";", MILENA_ERR_TYPE},
        {"UPDATE duplicate assignment", "actualizar personas establecer id = 8, id = 9 donde id = 7;", MILENA_ERR_TYPE}
    };
    const char *path = "sqlite-typed-no-open.db";
    (void)remove(path);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char source[1024];
        int size = snprintf(source, sizeof(source),
            "sql desde \"%s\" { tabla personas (id entero, nombre texto); %s }",
            path, cases[i].statement);
        CHECK(size > 0 && (size_t)size < sizeof(source),
              "could not format invalid semantic case");
        ASTNode *root = parse_sql(source);
        CHECK(root && root->child_count == 1,
              "parser did not produce AST for a semantic-rejection case");
        MilenaError error;
        MilenaStatus status = milena_sql_semantic_validate(
            root->children[0], &error);
        CHECK(status == cases[i].expected, cases[i].name);
        CHECK(database_was_not_created(path),
              "semantic rejection created/opened the deliberately missing database");
        ast_destroy(root);
    }
    (void)remove(path);
    return 0;
}

int main(void) {
    CHECK(test_valid_ast_and_plan() == 0, "valid AST/plan case failed");
    CHECK(test_update_ast_and_plan() == 0, "typed UPDATE AST/plan case failed");
    CHECK(test_typed_null_bindings() == 0, "typed NULL binding case failed");
    CHECK(test_semantic_rejection_before_open() == 0,
          "pre-open semantic rejection case failed");
    puts("typed SQLite AST/semantic/planner checks passed");
    return 0;
}
