#include "canonical_compiler.h"

#include "language_semantic.h"
#include "lexer.h"
#include "parser.h"

#include <stdlib.h>
#include <string.h>

static void canonical_error(MilenaError *error, MilenaStatus code,
                            const char *message) {
    if (error) milena_error_set(error, code, 0, 0, 0, message);
}

static void hir_source_span(MilenaHIRSourceSpan *span, const ASTNode *node) {
    if (!span || !node) return;
    span->line = node->line > 0 ? (size_t)node->line : 0;
    span->column = node->column > 0 ? (size_t)node->column : 0;
    span->end_line = node->end_line > 0 ? (size_t)node->end_line : 0;
    span->end_column = node->end_column > 0 ? (size_t)node->end_column : 0;
    span->start_offset = node->start_offset;
    span->end_offset = node->end_offset;
    span->has_source_span = node->has_source_span;
}

static bool hir_value_type(ASTValueType source, MilenaHIRValueType *target) {
    if (!target) return false;
    if (source == AST_VALUE_NUMBER) {
        *target = MILENA_HIR_NUMBER;
        return true;
    }
    if (source == AST_VALUE_BOOLEAN) {
        *target = MILENA_HIR_BOOLEAN;
        return true;
    }
    return false;
}

static void hir_expression_release(MilenaHIRExpression *expression);
static void hir_statement_release(MilenaHIRStatement *statement);

static void hir_expression_release(MilenaHIRExpression *expression) {
    if (!expression) return;
    switch (expression->kind) {
        case MILENA_HIR_EXPR_BINARY:
            hir_expression_release(expression->as.binary.left);
            hir_expression_release(expression->as.binary.right);
            break;
        case MILENA_HIR_EXPR_CALL:
            for (size_t i = 0; i < expression->as.call.argument_count; ++i)
                hir_expression_release(expression->as.call.arguments[i]);
            free(expression->as.call.arguments);
            break;
        case MILENA_HIR_EXPR_LITERAL:
        case MILENA_HIR_EXPR_VARIABLE:
            break;
    }
    free(expression);
}

static void hir_statement_release(MilenaHIRStatement *statement) {
    if (!statement) return;
    free(statement->name);
    switch (statement->kind) {
        case MILENA_HIR_STMT_DECLARE:
        case MILENA_HIR_STMT_ASSIGN:
        case MILENA_HIR_STMT_RETURN:
            hir_expression_release(statement->as.expression);
            break;
        case MILENA_HIR_STMT_IF:
            hir_expression_release(statement->as.conditional.condition);
            if (statement->as.conditional.then_body)
                for (size_t i = 0; i < statement->as.conditional.then_count; ++i)
                    hir_statement_release(statement->as.conditional.then_body[i]);
            if (statement->as.conditional.else_body)
                for (size_t i = 0; i < statement->as.conditional.else_count; ++i)
                    hir_statement_release(statement->as.conditional.else_body[i]);
            free(statement->as.conditional.then_body);
            free(statement->as.conditional.else_body);
            break;
    }
    free(statement);
}

static void hir_statement_array_release(MilenaHIRStatement **statements,
                                        size_t count) {
    if (!statements) return;
    for (size_t i = 0; i < count; ++i) hir_statement_release(statements[i]);
    free(statements);
}

static void scalar_hir_release(MilenaScalarHIR *hir) {
    if (!hir) return;
    for (size_t i = 0; i < hir->function_count; ++i) {
        MilenaHIRFunction *function = &hir->functions[i];
        free(function->name);
        for (size_t j = 0; j < function->parameter_count; ++j)
            free(function->parameters[j].name);
        free(function->parameters);
        hir_statement_array_release(function->body, function->body_count);
    }
    free(hir->functions);
    hir_statement_array_release(hir->statements, hir->statement_count);
    free(hir);
}

typedef enum {
    HIR_BUILD_OK,
    HIR_BUILD_UNSUPPORTED,
    HIR_BUILD_MEMORY
} HIRBuildResult;

static MilenaHIRExpression *hir_build_expression(const ASTNode *node,
                                                 HIRBuildResult *result) {
    if (!node || !result) {
        if (result) *result = HIR_BUILD_UNSUPPORTED;
        return NULL;
    }
    MilenaHIRValueType type;
    if (!hir_value_type(node->value_type, &type)) {
        *result = HIR_BUILD_UNSUPPORTED;
        return NULL;
    }
    MilenaHIRExpression *expression = calloc(1, sizeof(*expression));
    if (!expression) {
        *result = HIR_BUILD_MEMORY;
        return NULL;
    }
    expression->value_type = type;
    expression->resolved_symbol_id = node->resolved_symbol_id;
    hir_source_span(&expression->span, node);
    switch (node->type) {
        case AST_EXPRESION_LITERAL:
            expression->kind = MILENA_HIR_EXPR_LITERAL;
            if (type == MILENA_HIR_NUMBER)
                expression->as.number = node->number_value;
            else
                expression->as.boolean = node->number_value != 0.0;
            break;
        case AST_EXPRESION_IDENTIFICADOR:
            if (node->resolved_symbol_id == 0) {
                free(expression);
                *result = HIR_BUILD_UNSUPPORTED;
                return NULL;
            }
            expression->kind = MILENA_HIR_EXPR_VARIABLE;
            break;
        case AST_EXPRESION_OPERACION:
            expression->kind = MILENA_HIR_EXPR_BINARY;
            expression->as.binary.operation = node->operator_kind;
            expression->as.binary.left = hir_build_expression(
                node->left_operand, result);
            if (*result == HIR_BUILD_OK)
                expression->as.binary.right = hir_build_expression(
                    node->right_operand, result);
            if (*result != HIR_BUILD_OK) {
                hir_expression_release(expression);
                return NULL;
            }
            break;
        case AST_EXPRESION_FUNCION:
        case AST_EXPRESION_LLAMADA:
            if (node->resolved_symbol_id == 0) {
                free(expression);
                *result = HIR_BUILD_UNSUPPORTED;
                return NULL;
            }
            expression->kind = MILENA_HIR_EXPR_CALL;
            expression->as.call.argument_count = node->child_count;
            if (node->child_count) {
                if (node->child_count > SIZE_MAX / sizeof(*expression->as.call.arguments)) {
                    free(expression);
                    *result = HIR_BUILD_MEMORY;
                    return NULL;
                }
                expression->as.call.arguments = calloc(
                    node->child_count, sizeof(*expression->as.call.arguments));
                if (!expression->as.call.arguments) {
                    free(expression);
                    *result = HIR_BUILD_MEMORY;
                    return NULL;
                }
                for (size_t i = 0; i < node->child_count; ++i) {
                    expression->as.call.arguments[i] = hir_build_expression(
                        node->children[i], result);
                    if (*result != HIR_BUILD_OK) {
                        hir_expression_release(expression);
                        return NULL;
                    }
                }
            }
            break;
        default:
            free(expression);
            *result = HIR_BUILD_UNSUPPORTED;
            return NULL;
    }
    *result = HIR_BUILD_OK;
    return expression;
}

static MilenaHIRStatement *hir_build_statement(const ASTNode *node,
                                               HIRBuildResult *result);

static bool hir_build_statement_array(const ASTNode *const *nodes, size_t count,
                                      MilenaHIRStatement ***output,
                                      HIRBuildResult *result) {
    *output = NULL;
    if (!count) return true;
    if (count > SIZE_MAX / sizeof(**output)) {
        *result = HIR_BUILD_MEMORY;
        return false;
    }
    MilenaHIRStatement **items = calloc(count, sizeof(*items));
    if (!items) {
        *result = HIR_BUILD_MEMORY;
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        items[i] = hir_build_statement(nodes[i], result);
        if (*result != HIR_BUILD_OK) {
            hir_statement_array_release(items, count);
            return false;
        }
    }
    *output = items;
    *result = HIR_BUILD_OK;
    return true;
}

static MilenaHIRStatement *hir_build_statement(const ASTNode *node,
                                               HIRBuildResult *result) {
    if (!node || !result) {
        if (result) *result = HIR_BUILD_UNSUPPORTED;
        return NULL;
    }
    MilenaHIRStatement *statement = calloc(1, sizeof(*statement));
    if (!statement) {
        *result = HIR_BUILD_MEMORY;
        return NULL;
    }
    hir_source_span(&statement->span, node);
    statement->resolved_symbol_id = node->resolved_symbol_id;
    if (node->value) {
        statement->name = milena_strdup(node->value);
        if (!statement->name) {
            free(statement);
            *result = HIR_BUILD_MEMORY;
            return NULL;
        }
    }
    switch (node->type) {
        case AST_DECLARACION_VARIABLE:
        case AST_ASIGNACION_VARIABLE:
        case AST_COMANDO_RETORNAR:
            if (node->child_count != 1 ||
                !hir_value_type(node->value_type, &statement->value_type)) {
                hir_statement_release(statement);
                *result = HIR_BUILD_UNSUPPORTED;
                return NULL;
            }
            statement->kind = node->type == AST_DECLARACION_VARIABLE ?
                MILENA_HIR_STMT_DECLARE :
                (node->type == AST_ASIGNACION_VARIABLE ?
                 MILENA_HIR_STMT_ASSIGN : MILENA_HIR_STMT_RETURN);
            statement->as.expression = hir_build_expression(
                node->children[0], result);
            if (*result != HIR_BUILD_OK) {
                hir_statement_release(statement);
                return NULL;
            }
            break;
        case AST_CONDICION_SI: {
            if (node->child_count == 0) {
                hir_statement_release(statement);
                *result = HIR_BUILD_UNSUPPORTED;
                return NULL;
            }
            statement->kind = MILENA_HIR_STMT_IF;
            statement->as.conditional.condition = hir_build_expression(
                node->children[0], result);
            if (*result != HIR_BUILD_OK) {
                hir_statement_release(statement);
                return NULL;
            }
            size_t then_count = node->child_count - 1;
            const ASTNode *else_block = NULL;
            if (then_count && node->children[node->child_count - 1]->type ==
                                  AST_BLOQUE_FUNCION) {
                else_block = node->children[node->child_count - 1];
                then_count--;
            }
            statement->as.conditional.then_count = then_count;
            if (!hir_build_statement_array(
                    (const ASTNode *const *)(node->children + 1), then_count,
                    &statement->as.conditional.then_body, result)) {
                hir_statement_release(statement);
                return NULL;
            }
            if (else_block) {
                statement->as.conditional.else_count = else_block->child_count;
                if (!hir_build_statement_array(
                        (const ASTNode *const *)else_block->children,
                        else_block->child_count,
                        &statement->as.conditional.else_body, result)) {
                    hir_statement_release(statement);
                    return NULL;
                }
            }
            break;
        }
        default:
            hir_statement_release(statement);
            *result = HIR_BUILD_UNSUPPORTED;
            return NULL;
    }
    *result = HIR_BUILD_OK;
    return statement;
}

static HIRBuildResult scalar_hir_build(const ASTNode *ast,
                                       MilenaScalarHIR **output) {
    *output = NULL;
    if (!ast || ast->type != AST_PROGRAMA) return HIR_BUILD_UNSUPPORTED;
    size_t function_count = 0, statement_count = 0;
    for (size_t i = 0; i < ast->child_count; ++i) {
        if (ast->children[i]->type == AST_DECLARACION_FUNCION) function_count++;
        else statement_count++;
    }
    MilenaScalarHIR *hir = calloc(1, sizeof(*hir));
    if (!hir) return HIR_BUILD_MEMORY;
    if (function_count) {
        hir->functions = calloc(function_count, sizeof(*hir->functions));
        if (!hir->functions) {
            scalar_hir_release(hir);
            return HIR_BUILD_MEMORY;
        }
    }
    hir->function_count = function_count;
    if (statement_count) {
        hir->statements = calloc(statement_count, sizeof(*hir->statements));
        if (!hir->statements) {
            scalar_hir_release(hir);
            return HIR_BUILD_MEMORY;
        }
    }
    hir->statement_count = statement_count;
    size_t function_index = 0, statement_index = 0;
    HIRBuildResult result = HIR_BUILD_OK;
    for (size_t i = 0; i < ast->child_count && result == HIR_BUILD_OK; ++i) {
        const ASTNode *node = ast->children[i];
        if (node->type != AST_DECLARACION_FUNCION) {
            hir->statements[statement_index] = hir_build_statement(node, &result);
            if (result == HIR_BUILD_OK) statement_index++;
            continue;
        }
        if (!node->value || node->child_count != 2 || !node->children[0] ||
            !node->children[1] || node->resolved_symbol_id == 0) {
            result = HIR_BUILD_UNSUPPORTED;
            break;
        }
        MilenaHIRFunction *function = &hir->functions[function_index];
        function->name = milena_strdup(node->value);
        if (!function->name) {
            result = HIR_BUILD_MEMORY;
            break;
        }
        function->resolved_symbol_id = node->resolved_symbol_id;
        hir_source_span(&function->span, node);
        const ASTNode *parameters = node->children[0];
        if (parameters->child_count) {
            if (parameters->child_count > SIZE_MAX / sizeof(*function->parameters)) {
                result = HIR_BUILD_MEMORY;
                break;
            }
            function->parameters = calloc(parameters->child_count,
                                           sizeof(*function->parameters));
            if (!function->parameters) {
                result = HIR_BUILD_MEMORY;
                break;
            }
        }
        function->parameter_count = parameters->child_count;
        for (size_t j = 0; j < function->parameter_count; ++j) {
            const ASTNode *parameter = parameters->children[j];
            if (!parameter || !parameter->value ||
                parameter->resolved_symbol_id == 0) {
                result = HIR_BUILD_UNSUPPORTED;
                break;
            }
            function->parameters[j].name = milena_strdup(parameter->value);
            function->parameters[j].resolved_symbol_id =
                parameter->resolved_symbol_id;
            if (!function->parameters[j].name) {
                result = HIR_BUILD_MEMORY;
                break;
            }
        }
        if (result != HIR_BUILD_OK) break;
        const ASTNode *body = node->children[1];
        function->body_count = body->child_count;
        if (!hir_build_statement_array(
                (const ASTNode *const *)body->children, body->child_count,
                &function->body, &result)) break;
        function_index++;
    }
    if (result != HIR_BUILD_OK) {
        scalar_hir_release(hir);
        return result;
    }
    *output = hir;
    return HIR_BUILD_OK;
}

void milena_canonical_program_init(MilenaCanonicalProgram *program) {
    if (!program) return;
    program->ast = NULL;
    program->table = NULL;
    program->hir = NULL;
}

void milena_canonical_program_release(MilenaCanonicalProgram *program) {
    if (!program) return;
    scalar_hir_release(program->hir);
    program->hir = NULL;
    ast_destroy(program->ast);
    program->ast = NULL;
    program->table = NULL;
}

MilenaStatus milena_canonical_program_parse(MilenaCanonicalProgram *program,
                                             const char *source,
                                             MilenaError *error) {
    if (!program || !source) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "El programa canónico y su fuente son obligatorios");
        return MILENA_ERR_ARGUMENT;
    }
    milena_canonical_program_release(program);
    if (error) milena_error_clear(error);

    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *ast = parser_parse(&parser);
    if (!ast || parser.has_error) {
        if (error) {
            if (parser.error.code != MILENA_OK) *error = parser.error;
            else canonical_error(error, MILENA_ERR_PARSE,
                                 "La fuente no produjo un AST canónico válido");
        }
        ast_destroy(ast);
        parser_release(&parser);
        return error && error->code != MILENA_OK ? error->code : MILENA_ERR_PARSE;
    }
    parser_release(&parser);

    MilenaStatus status = milena_validate_ast(ast, error);
    if (status != MILENA_OK) {
        ast_destroy(ast);
        return status;
    }
    MilenaScalarHIR *hir = NULL;
    HIRBuildResult hir_result = scalar_hir_build(ast, &hir);
    if (hir_result == HIR_BUILD_MEMORY) {
        ast_destroy(ast);
        canonical_error(error, MILENA_ERR_MEMORY,
                        "Sin memoria para construir la HIR escalar canónica");
        return MILENA_ERR_MEMORY;
    }
    /* Unsupported legacy/data-operation ASTs remain intact and have no HIR. */
    program->ast = ast;
    program->hir = hir;
    return MILENA_OK;
}

MilenaStatus milena_canonical_program_bind_table(MilenaCanonicalProgram *program,
                                                 const MilenaTable *table,
                                                 MilenaError *error) {
    if (!program || !program->ast || !table) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La frontera canónica requiere AST y MilenaTable");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;

    const ASTNode *analysis = NULL;
    for (size_t i = 0; i < program->ast->child_count; i++) {
        const ASTNode *child = program->ast->children[i];
        if (child && child->type == AST_BLOQUE_ANALISIS) {
            analysis = child;
            break;
        }
    }
    if (analysis) {
        status = milena_validate_sst_table(analysis, table, error);
        if (status != MILENA_OK) return status;
    }
    program->table = table;
    return MILENA_OK;
}

MilenaStatus milena_canonical_compiler_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error) {
    if (!program || !program->ast || !input) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La entrada del compilador canónico es inválida");
        return MILENA_ERR_ARGUMENT;
    }
    input->ast = program->ast;
    input->table = program->table;
    input->hir = program->hir;
    return MILENA_OK;
}
