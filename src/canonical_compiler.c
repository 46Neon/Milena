#include "canonical_compiler.h"

#include "language_semantic.h"
#include "lexer.h"
#include "parser.h"

#include <stdio.h>
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

static void hir_column_ref_release(MilenaHIRColumnRef *column) {
    if (!column) return;
    free(column->name);
    memset(column, 0, sizeof(*column));
    column->resolved_column_index = SIZE_MAX;
}

static void hir_aggregate_array_release(MilenaHIRAggregate *aggregates,
                                        size_t count) {
    if (!aggregates) return;
    for (size_t i = 0; i < count; ++i) {
        hir_column_ref_release(&aggregates[i].input);
        free(aggregates[i].output_name);
    }
    free(aggregates);
}

static void data_hir_release(MilenaDataHIR *hir) {
    if (!hir) return;
    free(hir->source.path);
    free(hir->export_path);
    for (size_t i = 0; i < hir->declared_column_count; ++i)
        hir_column_ref_release(&hir->declared_schema[i]);
    free(hir->declared_schema);
    for (size_t i = 0; i < hir->operation_count; ++i) {
        MilenaHIRDataOperation *op = &hir->operations[i];
        switch (op->kind) {
            case MILENA_HIR_DATA_PRODUCT:
                hir_column_ref_release(&op->as.product.left);
                hir_column_ref_release(&op->as.product.right);
                free(op->as.product.output_name);
                break;
            case MILENA_HIR_DATA_FILTER_NUMERIC:
                hir_column_ref_release(&op->as.filter.column);
                break;
            case MILENA_HIR_DATA_SELECT_COLUMNS:
                for (size_t j = 0; j < op->as.select.count; ++j)
                    hir_column_ref_release(&op->as.select.columns[j]);
                free(op->as.select.columns);
                break;
            case MILENA_HIR_DATA_GROUP:
                hir_column_ref_release(&op->as.group.key);
                hir_aggregate_array_release(op->as.group.aggregates,
                                            op->as.group.aggregate_count);
                break;
            case MILENA_HIR_DATA_SUMMARIZE:
                hir_aggregate_array_release(op->as.summarize.aggregates,
                                            op->as.summarize.aggregate_count);
                break;
            case MILENA_HIR_DATA_DROP_NULLS:
            case MILENA_HIR_DATA_DROP_DUPLICATES:
                break;
            case MILENA_HIR_DATA_JOIN:
                free(op->as.join.right_source);
                hir_column_ref_release(&op->as.join.left_key);
                hir_column_ref_release(&op->as.join.right_key);
                break;
            case MILENA_HIR_DATA_SST:
                free(op->as.sst.name);
                for (size_t j = 0; j < op->as.sst.column_count; ++j)
                    hir_column_ref_release(&op->as.sst.columns[j]);
                free(op->as.sst.columns);
                break;
            case MILENA_HIR_DATA_EXPORT:
                free(op->as.export_result.path);
                break;
        }
    }
    free(hir->operations);
    free(hir);
}

static MilenaHIRColumnRef hir_unresolved_column(const char *name,
                                                 const ASTNode *node) {
    MilenaHIRColumnRef ref = {0};
    ref.name = name ? milena_strdup(name) : NULL;
    ref.resolved_column_index = SIZE_MAX;
    ref.type = MILENA_HIR_COLUMN_UNKNOWN;
    hir_source_span(&ref.span, node);
    return ref;
}

static bool hir_append_data_operation(MilenaDataHIR *hir,
                                     MilenaHIRDataOperation *operation) {
    if (!hir || !operation || hir->operation_count >= SIZE_MAX / sizeof(*hir->operations))
        return false;
    size_t count = hir->operation_count + 1;
    MilenaHIRDataOperation *grown = (MilenaHIRDataOperation *)realloc(
        hir->operations, count * sizeof(*grown));
    if (!grown) return false;
    hir->operations = grown;
    hir->operations[hir->operation_count++] = *operation;
    memset(operation, 0, sizeof(*operation));
    return true;
}

static bool hir_parse_product(const char *text, char left[128], char right[128]) {
    char extra;
    return text && sscanf(text, " %127s * %127s %c", left, right, &extra) == 2;
}

static char *hir_trim(char *text) {
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = '\0';
    return text;
}

static bool hir_parse_selection(const char *text, MilenaHIRDataOperation *op,
                                const ASTNode *span_node) {
    if (!text || !op) return false;
    char *copy = milena_strdup(text);
    if (!copy) return false;
    size_t count = 1;
    for (const char *p = text; *p; ++p) if (*p == ',') count++;
    if (!count || count > 32 || count > SIZE_MAX / sizeof(*op->as.select.columns)) {
        free(copy);
        return false;
    }
    op->as.select.columns = (MilenaHIRColumnRef *)calloc(
        count, sizeof(*op->as.select.columns));
    if (!op->as.select.columns) { free(copy); return false; }
    char *cursor = copy;
    for (size_t i = 0; i < count; ++i) {
        char *comma = strchr(cursor, ',');
        if (comma) *comma = '\0';
        char *name = hir_trim(cursor);
        if (!name || !*name || strlen(name) >= 128) { free(copy); return false; }
        for (size_t j = 0; j < i; ++j)
            if (strcmp(op->as.select.columns[j].name, name) == 0) {
                free(copy);
                return false;
            }
        op->as.select.columns[i] = hir_unresolved_column(name, span_node);
        if (!op->as.select.columns[i].name) { free(copy); return false; }
        op->as.select.count++;
        if (i + 1 < count && !comma) { free(copy); return false; }
        if (i + 1 == count && comma) { free(copy); return false; }
        cursor = comma ? comma + 1 : cursor + strlen(cursor);
    }
    free(copy);
    return true;
}

static const char *hir_aggregate_legacy_name(ASTAggregateOperation operation) {
    switch (operation) {
        case AST_AGGREGATE_OPERATION_SUM: return "suma";
        case AST_AGGREGATE_OPERATION_MEAN: return "media";
        case AST_AGGREGATE_OPERATION_MIN: return "minimo";
        case AST_AGGREGATE_OPERATION_MAX: return "maximo";
        case AST_AGGREGATE_OPERATION_COUNT: return "conteo";
        case AST_AGGREGATE_OPERATION_VARIANCE: return "varianza";
        case AST_AGGREGATE_OPERATION_STDDEV: return "desviacion_estandar";
        case AST_AGGREGATE_OPERATION_MEDIAN: return "mediana";
        case AST_AGGREGATE_OPERATION_PERCENTILE: return "percentil";
        default: return NULL;
    }
}

static HIRBuildResult hir_parse_aggregate(const ASTNode *node,
                                          MilenaHIRAggregate *aggregate) {
    if (!node || node->type != AST_RESUMEN_METRICA || !aggregate ||
        !node->has_aggregate_metric || !node->aggregate_column ||
        !node->aggregate_column[0] || !node->value)
        return HIR_BUILD_UNSUPPORTED;
    const char *legacy_name =
        hir_aggregate_legacy_name(node->aggregate_operation);
    if (!legacy_name) return HIR_BUILD_UNSUPPORTED;
    size_t legacy_name_length = strlen(legacy_name);
    if (strncmp(node->value, legacy_name, legacy_name_length) != 0 ||
        node->value[legacy_name_length] != ':' ||
        strcmp(node->value + legacy_name_length + 1,
               node->aggregate_column) != 0)
        return HIR_BUILD_UNSUPPORTED;

    const char *suffix = NULL;
    switch (node->aggregate_operation) {
        case AST_AGGREGATE_OPERATION_SUM:
            aggregate->operation = MILENA_AGG_SUM;
            suffix = "suma";
            break;
        case AST_AGGREGATE_OPERATION_MEAN:
            aggregate->operation = MILENA_AGG_MEAN;
            suffix = "media";
            break;
        case AST_AGGREGATE_OPERATION_MIN:
            aggregate->operation = MILENA_AGG_MIN;
            suffix = "minimo";
            break;
        case AST_AGGREGATE_OPERATION_MAX:
            aggregate->operation = MILENA_AGG_MAX;
            suffix = "maximo";
            break;
        case AST_AGGREGATE_OPERATION_COUNT:
            aggregate->operation = MILENA_AGG_COUNT;
            suffix = "conteo";
            break;
        default:
            return HIR_BUILD_UNSUPPORTED;
    }
    aggregate->input = hir_unresolved_column(node->aggregate_column, node);
    if (!aggregate->input.name) return HIR_BUILD_MEMORY;
    hir_source_span(&aggregate->span, node);
    size_t column_len = strlen(node->aggregate_column), suffix_len = strlen(suffix);
    if (column_len > SIZE_MAX - suffix_len - 2) {
        hir_column_ref_release(&aggregate->input);
        return HIR_BUILD_MEMORY;
    }
    size_t name_len = column_len + suffix_len + 2;
    aggregate->output_name = (char *)malloc(name_len);
    if (!aggregate->output_name) {
        hir_column_ref_release(&aggregate->input);
        return HIR_BUILD_MEMORY;
    }
    (void)snprintf(aggregate->output_name, name_len, "%s_%s",
                   node->aggregate_column, suffix);
    return HIR_BUILD_OK;
}

static bool hir_append_declared_column(MilenaDataHIR *hir,
                                       MilenaHIRColumnRef *column) {
    if (!hir || !column || hir->declared_column_count >=
        SIZE_MAX / sizeof(*hir->declared_schema)) return false;
    size_t count = hir->declared_column_count + 1;
    MilenaHIRColumnRef *grown = (MilenaHIRColumnRef *)realloc(
        hir->declared_schema, count * sizeof(*grown));
    if (!grown) return false;
    hir->declared_schema = grown;
    hir->declared_schema[hir->declared_column_count++] = *column;
    memset(column, 0, sizeof(*column));
    return true;
}

static HIRBuildResult data_hir_build(const ASTNode *ast, MilenaDataHIR **output) {
    *output = NULL;
    if (!ast || ast->type != AST_PROGRAMA || ast->child_count != 1 ||
        !ast->children[0] || ast->children[0]->type != AST_BLOQUE_ANALISIS)
        return HIR_BUILD_UNSUPPORTED;
    const ASTNode *analysis = ast->children[0];
    MilenaDataHIR *hir = (MilenaDataHIR *)calloc(1, sizeof(*hir));
    if (!hir) return HIR_BUILD_MEMORY;
    hir_source_span(&hir->source.span, analysis);
    hir->resource_policy.max_input_rows = SIZE_MAX;
    hir->resource_policy.max_output_rows = SIZE_MAX;
    hir->resource_policy.max_columns = SIZE_MAX;
    const ASTNode *load = NULL;
    bool terminal_operation_seen = false;
    bool export_seen = false;
    for (size_t i = 0; i < analysis->child_count; ++i) {
        const ASTNode *node = analysis->children[i];
        if (!node || export_seen ||
            (terminal_operation_seen && node->type != AST_BLOQUE_EXPORTAR)) {
            data_hir_release(hir);
            return HIR_BUILD_UNSUPPORTED;
        }
        if (node->type == AST_LLAMADA_CARGAR) {
            if (load || !node->value || (node->type_name &&
                strcmp(node->type_name, "flujo") == 0)) {
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            load = node;
            hir->source.path = milena_strdup(node->value);
            hir->source.resolved_dataset_id = 1;
            hir->source.streaming = false;
            hir->source.chunk_rows = 0;
            if (node->has_source_span) hir_source_span(&hir->source.span, node);
            if (!hir->source.path) { data_hir_release(hir); return HIR_BUILD_MEMORY; }
            continue;
        }
        if (node->type == AST_DECLARACION_VARIABLE) {
            if (!node->value || !node->type_name || node->child_count != 0) {
                data_hir_release(hir); return HIR_BUILD_UNSUPPORTED;
            }
            MilenaHIRColumnRef declared = hir_unresolved_column(node->value, node);
            if (strcmp(node->type_name, "numerica") == 0)
                declared.declared_type = MILENA_HIR_COLUMN_NUMERIC;
            else if (strcmp(node->type_name, "binaria") == 0 ||
                     strcmp(node->type_name, "categorica") == 0)
                declared.declared_type = MILENA_HIR_COLUMN_CATEGORICAL;
            else if (strcmp(node->type_name, "texto") == 0 ||
                     strcmp(node->type_name, "fecha") == 0)
                declared.declared_type = MILENA_HIR_COLUMN_TEXT;
            else {
                hir_column_ref_release(&declared);
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            if (!declared.name) {
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            bool duplicate = false;
            for (size_t j = 0; j < hir->declared_column_count; ++j)
                duplicate |= strcmp(hir->declared_schema[j].name,
                                    declared.name) == 0;
            if (duplicate || !hir_append_declared_column(hir, &declared)) {
                hir_column_ref_release(&declared);
                data_hir_release(hir);
                return duplicate ? HIR_BUILD_UNSUPPORTED : HIR_BUILD_MEMORY;
            }
            continue;
        }
        if (node->type == AST_BLOQUE_TRANSFORMAR) {
            for (size_t j = 0; j < node->child_count; ++j) {
                const ASTNode *command = node->children[j];
                if (!command || command->type != AST_COMANDO_TOTAL || !command->value) {
                    data_hir_release(hir); return HIR_BUILD_UNSUPPORTED;
                }
                char left[128] = {0}, right[128] = {0};
                if (!hir_parse_product(command->value, left, right)) {
                    data_hir_release(hir); return HIR_BUILD_UNSUPPORTED;
                }
                MilenaHIRDataOperation op = {0};
                op.kind = MILENA_HIR_DATA_PRODUCT;
                hir_source_span(&op.span, command->has_source_span ? command : node);
                op.as.product.left = hir_unresolved_column(left, command);
                op.as.product.right = hir_unresolved_column(right, command);
                op.as.product.output_name = milena_strdup("total");
                if (!op.as.product.left.name || !op.as.product.right.name ||
                    !op.as.product.output_name || !hir_append_data_operation(hir, &op)) {
                    hir_column_ref_release(&op.as.product.left);
                    hir_column_ref_release(&op.as.product.right);
                    free(op.as.product.output_name);
                    data_hir_release(hir);
                    return HIR_BUILD_MEMORY;
                }
            }
            continue;
        }
        if (node->type == AST_BLOQUE_FILTRAR) {
            if (node->child_count != 1 || !node->children[0] ||
                node->children[0]->type != AST_COMANDO_CONDICION ||
                !node->children[0]->has_filter_predicate ||
                !node->children[0]->filter_column) {
                data_hir_release(hir); return HIR_BUILD_UNSUPPORTED;
            }
            const ASTNode *condition = node->children[0];
            MilenaHIRDataOperation op = {0};
            op.kind = MILENA_HIR_DATA_FILTER_NUMERIC;
            op.as.filter.operation = condition->filter_operator;
            op.as.filter.threshold = condition->filter_threshold;
            hir_source_span(&op.span, condition->has_source_span ? condition : node);
            op.as.filter.column = hir_unresolved_column(condition->filter_column, condition);
            if (!op.as.filter.column.name || !hir_append_data_operation(hir, &op)) {
                hir_column_ref_release(&op.as.filter.column);
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            continue;
        }
        if (node->type == AST_BLOQUE_AGRUPAR) {
            MilenaHIRDataOperation op = {0};
            op.kind = MILENA_HIR_DATA_GROUP;
            hir_source_span(&op.span, node);
            if ((node->type_name && strcmp(node->type_name, "flujo") == 0) ||
                node->child_count == 0 || node->child_count > 17) {
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            op.as.group.aggregates = (MilenaHIRAggregate *)calloc(
                node->child_count, sizeof(*op.as.group.aggregates));
            if (!op.as.group.aggregates) {
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            const ASTNode *key_node = NULL;
            HIRBuildResult aggregate_result = HIR_BUILD_OK;
            for (size_t j = 0; j < node->child_count; ++j) {
                const ASTNode *child = node->children[j];
                if (!child) {
                    aggregate_result = HIR_BUILD_UNSUPPORTED;
                    break;
                }
                if (child->type == AST_AGRUPACION_POR && child->value && !key_node) {
                    key_node = child;
                } else if (child->type == AST_RESUMEN_METRICA &&
                           op.as.group.aggregate_count < 16) {
                    aggregate_result = hir_parse_aggregate(
                        child, &op.as.group.aggregates[op.as.group.aggregate_count]);
                    if (aggregate_result != HIR_BUILD_OK) break;
                    op.as.group.aggregate_count++;
                } else {
                    aggregate_result = HIR_BUILD_UNSUPPORTED;
                    break;
                }
            }
            if (aggregate_result != HIR_BUILD_OK || !key_node ||
                op.as.group.aggregate_count == 0) {
                hir_column_ref_release(&op.as.group.key);
                hir_aggregate_array_release(op.as.group.aggregates,
                                            op.as.group.aggregate_count);
                data_hir_release(hir);
                return aggregate_result == HIR_BUILD_OK ? HIR_BUILD_UNSUPPORTED :
                                                         aggregate_result;
            }
            op.as.group.key = hir_unresolved_column(key_node->value, key_node);
            if (!op.as.group.key.name) {
                hir_aggregate_array_release(op.as.group.aggregates,
                                            op.as.group.aggregate_count);
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            if (!hir_append_data_operation(hir, &op)) {
                hir_column_ref_release(&op.as.group.key);
                hir_aggregate_array_release(op.as.group.aggregates,
                                            op.as.group.aggregate_count);
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            terminal_operation_seen = true;
            continue;
        }
        if (node->type == AST_BLOQUE_RESUMIR) {
            MilenaHIRDataOperation op = {0};
            op.kind = MILENA_HIR_DATA_SUMMARIZE;
            hir_source_span(&op.span, node);
            if (node->child_count == 0 || node->child_count > 16) {
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            op.as.summarize.aggregates = (MilenaHIRAggregate *)calloc(
                node->child_count, sizeof(*op.as.summarize.aggregates));
            if (!op.as.summarize.aggregates) {
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            HIRBuildResult aggregate_result = HIR_BUILD_OK;
            for (size_t j = 0; j < node->child_count; ++j) {
                aggregate_result = hir_parse_aggregate(
                    node->children[j], &op.as.summarize.aggregates[j]);
                if (aggregate_result != HIR_BUILD_OK) break;
                op.as.summarize.aggregate_count++;
            }
            if (aggregate_result != HIR_BUILD_OK) {
                hir_aggregate_array_release(op.as.summarize.aggregates,
                                            op.as.summarize.aggregate_count);
                data_hir_release(hir);
                return aggregate_result;
            }
            if (!hir_append_data_operation(hir, &op)) {
                hir_aggregate_array_release(op.as.summarize.aggregates,
                                            op.as.summarize.aggregate_count);
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            terminal_operation_seen = true;
            continue;
        }
        if (node->type == AST_BLOQUE_LIMPIAR) {
            if (node->child_count == 0) {
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            for (size_t j = 0; j < node->child_count; ++j) {
                const ASTNode *command = node->children[j];
                MilenaHIRDataOperation op = {0};
                if (!command || !command->value ||
                    strcmp(command->value, "eliminar") != 0) {
                    data_hir_release(hir);
                    return HIR_BUILD_UNSUPPORTED;
                }
                if (command->type == AST_COMANDO_NULOS)
                    op.kind = MILENA_HIR_DATA_DROP_NULLS;
                else if (command->type == AST_COMANDO_DUPLICADOS)
                    op.kind = MILENA_HIR_DATA_DROP_DUPLICATES;
                else {
                    data_hir_release(hir);
                    return HIR_BUILD_UNSUPPORTED;
                }
                hir_source_span(&op.span, command->has_source_span ? command : node);
                if (!hir_append_data_operation(hir, &op)) {
                    data_hir_release(hir);
                    return HIR_BUILD_MEMORY;
                }
            }
            continue;
        }
        if (node->type == AST_BLOQUE_UNIR) {
            const ASTNode *right_node = NULL;
            const ASTNode *key_node = NULL;
            for (size_t j = 0; j < node->child_count; ++j) {
                const ASTNode *child = node->children[j];
                if (!child) { data_hir_release(hir); return HIR_BUILD_UNSUPPORTED; }
                if (child->type == AST_COMANDO_DERECHA && child->value && !right_node)
                    right_node = child;
                else if (child->type == AST_COMANDO_CLAVE && child->value && !key_node)
                    key_node = child;
                else {
                    data_hir_release(hir);
                    return HIR_BUILD_UNSUPPORTED;
                }
            }
            if (!right_node || !key_node || !node->join_memory_budget_bytes ||
                !node->join_max_output_rows) {
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            MilenaHIRDataOperation op = {0};
            op.kind = MILENA_HIR_DATA_JOIN;
            hir_source_span(&op.span, node);
            op.as.join.right_source = milena_strdup(right_node->value);
            op.as.join.right_dataset_id = 2;
            op.as.join.left_key = hir_unresolved_column(key_node->value, key_node);
            op.as.join.right_key = hir_unresolved_column(key_node->value, key_node);
            op.as.join.join_type = MILENA_JOIN_INNER;
            op.as.join.policy.max_input_rows = SIZE_MAX;
            op.as.join.policy.max_output_rows = node->join_max_output_rows;
            op.as.join.policy.max_columns = SIZE_MAX;
            op.as.join.memory_budget_bytes = node->join_memory_budget_bytes;
            if (!op.as.join.right_source || !op.as.join.left_key.name ||
                !op.as.join.right_key.name ||
                !hir_append_data_operation(hir, &op)) {
                free(op.as.join.right_source);
                hir_column_ref_release(&op.as.join.left_key);
                hir_column_ref_release(&op.as.join.right_key);
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            terminal_operation_seen = true;
            continue;
        }
        if (node->type == AST_BLOQUE_SELECCIONAR) {
            if (node->child_count != 1 || !node->children[0] ||
                node->children[0]->type != AST_COMANDO_COLUMNAS ||
                !node->children[0]->value) {
                data_hir_release(hir); return HIR_BUILD_UNSUPPORTED;
            }
            MilenaHIRDataOperation op = {0};
            op.kind = MILENA_HIR_DATA_SELECT_COLUMNS;
            hir_source_span(&op.span, node->children[0]->has_source_span ?
                            node->children[0] : node);
            if (!hir_parse_selection(node->children[0]->value, &op,
                                     node->children[0])) {
                for (size_t j = 0; j < op.as.select.count; ++j)
                    hir_column_ref_release(&op.as.select.columns[j]);
                free(op.as.select.columns);
                data_hir_release(hir);
                return HIR_BUILD_UNSUPPORTED;
            }
            if (!hir_append_data_operation(hir, &op)) {
                for (size_t j = 0; j < op.as.select.count; ++j)
                    hir_column_ref_release(&op.as.select.columns[j]);
                free(op.as.select.columns);
                data_hir_release(hir);
                return HIR_BUILD_MEMORY;
            }
            terminal_operation_seen = true;
            continue;
        }
        if (node->type == AST_BLOQUE_EXPORTAR && node->value) {
            if (hir->export_path) { data_hir_release(hir); return HIR_BUILD_UNSUPPORTED; }
            hir->export_path = milena_strdup(node->value);
            if (!hir->export_path) { data_hir_release(hir); return HIR_BUILD_MEMORY; }
            export_seen = true;
            continue;
        }
        data_hir_release(hir);
        return HIR_BUILD_UNSUPPORTED;
    }
    if (!load) { data_hir_release(hir); return HIR_BUILD_UNSUPPORTED; }
    /* This closed HIR revision declares one dataset symbol per program. Every
     * operation is explicitly resolved to that source after the full AST scan. */
    for (size_t i = 0; i < hir->operation_count; ++i) {
        if (hir->operations[i].resolved_dataset_id != 0 &&
            hir->operations[i].resolved_dataset_id != hir->source.resolved_dataset_id) {
            data_hir_release(hir);
            return HIR_BUILD_UNSUPPORTED;
        }
        hir->operations[i].resolved_dataset_id = hir->source.resolved_dataset_id;
    }
    *output = hir;
    return HIR_BUILD_OK;
}

void milena_canonical_program_init(MilenaCanonicalProgram *program) {
    if (!program) return;
    program->ast = NULL;
    program->table = NULL;
    program->right_table = NULL;
    program->hir = NULL;
    program->data_hir = NULL;
}

void milena_canonical_program_release(MilenaCanonicalProgram *program) {
    if (!program) return;
    scalar_hir_release(program->hir);
    program->hir = NULL;
    data_hir_release(program->data_hir);
    program->data_hir = NULL;
    ast_destroy(program->ast);
    program->ast = NULL;
    program->table = NULL;
    program->right_table = NULL;
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
    MilenaDataHIR *data_hir = NULL;
    HIRBuildResult data_result = data_hir_build(ast, &data_hir);
    if (data_result == HIR_BUILD_MEMORY) {
        scalar_hir_release(hir);
        ast_destroy(ast);
        canonical_error(error, MILENA_ERR_MEMORY,
                        "Sin memoria para construir la HIR de datos canónica");
        return MILENA_ERR_MEMORY;
    }
    /* ASTs outside both closed HIR subsets remain compatibility-only. */
    program->ast = ast;
    program->hir = hir;
    program->data_hir = data_hir;
    return MILENA_OK;
}

static void canonical_span_error(MilenaError *error, MilenaStatus code,
                                 const MilenaHIRSourceSpan *span,
                                 const char *message) {
    if (!error) return;
    milena_error_set(error, code, span && span->has_source_span ? span->line : 0,
                     span && span->has_source_span ? span->column : 0, 0, message);
}

static bool hir_numeric_dtype(MilenaDType dtype) {
    return dtype >= MILENA_DTYPE_INT8 && dtype <= MILENA_DTYPE_FLOAT64;
}

static bool hir_previous_product(const MilenaDataHIR *hir, size_t before,
                                 const char *name) {
    if (!hir || !name) return false;
    if (before > hir->operation_count) before = hir->operation_count;
    for (size_t i = 0; i < before; ++i) {
        const MilenaHIRDataOperation *op = &hir->operations[i];
        if (op->kind == MILENA_HIR_DATA_PRODUCT && op->as.product.output_name &&
            strcmp(op->as.product.output_name, name) == 0) return true;
    }
    return false;
}

static MilenaStatus hir_bind_column(const MilenaTable *table,
                                    const MilenaDataHIR *hir,
                                    MilenaHIRColumnRef *ref,
                                    size_t before_operation,
                                    bool require_numeric,
                                    MilenaError *error) {
    if (!table || !hir || !ref || !ref->name) return MILENA_ERR_ARGUMENT;
    int index = milena_table_column_index(table, ref->name);
    const MilenaTableColumn *column = index >= 0
        ? milena_table_column(table, (size_t)index) : NULL;
    if (!column && hir_previous_product(hir, before_operation, ref->name)) {
        size_t virtual_index = table->column_count;
        for (size_t i = 0; i < before_operation; ++i) {
            const MilenaHIRDataOperation *prior = &hir->operations[i];
            if (prior->kind != MILENA_HIR_DATA_PRODUCT) continue;
            if (strcmp(prior->as.product.output_name, ref->name) == 0) break;
            virtual_index++;
        }
        ref->resolved_column_index = virtual_index;
        ref->type = MILENA_HIR_COLUMN_NUMERIC;
        ref->dtype = MILENA_DTYPE_FLOAT64;
        ref->nullable = true;
        ref->rank = 1;
        ref->shape[0] = table->row_count;
        return MILENA_OK;
    }
    if (!column) {
        canonical_span_error(error, MILENA_ERR_TYPE, &ref->span,
                             "Columna no declarada en la fuente de datos ligada");
        return MILENA_ERR_TYPE;
    }
    ref->resolved_column_index = (size_t)index;
    ref->nullable = column->nullable;
    ref->rank = 1;
    ref->shape[0] = table->row_count;
    if (column->type == MILENA_COLUMN_ARRAY) {
        ref->dtype = column->values.dtype;
        if (column->values.dtype == MILENA_DTYPE_BOOL)
            ref->type = MILENA_HIR_COLUMN_BOOLEAN;
        else if (hir_numeric_dtype(column->values.dtype))
            ref->type = MILENA_HIR_COLUMN_NUMERIC;
        else
            ref->type = MILENA_HIR_COLUMN_UNKNOWN;
    } else if (column->type == MILENA_COLUMN_STRING) {
        ref->type = MILENA_HIR_COLUMN_TEXT;
        ref->dtype = MILENA_DTYPE_UINT8;
    } else {
        ref->type = MILENA_HIR_COLUMN_CATEGORICAL;
        ref->dtype = MILENA_DTYPE_UINT32;
    }
    if (require_numeric && ref->type != MILENA_HIR_COLUMN_NUMERIC) {
        canonical_span_error(error, MILENA_ERR_TYPE, &ref->span,
                             "La operación requiere una columna numérica no booleana");
        return MILENA_ERR_TYPE;
    }
    return MILENA_OK;
}

static MilenaStatus data_hir_bind_tables(MilenaDataHIR *hir,
                                         const MilenaTable *table,
                                         const MilenaTable *right_table,
                                         MilenaError *error) {
    if (!hir || !table) return MILENA_ERR_ARGUMENT;
    const char *table_source = milena_table_get_metadata(
        table, MILENA_HIR_DATASET_PATH_METADATA);
    if (!hir->source.path || !table_source ||
        strcmp(table_source, hir->source.path) != 0) {
        canonical_span_error(error, MILENA_ERR_DATA, &hir->source.span,
            "La tabla no acredita la ruta de fuente de datos declarada por HIR");
        return MILENA_ERR_DATA;
    }
    MilenaHIRDataOperation *join = NULL;
    for (size_t i = 0; i < hir->operation_count; ++i) {
        if (hir->operations[i].kind == MILENA_HIR_DATA_JOIN) {
            if (join) {
                canonical_span_error(error, MILENA_ERR_UNSUPPORTED,
                    &hir->operations[i].span,
                    "La HIR admite un solo dataset derecho por programa");
                return MILENA_ERR_UNSUPPORTED;
            }
            join = &hir->operations[i];
        }
    }
    if ((join && !right_table) || (!join && right_table)) {
        canonical_span_error(error, MILENA_ERR_DATA, join ? &join->span :
                             &hir->source.span,
            join ? "La operación join requiere el segundo dataset ligado" :
                   "Se recibió un segundo dataset que la HIR no declaró");
        return MILENA_ERR_DATA;
    }
    if (join) {
        const char *right_source = milena_table_get_metadata(
            right_table, MILENA_HIR_DATASET_PATH_METADATA);
        if (!join->as.join.right_source || !right_source ||
            strcmp(right_source, join->as.join.right_source) != 0 ||
            join->as.join.right_dataset_id == 0 ||
            join->as.join.right_dataset_id == hir->source.resolved_dataset_id) {
            canonical_span_error(error, MILENA_ERR_DATA, &join->span,
                "La tabla derecha no acredita el dataset declarado por la operación join");
            return MILENA_ERR_DATA;
        }
    }
    size_t current_columns = table->column_count;
    size_t maximum_columns = table->column_count;
    size_t maximum_input_rows = table->row_count;
    size_t maximum_output_rows = table->row_count;
    if (right_table) {
        if (right_table->row_count > maximum_input_rows)
            maximum_input_rows = right_table->row_count;
        if (right_table->column_count > maximum_columns)
            maximum_columns = right_table->column_count;
    }
    for (size_t i = 0; i < hir->declared_column_count; ++i) {
        MilenaHIRColumnRef *declared = &hir->declared_schema[i];
        if (!declared->span.has_source_span) declared->span = hir->source.span;
        MilenaStatus status = hir_bind_column(table, hir, declared, 0, false, error);
        if (status != MILENA_OK) return status;
        if (declared->type != declared->declared_type) {
            canonical_span_error(error, MILENA_ERR_TYPE, &declared->span,
                                 "El tipo declarado no coincide con el tipo enlazado de la columna");
            return MILENA_ERR_TYPE;
        }
    }
    for (size_t i = 0; i < hir->operation_count; ++i) {
        MilenaHIRDataOperation *op = &hir->operations[i];
        MilenaStatus status = MILENA_OK;
        if (!op->span.has_source_span) op->span = hir->source.span;
        if (op->resolved_dataset_id != hir->source.resolved_dataset_id) {
            canonical_span_error(error, MILENA_ERR_DATA, &op->span,
                                 "La operación HIR no está ligada a la fuente declarada");
            return MILENA_ERR_DATA;
        }
        if (op->kind == MILENA_HIR_DATA_PRODUCT) {
            if (!op->as.product.left.span.has_source_span)
                op->as.product.left.span = op->span;
            if (!op->as.product.right.span.has_source_span)
                op->as.product.right.span = op->span;
            if (milena_table_column_index(table, op->as.product.output_name) >= 0 ||
                hir_previous_product(hir, i, op->as.product.output_name)) {
                canonical_span_error(error, MILENA_ERR_TYPE, &op->span,
                                     "La columna derivada 'total' ya existe");
                return MILENA_ERR_TYPE;
            }
            status = hir_bind_column(table, hir, &op->as.product.left, i, true, error);
            if (status == MILENA_OK)
                status = hir_bind_column(table, hir, &op->as.product.right, i, true, error);
            if (status != MILENA_OK) return status;
            if (current_columns == SIZE_MAX) return MILENA_ERR_OVERFLOW;
            current_columns++;
            if (current_columns > maximum_columns) maximum_columns = current_columns;
        } else if (op->kind == MILENA_HIR_DATA_FILTER_NUMERIC) {
            if (!op->as.filter.column.span.has_source_span)
                op->as.filter.column.span = op->span;
            status = hir_bind_column(table, hir, &op->as.filter.column, i, true, error);
            if (status != MILENA_OK) return status;
        } else if (op->kind == MILENA_HIR_DATA_SELECT_COLUMNS) {
            for (size_t j = 0; j < op->as.select.count; ++j) {
                if (!op->as.select.columns[j].span.has_source_span)
                    op->as.select.columns[j].span = op->span;
                status = hir_bind_column(table, hir, &op->as.select.columns[j], i,
                                         false, error);
                if (status != MILENA_OK) return status;
            }
            current_columns = op->as.select.count;
        } else if (op->kind == MILENA_HIR_DATA_GROUP) {
            status = hir_bind_column(table, hir, &op->as.group.key, i, false, error);
            if (status != MILENA_OK) return status;
            for (size_t j = 0; j < op->as.group.aggregate_count; ++j) {
                MilenaHIRAggregate *aggregate = &op->as.group.aggregates[j];
                if (!aggregate->input.span.has_source_span)
                    aggregate->input.span = aggregate->span.has_source_span
                        ? aggregate->span : op->span;
                status = hir_bind_column(table, hir, &aggregate->input, i,
                    aggregate->operation != MILENA_AGG_COUNT, error);
                if (status != MILENA_OK) return status;
            }
            if (op->as.group.aggregate_count >= SIZE_MAX) return MILENA_ERR_OVERFLOW;
            current_columns = op->as.group.aggregate_count + 1;
        } else if (op->kind == MILENA_HIR_DATA_SUMMARIZE) {
            for (size_t j = 0; j < op->as.summarize.aggregate_count; ++j) {
                MilenaHIRAggregate *aggregate = &op->as.summarize.aggregates[j];
                if (!aggregate->input.span.has_source_span)
                    aggregate->input.span = aggregate->span.has_source_span
                        ? aggregate->span : op->span;
                status = hir_bind_column(table, hir, &aggregate->input, i,
                    aggregate->operation != MILENA_AGG_COUNT, error);
                if (status != MILENA_OK) return status;
            }
            current_columns = op->as.summarize.aggregate_count;
        } else if (op->kind == MILENA_HIR_DATA_JOIN) {
            if (!right_table || !op->as.join.memory_budget_bytes ||
                !op->as.join.policy.max_output_rows) {
                canonical_span_error(error, MILENA_ERR_DATA, &op->span,
                                     "El join HIR requiere tablas y límites válidos");
                return MILENA_ERR_DATA;
            }
            status = hir_bind_column(table, hir, &op->as.join.left_key, i,
                                     false, error);
            MilenaDataHIR right_scope = {0};
            if (status == MILENA_OK)
                status = hir_bind_column(right_table, &right_scope,
                    &op->as.join.right_key, 0, false, error);
            if (status != MILENA_OK) return status;
            if (op->as.join.left_key.type == MILENA_HIR_COLUMN_UNKNOWN ||
                op->as.join.left_key.type != op->as.join.right_key.type ||
                op->as.join.left_key.dtype != op->as.join.right_key.dtype) {
                canonical_span_error(error, MILENA_ERR_TYPE,
                    &op->as.join.left_key.span,
                    "Las claves de join deben tener tipo y dtype compatibles");
                return MILENA_ERR_TYPE;
            }
            if (current_columns > SIZE_MAX - right_table->column_count)
                return MILENA_ERR_OVERFLOW;
            current_columns += right_table->column_count;
            if (op->as.join.policy.max_output_rows > maximum_output_rows)
                maximum_output_rows = op->as.join.policy.max_output_rows;
        }
        if (current_columns > maximum_columns) maximum_columns = current_columns;
    }
    hir->resource_policy.max_input_rows = maximum_input_rows;
    hir->resource_policy.max_output_rows = maximum_output_rows;
    hir->resource_policy.max_columns = maximum_columns;
    hir->schema_bound = true;
    return MILENA_OK;
}

MilenaStatus milena_canonical_program_bind_tables(
    MilenaCanonicalProgram *program, const MilenaTable *table,
    const MilenaTable *right_table, MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!program || !program->ast || !table) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La frontera canónica requiere AST y MilenaTable");
        return MILENA_ERR_ARGUMENT;
    }
    program->table = NULL;
    program->right_table = NULL;
    if (program->data_hir) program->data_hir->schema_bound = false;
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;
    if (right_table) {
        status = milena_table_validate(right_table, error);
        if (status != MILENA_OK) return status;
    }

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
    if (program->data_hir) {
        status = data_hir_bind_tables(program->data_hir, table, right_table, error);
        if (status != MILENA_OK) return status;
    } else if (right_table) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La entrada HIR no declara un segundo dataset");
        return MILENA_ERR_ARGUMENT;
    }
    program->table = table;
    program->right_table = right_table;
    return MILENA_OK;
}

MilenaStatus milena_canonical_program_bind_table(MilenaCanonicalProgram *program,
                                                 const MilenaTable *table,
                                                 MilenaError *error) {
    return milena_canonical_program_bind_tables(program, table, NULL, error);
}

static const char *hir_operator_text(ASTOperatorKind operation) {
    switch (operation) {
        case AST_OPERATOR_EQUAL: return "==";
        case AST_OPERATOR_NOT_EQUAL: return "!=";
        case AST_OPERATOR_GREATER: return ">";
        case AST_OPERATOR_GREATER_EQUAL: return ">=";
        case AST_OPERATOR_LESS: return "<";
        case AST_OPERATOR_LESS_EQUAL: return "<=";
        default: return NULL;
    }
}

static void hir_attach_error_span(MilenaError *error,
                                 const MilenaHIRSourceSpan *span) {
    if (!error || error->code == MILENA_OK || error->line != 0 ||
        !span || !span->has_source_span) return;
    error->line = span->line;
    error->column = span->column;
}

MilenaStatus milena_canonical_program_execute_data(
    const MilenaCanonicalProgram *program,
    const MilenaHIRResourcePolicy *policy,
    MilenaTable *output,
    MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!program || !program->data_hir || !program->data_hir->schema_bound ||
        !program->table || !output) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La ejecución de datos HIR requiere HIR ligada, tabla y salida inicializada");
        return MILENA_ERR_ARGUMENT;
    }
    if (output == program->table || output == program->right_table) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La salida no puede aliasar ninguna tabla prestada de entrada");
        return MILENA_ERR_ARGUMENT;
    }
    const MilenaHIRResourcePolicy *limits = policy ? policy :
        &program->data_hir->resource_policy;
    const MilenaTable *input = program->table;
    if (input->row_count > limits->max_input_rows ||
        input->column_count > limits->max_columns ||
        (program->right_table &&
         (program->right_table->row_count > limits->max_input_rows ||
          program->right_table->column_count > limits->max_columns))) {
        canonical_error(error, MILENA_ERR_OVERFLOW,
                        "Una tabla ligada excede la política de recursos HIR");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaTable working = {0};
    milena_table_init(&working);
    MilenaStatus status = milena_table_clone(&working, input, error);
    for (size_t i = 0; status == MILENA_OK &&
         i < program->data_hir->operation_count; ++i) {
        const MilenaHIRDataOperation *op = &program->data_hir->operations[i];
        if (op->kind == MILENA_HIR_DATA_PRODUCT) {
            if (working.column_count >= limits->max_columns) {
                canonical_span_error(error, MILENA_ERR_OVERFLOW, &op->span,
                                     "La transformación excede el límite de columnas HIR");
                status = MILENA_ERR_OVERFLOW;
            } else {
                status = milena_table_add_product(&working,
                    op->as.product.left.name, op->as.product.right.name,
                    op->as.product.output_name, error);
            }
        } else if (op->kind == MILENA_HIR_DATA_FILTER_NUMERIC) {
            const char *operator_text = hir_operator_text(op->as.filter.operation);
            MilenaTable filtered = {0};
            milena_table_init(&filtered);
            if (!operator_text) {
                status = MILENA_ERR_UNSUPPORTED;
                canonical_span_error(error, status, &op->span,
                                     "Operador de filtro no representado por HIR");
            } else {
                status = milena_table_filter_numeric(&filtered, &working,
                    op->as.filter.column.name, operator_text,
                    op->as.filter.threshold, error);
            }
            if (status == MILENA_OK) milena_table_swap(&working, &filtered);
            milena_table_destroy(&filtered);
        } else if (op->kind == MILENA_HIR_DATA_SELECT_COLUMNS) {
            size_t count = op->as.select.count;
            const char **names = count ? (const char **)calloc(count, sizeof(*names)) : NULL;
            if (count && !names) {
                status = MILENA_ERR_MEMORY;
                canonical_span_error(error, status, &op->span,
                                     "Sin memoria para proyectar columnas HIR");
            } else {
                for (size_t j = 0; j < count; ++j)
                    names[j] = op->as.select.columns[j].name;
                MilenaTable selected = {0};
                milena_table_init(&selected);
                status = milena_table_select_columns(&selected, &working, names,
                                                      count, error);
                if (status == MILENA_OK) milena_table_swap(&working, &selected);
                milena_table_destroy(&selected);
                free(names);
            }
        } else if (op->kind == MILENA_HIR_DATA_GROUP ||
                   op->kind == MILENA_HIR_DATA_SUMMARIZE) {
            size_t aggregate_count = op->kind == MILENA_HIR_DATA_GROUP
                ? op->as.group.aggregate_count : op->as.summarize.aggregate_count;
            MilenaHIRAggregate *aggregates = op->kind == MILENA_HIR_DATA_GROUP
                ? op->as.group.aggregates : op->as.summarize.aggregates;
            MilenaAggregateSpec *specifications = aggregate_count
                ? (MilenaAggregateSpec *)calloc(aggregate_count,
                                                 sizeof(*specifications)) : NULL;
            if (!aggregate_count || !specifications) {
                status = aggregate_count ? MILENA_ERR_MEMORY : MILENA_ERR_ARGUMENT;
                canonical_span_error(error, status, &op->span,
                                     "No se pudieron preparar agregados HIR");
            } else {
                for (size_t j = 0; j < aggregate_count; ++j) {
                    specifications[j].value_column = aggregates[j].input.name;
                    specifications[j].operation = aggregates[j].operation;
                    specifications[j].output_name = aggregates[j].output_name;
                }
                MilenaTable aggregated = {0};
                milena_table_init(&aggregated);
                if (op->kind == MILENA_HIR_DATA_GROUP) {
                    const char *keys[1] = {op->as.group.key.name};
                    status = milena_table_group_by(&aggregated, &working, keys, 1,
                                                   specifications, aggregate_count,
                                                   error);
                } else {
                    status = milena_table_summarize(&aggregated, &working,
                                                    specifications,
                                                    aggregate_count, error);
                }
                if (status == MILENA_OK)
                    milena_table_swap(&working, &aggregated);
                else
                    hir_attach_error_span(error, &op->span);
                milena_table_destroy(&aggregated);
                free(specifications);
            }
        } else if (op->kind == MILENA_HIR_DATA_DROP_NULLS ||
                   op->kind == MILENA_HIR_DATA_DROP_DUPLICATES) {
            MilenaTable cleaned = {0};
            milena_table_init(&cleaned);
            if (op->kind == MILENA_HIR_DATA_DROP_NULLS)
                status = milena_table_drop_null(&cleaned, &working, error);
            else
                status = milena_table_drop_duplicates(&cleaned, &working, error);
            if (status == MILENA_OK) milena_table_swap(&working, &cleaned);
            else hir_attach_error_span(error, &op->span);
            milena_table_destroy(&cleaned);
        } else if (op->kind == MILENA_HIR_DATA_JOIN) {
            if (!program->right_table || !op->as.join.right_source) {
                status = MILENA_ERR_DATA;
                canonical_span_error(error, status, &op->span,
                                     "El join HIR requiere el dataset derecho ligado");
            } else {
                size_t max_rows = op->as.join.policy.max_output_rows;
                if (limits->max_output_rows < max_rows)
                    max_rows = limits->max_output_rows;
                if (!max_rows) {
                    status = MILENA_ERR_OVERFLOW;
                    canonical_span_error(error, status, &op->span,
                                         "El límite de filas join es cero");
                } else {
                    const char *left_keys[1] = {op->as.join.left_key.name};
                    const char *right_keys[1] = {op->as.join.right_key.name};
                    MilenaTable joined = {0};
                    milena_table_init(&joined);
                    status = milena_table_join_with_limits(
                        &joined, &working, program->right_table, left_keys,
                        right_keys, 1, op->as.join.join_type,
                        op->as.join.memory_budget_bytes, max_rows, error);
                    if (status == MILENA_OK &&
                        joined.column_count > limits->max_columns) {
                        status = MILENA_ERR_OVERFLOW;
                        canonical_span_error(error, status, &op->span,
                            "La unión excede el límite de columnas HIR");
                    }
                    if (status == MILENA_OK) milena_table_swap(&working, &joined);
                    else hir_attach_error_span(error, &op->span);
                    milena_table_destroy(&joined);
                }
            }
        } else {
            status = MILENA_ERR_UNSUPPORTED;
            canonical_span_error(error, status, &op->span,
                                 "Operación de datos no está en el subconjunto HIR ejecutable");
        }
        if (status == MILENA_OK && working.row_count > limits->max_output_rows) {
            canonical_span_error(error, MILENA_ERR_OVERFLOW, &op->span,
                                 "La operación excede el límite de filas HIR");
            status = MILENA_ERR_OVERFLOW;
        }
        if (status == MILENA_OK && working.column_count > limits->max_columns) {
            canonical_span_error(error, MILENA_ERR_OVERFLOW, &op->span,
                                 "La operación excede el límite de columnas HIR");
            status = MILENA_ERR_OVERFLOW;
        }
        if (status != MILENA_OK) hir_attach_error_span(error, &op->span);
    }
    if (status == MILENA_OK &&
        (working.row_count > limits->max_output_rows ||
         working.column_count > limits->max_columns)) {
        canonical_error(error, MILENA_ERR_OVERFLOW,
                        "La salida excede la política de recursos HIR");
        status = MILENA_ERR_OVERFLOW;
    }
    if (status == MILENA_OK) {
        milena_table_swap(output, &working);
    }
    milena_table_destroy(&working);
    return status;
}

MilenaStatus milena_canonical_compatibility_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error) {
    if (input) {
        input->ast = NULL;
        input->table = NULL;
        input->right_table = NULL;
        input->hir = NULL;
        input->data_hir = NULL;
    }
    if (error) milena_error_clear(error);
    if (!program || !program->ast || !input) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La entrada de compatibilidad canónica es inválida");
        return MILENA_ERR_ARGUMENT;
    }
    input->ast = program->ast;
    input->table = program->table;
    input->right_table = program->right_table;
    input->hir = program->hir;
    input->data_hir = program->data_hir;
    return MILENA_OK;
}

static bool hir_supports_ast_node(const ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_PROGRAMA:
        case AST_BLOQUE_FUNCION:
        case AST_CONDICION_SI:
        case AST_DECLARACION_FUNCION:
            return true;
        case AST_EXPRESION_OPERACION:
        case AST_EXPRESION_LITERAL:
        case AST_EXPRESION_IDENTIFICADOR:
        case AST_EXPRESION_FUNCION:
        case AST_EXPRESION_LLAMADA:
        case AST_COMANDO_RETORNAR:
        case AST_DECLARACION_VARIABLE:
        case AST_ASIGNACION_VARIABLE: {
            MilenaHIRValueType type;
            return hir_value_type(node->value_type, &type);
        }
        default:
            return false;
    }
}

static const ASTNode *hir_first_unsupported_node(const ASTNode *node) {
    if (!node) return NULL;
    if (!hir_supports_ast_node(node)) return node;
    for (size_t i = 0; i < node->child_count; ++i) {
        const ASTNode *unsupported = hir_first_unsupported_node(node->children[i]);
        if (unsupported) return unsupported;
    }
    return NULL;
}

static bool hir_supports_data_ast_node(const ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_COMANDO_CONDICION:
            return node->has_filter_predicate && node->filter_column != NULL;
        case AST_PROGRAMA:
        case AST_BLOQUE_ANALISIS:
        case AST_LLAMADA_CARGAR:
        case AST_DECLARACION_VARIABLE:
        case AST_BLOQUE_TRANSFORMAR:
        case AST_COMANDO_TOTAL:
        case AST_BLOQUE_LIMPIAR:
        case AST_COMANDO_NULOS:
        case AST_COMANDO_DUPLICADOS:
        case AST_BLOQUE_FILTRAR:
        case AST_BLOQUE_AGRUPAR:
        case AST_AGRUPACION_POR:
        case AST_RESUMEN_METRICA:
        case AST_BLOQUE_RESUMIR:
        case AST_BLOQUE_UNIR:
        case AST_COMANDO_DERECHA:
        case AST_COMANDO_CLAVE:
        case AST_BLOQUE_SELECCIONAR:
        case AST_COMANDO_COLUMNAS:
        case AST_BLOQUE_EXPORTAR:
            return true;
        default:
            return false;
    }
}

static const ASTNode *hir_first_unsupported_data_node(const ASTNode *node) {
    if (!node) return NULL;
    if (!hir_supports_data_ast_node(node)) return node;
    for (size_t i = 0; i < node->child_count; ++i) {
        const ASTNode *unsupported =
            hir_first_unsupported_data_node(node->children[i]);
        if (unsupported) return unsupported;
    }
    return NULL;
}

MilenaStatus milena_canonical_hir_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error) {
    if (input) {
        input->ast = NULL;
        input->table = NULL;
        input->right_table = NULL;
        input->hir = NULL;
        input->data_hir = NULL;
    }
    if (error) milena_error_clear(error);
    if (!program || !program->ast || !input) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La entrada HIR del compilador canónico es inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (program->data_hir && !program->data_hir->schema_bound) {
        canonical_error(error, MILENA_ERR_DATA,
                        "La HIR de datos requiere enlace de esquema antes del consumo");
        return MILENA_ERR_DATA;
    }
    if (!program->hir && !program->data_hir) {
        const ASTNode *unsupported = NULL;
        if (program->ast->child_count == 1 && program->ast->children[0] &&
            program->ast->children[0]->type == AST_BLOQUE_ANALISIS) {
            unsupported = hir_first_unsupported_data_node(program->ast);
            if (!unsupported) unsupported = program->ast->children[0];
        } else {
            unsupported = hir_first_unsupported_node(program->ast);
        }
        if (!unsupported) unsupported = program->ast;
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
                       "El backend HIR no representa todavía el nodo %s; "
                       "el AST se conserva y este programa no debe compilarse por HIR",
                       ast_type_name(unsupported->type));
        const ASTNode *location = unsupported;
        while (location && !location->has_source_span && location->line <= 0)
            location = location->parent;
        milena_error_set(error, MILENA_ERR_UNSUPPORTED,
                         location && location->line > 0 ? (size_t)location->line : 0,
                         location && location->column > 0 ? (size_t)location->column : 0,
                         0, message);
        return MILENA_ERR_UNSUPPORTED;
    }
    return milena_canonical_compatibility_input(program, input, error);
}

MilenaStatus milena_canonical_compiler_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error) {
    return milena_canonical_hir_input(program, input, error);
}
