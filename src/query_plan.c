#include "query_plan.h"
#include "stream.h"

#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>

static MilenaStatus plan_error(MilenaError *error, MilenaStatus code,
                               const char *message) {
    milena_error_set(error, code, 0, 0, 0, message);
    return code;
}

static bool supported_analysis_child(ASTNodeType type) {
    return type == AST_LLAMADA_CARGAR ||
           type == AST_BLOQUE_RESUMIR ||
           type == AST_BLOQUE_AGRUPAR ||
           type == AST_STREAM_FILTER ||
           type == AST_BLOQUE_EXPORTAR ||
           type == AST_DECLARACION_VARIABLE ||
           type == AST_DECLARACION_ENTRADA ||
           type == AST_DECLARACION_SALIDA;
}

static bool valid_summary(const ASTNode *summary, bool allow_legacy) {
    if (!summary || summary->type != AST_BLOQUE_RESUMIR ||
        summary->child_count == 0 || summary->child_count > MILENA_STREAM_MAX_METRICS) return false;
    for (size_t i = 0; i < summary->child_count; i++) {
        const ASTNode *metric = summary->children[i];
        if (!metric || metric->type != AST_RESUMEN_METRICA ||
            !metric->value || !metric->value[0]) return false;
        if (metric->stream_operation == AST_STREAM_OPERATION_NONE) {
            /* Keep the documented legacy operation:column form available only
             * to the global summary backend; grouped metrics are AST-typed. */
            const char *separator = strchr(metric->value, ':');
            if (!allow_legacy || !separator || separator == metric->value ||
                !separator[1]) return false;
        }
    }
    return true;
}

MilenaStatus milena_stream_execution_plan_build(
    const ASTNode *analysis, MilenaStreamExecutionPlan *plan,
    MilenaError *error) {
    if (!analysis || !plan || analysis->type != AST_BLOQUE_ANALISIS)
        return plan_error(error, MILENA_ERR_ARGUMENT,
                          "El plan de flujo requiere un bloque .analisis válido");

    memset(plan, 0, sizeof(*plan));
    const ASTNode *global_summary = NULL;
    for (size_t i = 0; i < analysis->child_count; i++) {
        const ASTNode *node = analysis->children[i];
        if (!node)
            return plan_error(error, MILENA_ERR_PARSE,
                              "El plan lógico contiene un nodo AST nulo");
        if (!supported_analysis_child(node->type))
            return plan_error(error, MILENA_ERR_UNSUPPORTED,
                              "El plan de flujo no admite este operador lógico");
        switch (node->type) {
        case AST_LLAMADA_CARGAR:
            if (plan->source)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "El plan de flujo requiere una única fuente");
            if (!node->value || !node->value[0] || !node->type_name ||
                strcmp(node->type_name, "flujo") != 0)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "El plan de flujo requiere una fuente CSV en modo flujo");
            plan->source = node;
            break;
        case AST_STREAM_FILTER: {
            bool valid_filter = node->value && node->value[0];
            if (node->stream_filter_kind == AST_STREAM_FILTER_TEXT_EQUAL)
                valid_filter = valid_filter && node->type_name != NULL;
            else if (node->stream_filter_kind == AST_STREAM_FILTER_NUMERIC_GREATER)
                valid_filter = valid_filter && node->type_name == NULL &&
                               isfinite(node->number_value);
            else
                valid_filter = false;
            if (plan->filter || !valid_filter)
                return plan_error(error, MILENA_ERR_PARSE,
                    "El plan admite un único filtro textual == o numérico > con forma tipada");
            plan->filter = node;
            break;
        }
        case AST_BLOQUE_RESUMIR:
            if (global_summary)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "El plan de flujo no admite resúmenes globales duplicados");
            global_summary = node;
            break;
        case AST_BLOQUE_AGRUPAR:
            if (plan->group)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "El plan de flujo no admite agrupaciones duplicadas");
            plan->group = node;
            break;
        case AST_BLOQUE_EXPORTAR:
            if (plan->sink)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "El plan de flujo no admite destinos duplicados");
            if (!node->value || !node->value[0])
                return plan_error(error, MILENA_ERR_PARSE,
                                  "El reporte del plan necesita una ruta de salida");
            plan->sink = node;
            break;
        default:
            /* These declarations are metadata already checked by semantics. */
            break;
        }
    }
    if (!plan->source)
        return plan_error(error, MILENA_ERR_PARSE,
                          "El plan de flujo necesita una fuente CSV");
    if (plan->group && global_summary)
        return plan_error(error, MILENA_ERR_PARSE,
                          "El plan no puede mezclar agregado global y agrupación");

    if (plan->group) {
        for (size_t i = 0; i < plan->group->child_count; i++) {
            const ASTNode *child = plan->group->children[i];
            if (!child)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "La agrupación del plan contiene un nodo nulo");
            if (child->type == AST_AGRUPACION_POR) {
                if (plan->group_key_count >= 2 || !child->value || !child->value[0])
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "El plan agrupado admite como máximo dos claves válidas");
                plan->group_keys[plan->group_key_count++] = child;
                if (!plan->group_key) plan->group_key = child;
            } else if (child->type == AST_BLOQUE_RESUMIR) {
                if (plan->group_summary)
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "El plan agrupado requiere un único resumen");
                plan->group_summary = child;
            } else if (child->type == AST_AGRUPACION_SPILL) {
                if (plan->spill_policy || !child->value || !child->value[0] ||
                    child->group_memory_budget_bytes == 0 ||
                    child->group_spill_quota_bytes == 0 ||
                    child->group_max_key_bytes < 3 ||
                    child->group_max_output_groups == 0 ||
                    child->group_max_output_bytes == 0 ||
                    child->group_max_output_bytes > 1073741824u ||
                    child->group_max_runs == 0 || child->group_max_runs > 65536u)
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "La política spill del plan es única y debe tener límites positivos");
                plan->spill_policy = child;
            } else {
                return plan_error(error, MILENA_ERR_UNSUPPORTED,
                                  "El plan agrupado contiene un operador no soportado");
            }
        }
        if (plan->group_key_count == 0 || !plan->group_key ||
            !valid_summary(plan->group_summary, false))
            return plan_error(error, MILENA_ERR_PARSE,
                              "El plan agrupado requiere clave y métricas tipadas");
        if (plan->group_key_count > 1 && !plan->spill_policy)
            return plan_error(error, MILENA_ERR_UNSUPPORTED,
                              "La agrupación de varias claves solo está soportada con #spill");
        if (plan->spill_policy) {
            if (plan->group_summary->child_count > MILENA_STREAM_MAX_METRICS)
                return plan_error(error, MILENA_ERR_UNSUPPORTED,
                                  "El spill agrupado admite como máximo 64 métricas");
            for (size_t i = 0; i < plan->group_summary->child_count; ++i) {
                ASTStreamOperation op = plan->group_summary->children[i]->stream_operation;
                if (op != AST_STREAM_OPERATION_SUM && op != AST_STREAM_OPERATION_MEAN &&
                    op != AST_STREAM_OPERATION_MIN && op != AST_STREAM_OPERATION_MAX &&
                    op != AST_STREAM_OPERATION_COUNT)
                    return plan_error(error, MILENA_ERR_UNSUPPORTED,
                                      "El plan spill solo admite suma, media, mínimo, máximo y contar");
            }
        }
        plan->summary = plan->group_summary;
        plan->physical_operator = plan->spill_policy
            ? MILENA_PHYSICAL_CSV_STREAM_GROUPED_SPILL
            : MILENA_PHYSICAL_CSV_STREAM_GROUPED;
        plan->logical_operators[1] = MILENA_LOGICAL_GROUP_AGGREGATE;
    } else {
        if (!valid_summary(global_summary, true))
            return plan_error(error, MILENA_ERR_PARSE,
                              "El plan global requiere un resumen con métricas tipadas");
        plan->summary = global_summary;
        plan->physical_operator = MILENA_PHYSICAL_CSV_STREAM_SUMMARY;
        plan->logical_operators[1] = MILENA_LOGICAL_GLOBAL_AGGREGATE;
    }

    plan->logical_operators[0] = MILENA_LOGICAL_CSV_SCAN;
    if (plan->filter) {
        plan->logical_operators[1] = MILENA_LOGICAL_FILTER;
        plan->logical_operators[2] = plan->group
            ? MILENA_LOGICAL_GROUP_AGGREGATE : MILENA_LOGICAL_GLOBAL_AGGREGATE;
        plan->logical_operators[3] = MILENA_LOGICAL_JSON_REPORT;
        plan->logical_operator_count = 4;
    } else {
        plan->logical_operators[1] = plan->group
            ? MILENA_LOGICAL_GROUP_AGGREGATE : MILENA_LOGICAL_GLOBAL_AGGREGATE;
        plan->logical_operators[2] = MILENA_LOGICAL_JSON_REPORT;
        plan->logical_operator_count = 3;
    }

    /* One typed plan carries both logical intent and the physical CSV pipeline.
     * CSV is scanned as complete records; byte-range partitioning is forbidden. */
    plan->physical_operators[plan->physical_operator_count++] =
        MILENA_STREAM_PLAN_SCAN_CSV_RECORDS;
    if (plan->filter)
        plan->physical_operators[plan->physical_operator_count++] =
            MILENA_STREAM_PLAN_FILTER;
    if (plan->group) {
        plan->physical_operators[plan->physical_operator_count++] =
            plan->spill_policy ? MILENA_STREAM_PLAN_GROUPED_SPILL :
                                 MILENA_STREAM_PLAN_GROUPED_AGGREGATE;
        if (plan->spill_policy) {
            plan->physical_operators[plan->physical_operator_count++] =
                MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES;
        }
        plan->physical_operators[plan->physical_operator_count++] =
            MILENA_STREAM_PLAN_ORDER_BY_KEY;
    } else {
        plan->physical_operators[plan->physical_operator_count++] =
            MILENA_STREAM_PLAN_SUMMARY_AGGREGATE;
    }
    plan->physical_operators[plan->physical_operator_count++] =
        MILENA_STREAM_PLAN_JSON_SINK;
    plan->partition_count = 1u;
    plan->worker_count = 1u;
    plan->csv_record_safe = true;
    plan->parallel_enabled = false;
    plan->physical_plan_reason =
        "CSV se procesa por registros completos; no se divide por rangos de bytes";
    return milena_stream_execution_plan_validate(plan, error);
}

MilenaStatus milena_stream_execution_plan_validate(
    const MilenaStreamExecutionPlan *plan, MilenaError *error) {
    if (!plan || !plan->source || !plan->summary ||
        plan->physical_operator_count == 0 ||
        plan->physical_operator_count > MILENA_STREAM_PLAN_MAX_OPERATORS ||
        plan->partition_count != 1u || plan->worker_count != 1u ||
        !plan->csv_record_safe || plan->parallel_enabled ||
        !plan->physical_plan_reason) {
        return plan_error(error, MILENA_ERR_UNSUPPORTED,
            "El plan de CSV debe conservar registros completos y ejecutarse en un worker");
    }
    MilenaStreamPlanOperator expected[MILENA_STREAM_PLAN_MAX_OPERATORS];
    size_t count = 0;
    expected[count++] = MILENA_STREAM_PLAN_SCAN_CSV_RECORDS;
    if (plan->filter) expected[count++] = MILENA_STREAM_PLAN_FILTER;
    if (plan->group) {
        expected[count++] = plan->spill_policy ? MILENA_STREAM_PLAN_GROUPED_SPILL :
                                                  MILENA_STREAM_PLAN_GROUPED_AGGREGATE;
        if (plan->spill_policy)
            expected[count++] = MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES;
        expected[count++] = MILENA_STREAM_PLAN_ORDER_BY_KEY;
    } else {
        expected[count++] = MILENA_STREAM_PLAN_SUMMARY_AGGREGATE;
    }
    expected[count++] = MILENA_STREAM_PLAN_JSON_SINK;
    if (count != plan->physical_operator_count)
        return plan_error(error, MILENA_ERR_DATA,
                          "La cadena física del plan de CSV tiene longitud inválida");
    for (size_t i = 0; i < count; ++i) {
        if (plan->physical_operators[i] != expected[i])
            return plan_error(error, MILENA_ERR_DATA,
                              "El orden de operadores físicos de CSV es inválido");
    }
    if ((!plan->group && plan->physical_operator != MILENA_PHYSICAL_CSV_STREAM_SUMMARY) ||
        (plan->group && !plan->spill_policy &&
         plan->physical_operator != MILENA_PHYSICAL_CSV_STREAM_GROUPED) ||
        (plan->group && plan->spill_policy &&
         plan->physical_operator != MILENA_PHYSICAL_CSV_STREAM_GROUPED_SPILL))
        return plan_error(error, MILENA_ERR_DATA,
                          "El operador físico no coincide con el plan AST");
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

static MilenaStatus arrow_plan_error(MilenaError *error, MilenaStatus code,
                                     const char *message) {
    milena_error_set(error, code, 0, 0, 0, message);
    return code;
}

static const ASTNode *arrow_plan_find_declaration(const ASTNode *analysis,
                                                   const char *name) {
    if (!analysis || !name) return NULL;
    for (size_t i = 0; i < analysis->child_count; ++i) {
        const ASTNode *node = analysis->children[i];
        if (node && node->type == AST_DECLARACION_VARIABLE && node->value &&
            strcmp(node->value, name) == 0) return node;
    }
    return NULL;
}

static bool arrow_plan_declaration_is(const ASTNode *declaration,
                                      const char *type_name) {
    return declaration && declaration->type_name && type_name &&
           strcmp(declaration->type_name, type_name) == 0;
}

MilenaStatus milena_arrow_ipc_execution_plan_build(
    const ASTNode *analysis, MilenaArrowIpcExecutionPlan *plan,
    MilenaError *error) {
    if (!analysis || !plan || analysis->type != AST_BLOQUE_ANALISIS)
        return arrow_plan_error(error, MILENA_ERR_ARGUMENT,
                                "Arrow IPC plan requires a valid analysis block");
    memset(plan, 0, sizeof(*plan));
    for (size_t i = 0; i < analysis->child_count; ++i) {
        const ASTNode *node = analysis->children[i];
        if (!node) return arrow_plan_error(error, MILENA_ERR_PARSE,
                                           "Arrow IPC plan contains a null AST node");
        switch (node->type) {
        case AST_LLAMADA_CARGAR:
            if (plan->source || !node->type_name ||
                strcmp(node->type_name, "arrow_ipc_stream") != 0 ||
                !node->value || !node->value[0])
                return arrow_plan_error(error, MILENA_ERR_PARSE,
                    "Arrow IPC plan requires exactly one local IPC STREAM source");
            plan->source = node;
            break;
        case AST_COLUMNAR_PROJECT:
            if (plan->projection)
                return arrow_plan_error(error, MILENA_ERR_PARSE,
                                        "Arrow IPC plan allows one projection");
            plan->projection = node;
            break;
        case AST_STREAM_FILTER:
            if (plan->filter)
                return arrow_plan_error(error, MILENA_ERR_PARSE,
                                        "Arrow IPC plan allows one typed filter");
            plan->filter = node;
            break;
        case AST_BLOQUE_EXPORTAR:
            if (plan->sink || !node->value || !node->value[0])
                return arrow_plan_error(error, MILENA_ERR_PARSE,
                                        "Arrow IPC plan requires one stream output");
            plan->sink = node;
            break;
        case AST_DECLARACION_VARIABLE:
            /* Column declarations are resolved below and carried into the typed plan. */
            break;
        default:
            return arrow_plan_error(error, MILENA_ERR_UNSUPPORTED,
                "This Arrow IPC vertical only supports source/filter/project/stream sink");
        }
    }
    if (!plan->source || !plan->projection || plan->projection->child_count == 0 ||
        plan->projection->child_count > 128u || !plan->sink)
        return arrow_plan_error(error, MILENA_ERR_PARSE,
            "Arrow IPC vertical requires source, non-empty projection, and output");
    for (size_t i = 0; i < plan->projection->child_count; ++i) {
        const ASTNode *field = plan->projection->children[i];
        if (!field || field->type != AST_COLUMNAR_FIELD || !field->value ||
            !field->value[0])
            return arrow_plan_error(error, MILENA_ERR_PARSE,
                                    "Arrow projection contains an invalid field");
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(plan->projection->children[j]->value, field->value) == 0)
                return arrow_plan_error(error, MILENA_ERR_PARSE,
                                        "Arrow projection fields must be unique");
        }
        const ASTNode *declaration = arrow_plan_find_declaration(analysis, field->value);
        if (!arrow_plan_declaration_is(declaration, "numerica") &&
            !arrow_plan_declaration_is(declaration, "texto"))
            return arrow_plan_error(error, MILENA_ERR_TYPE,
                "Arrow projected fields require a supported numerica or texto declaration");
        plan->projection_declarations[i] = declaration;
    }
    if (plan->filter) {
        plan->filter_declaration = arrow_plan_find_declaration(analysis,
                                                                plan->filter->value);
        if (plan->filter->stream_filter_kind == AST_STREAM_FILTER_TEXT_EQUAL) {
            if (!arrow_plan_declaration_is(plan->filter_declaration, "texto"))
                return arrow_plan_error(error, MILENA_ERR_TYPE,
                    "Arrow text equality requires a texto declaration");
        } else if (plan->filter->stream_filter_kind ==
                   AST_STREAM_FILTER_NUMERIC_GREATER) {
            if (!arrow_plan_declaration_is(plan->filter_declaration, "numerica"))
                return arrow_plan_error(error, MILENA_ERR_TYPE,
                    "Arrow numeric comparison requires a numerica declaration");
        } else {
            return arrow_plan_error(error, MILENA_ERR_UNSUPPORTED,
                                     "Arrow filter kind is not supported");
        }
    }
    plan->logical_operators[plan->logical_operator_count++] =
        MILENA_ARROW_LOGICAL_SCAN_STREAM;
    plan->physical_operators[plan->physical_operator_count++] =
        MILENA_ARROW_PHYSICAL_IPC_STREAM_SCAN;
    if (plan->filter) {
        plan->logical_operators[plan->logical_operator_count++] =
            MILENA_ARROW_LOGICAL_FILTER;
        plan->physical_operators[plan->physical_operator_count++] =
            MILENA_ARROW_PHYSICAL_FILTER;
    }
    plan->logical_operators[plan->logical_operator_count++] =
        MILENA_ARROW_LOGICAL_PROJECT;
    plan->physical_operators[plan->physical_operator_count++] =
        MILENA_ARROW_PHYSICAL_PROJECT;
    plan->logical_operators[plan->logical_operator_count++] =
        MILENA_ARROW_LOGICAL_STREAM_SINK;
    plan->physical_operators[plan->physical_operator_count++] =
        MILENA_ARROW_PHYSICAL_IPC_STREAM_WRITE;
    plan->physical_plan_reason =
        "Local Arrow IPC STREAM batches are validated and transformed sequentially";
    return milena_arrow_ipc_execution_plan_validate(plan, error);
}

MilenaStatus milena_arrow_ipc_execution_plan_validate(
    const MilenaArrowIpcExecutionPlan *plan, MilenaError *error) {
    if (!plan || !plan->source || !plan->projection || !plan->sink ||
        plan->projection->child_count == 0 ||
        plan->physical_operator_count != (plan->filter ? 4u : 3u) ||
        plan->logical_operator_count != plan->physical_operator_count ||
        !plan->physical_plan_reason ||
        plan->physical_operators[0] != MILENA_ARROW_PHYSICAL_IPC_STREAM_SCAN ||
        plan->physical_operators[plan->physical_operator_count - 2u] !=
            MILENA_ARROW_PHYSICAL_PROJECT ||
        plan->physical_operators[plan->physical_operator_count - 1u] !=
            MILENA_ARROW_PHYSICAL_IPC_STREAM_WRITE ||
        (plan->filter && plan->physical_operators[1] != MILENA_ARROW_PHYSICAL_FILTER))
        return arrow_plan_error(error, MILENA_ERR_DATA,
                                "Invalid typed Arrow IPC STREAM physical plan");
    if (plan->projection->child_count > MILENA_ARROW_PLAN_MAX_COLUMNS)
        return arrow_plan_error(error, MILENA_ERR_OVERFLOW,
                                "Arrow projection exceeds the typed plan column limit");
    for (size_t i = 0; i < plan->projection->child_count; ++i) {
        const ASTNode *declaration = plan->projection_declarations[i];
        if (!declaration ||
            (!arrow_plan_declaration_is(declaration, "numerica") &&
             !arrow_plan_declaration_is(declaration, "texto")))
            return arrow_plan_error(error, MILENA_ERR_TYPE,
                                    "Arrow projection plan has no supported typed declaration");
    }
    if (plan->filter && !plan->filter_declaration)
        return arrow_plan_error(error, MILENA_ERR_TYPE,
                                "Arrow filter plan has no typed declaration");
    return MILENA_OK;
}

#define MILENA_SQL_PLAN_DEFAULT_ROWS 10000u
#define MILENA_SQL_PLAN_MAX_ROWS 100000u
#define MILENA_SQL_PLAN_DEFAULT_BYTES (8u * 1024u * 1024u)
#define MILENA_SQL_PLAN_MAX_BYTES (64u * 1024u * 1024u)
#define MILENA_SQL_PLAN_DEFAULT_TIMEOUT_MS 2000u
#define MILENA_SQL_PLAN_MAX_TIMEOUT_MS 30000u
#define MILENA_SQL_PLAN_MAX_OPERATIONS 10000u
#define MILENA_SQL_PLAN_MAX_PARAMETERS 999u

#define MILENA_SQL_PLAN_MAX_TEXT (1024u * 1024u)

#define MILENA_SQL_PLAN_DEFAULT_ROWS 10000u
#define MILENA_SQL_PLAN_MAX_ROWS 100000u
#define MILENA_SQL_PLAN_DEFAULT_BYTES (8u * 1024u * 1024u)
#define MILENA_SQL_PLAN_MAX_BYTES (64u * 1024u * 1024u)
#define MILENA_SQL_PLAN_DEFAULT_TIMEOUT_MS 2000u
#define MILENA_SQL_PLAN_MAX_TIMEOUT_MS 30000u
#define MILENA_SQL_PLAN_MAX_OPERATIONS 10000u
#define MILENA_SQL_PLAN_MAX_PARAMETERS 999u
#define MILENA_SQL_PLAN_MAX_TEXT (1024u * 1024u)
#define MILENA_SQL_TYPED_MAX_COLUMNS 128u

typedef struct {
    const ASTNode *source;
    const ASTNode *table_schema;
    size_t source_index;
} MilenaSqlSchemaEntry;

static bool sql_identifier_valid(const char *name) {
    if (!name || !name[0]) return false;
    unsigned char first = (unsigned char)name[0];
    if (!((first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z') || first == '_')) return false;
    for (const unsigned char *p = (const unsigned char *)name + 1; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

static const char *sql_operator_sql_text(ASTSqlOperator op);

static const char *sql_ast_type_name(ASTSqlType type) {
    switch (type) {
    case AST_SQL_TYPE_INTEGER: return "entero";
    case AST_SQL_TYPE_REAL: return "real";
    case AST_SQL_TYPE_TEXT: return "texto";
    case AST_SQL_TYPE_BOOLEAN: return "booleano";
    default: return NULL;
    }
}

static const ASTNode *sql_schema_find_column(const ASTNode *schema,
                                             const char *name) {
    if (!schema || !name) return NULL;
    for (size_t i = 0; i < schema->child_count; ++i) {
        const ASTNode *column = schema->children[i];
        if (column && column->type == AST_SQL_SCHEMA_COLUMN &&
            column->value && strcmp(column->value, name) == 0) return column;
    }
    return NULL;
}

static const ASTNode *sql_schema_before(const ASTNode *program,
                                        size_t before_index,
                                        const char *table_name) {
    if (!program || !table_name) return NULL;
    if (before_index > program->child_count) before_index = program->child_count;
    for (size_t i = 0; i < before_index; ++i) {
        const ASTNode *node = program->children[i];
        if (node && node->type == AST_SQL_TABLE_SCHEMA && node->value &&
            strcmp(node->value, table_name) == 0) return node;
    }
    return NULL;
}

static ASTSqlType sql_parameter_expected_type(const ASTNode *parameter) {
    if (!parameter || !parameter->type_name) return AST_SQL_TYPE_UNSPECIFIED;
    if (strcmp(parameter->type_name, "entero") == 0) return AST_SQL_TYPE_INTEGER;
    if (strcmp(parameter->type_name, "real") == 0) return AST_SQL_TYPE_REAL;
    if (strcmp(parameter->type_name, "texto") == 0) return AST_SQL_TYPE_TEXT;
    if (strcmp(parameter->type_name, "booleano") == 0) return AST_SQL_TYPE_BOOLEAN;
    return AST_SQL_TYPE_UNSPECIFIED;
}

static bool sql_parameter_type_matches_ast(const ASTNode *parameter) {
    if (!parameter || !parameter->type_name) return false;
    if (strcmp(parameter->type_name, "nulo") == 0)
        return parameter->sql_type == AST_SQL_TYPE_UNSPECIFIED;
    ASTSqlType expected = sql_parameter_expected_type(parameter);
    return expected != AST_SQL_TYPE_UNSPECIFIED && parameter->sql_type == expected;
}

static bool sql_parameter_is_null(const ASTNode *parameter) {
    return parameter && parameter->type == AST_SQL_PARAMETER &&
        parameter->type_name && strcmp(parameter->type_name, "nulo") == 0;
}

static bool sql_parameter_literal_valid(const ASTNode *parameter) {
    if (!parameter || parameter->type != AST_SQL_PARAMETER ||
        !parameter->type_name || !parameter->value) return false;
    if (strcmp(parameter->type_name, "texto") == 0) return true;
    if (strcmp(parameter->type_name, "booleano") == 0)
        return strcmp(parameter->value, "verdadero") == 0 ||
               strcmp(parameter->value, "falso") == 0;
    if (strcmp(parameter->type_name, "nulo") == 0)
        return strcmp(parameter->value, "nulo") == 0;
    char *end = NULL;
    errno = 0;
    if (strcmp(parameter->type_name, "entero") == 0) {
        (void)strtoll(parameter->value, &end, 10);
    } else if (strcmp(parameter->type_name, "real") == 0) {
        double parsed = strtod(parameter->value, &end);
        if (!isfinite(parsed)) errno = ERANGE;
    } else return false;
    return !errno && end && end != parameter->value && *end == '\0';
}

MilenaStatus milena_sql_semantic_validate(const ASTNode *program,
                                            MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!program || program->type != AST_SQL_PROGRAM || !program->value ||
        !program->value[0] || !program->child_count ||
        program->child_count > MILENA_SQL_PLAN_MAX_OPERATIONS ||
        !program->children)
        return plan_error(error, MILENA_ERR_ARGUMENT,
                          "El programa SQL requiere ruta y operaciones válidas");
    size_t executable_count = 0;
    for (size_t i = 0; i < program->child_count; ++i) {
        const ASTNode *node = program->children[i];
        if (!node || node->parent != program)
            return plan_error(error, MILENA_ERR_PARSE,
                              "El programa SQL contiene una operación AST inválida");
        if (node->type == AST_SQL_TABLE_SCHEMA) {
            if (!sql_identifier_valid(node->value) || !node->child_count ||
                node->child_count > MILENA_SQL_TYPED_MAX_COLUMNS || !node->children)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "Esquema SQL con nombre o cantidad de columnas inválidos");
            for (size_t prior = 0; prior < i; ++prior) {
                const ASTNode *other = program->children[prior];
                if (other && other->type == AST_SQL_TABLE_SCHEMA && other->value &&
                    strcmp(other->value, node->value) == 0)
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Tabla SQL declarada más de una vez en el bloque");
            }
            for (size_t c = 0; c < node->child_count; ++c) {
                const ASTNode *column = node->children[c];
                const char *type_name = column ? sql_ast_type_name(column->sql_type) : NULL;
                if (!column || column->parent != node ||
                    column->type != AST_SQL_SCHEMA_COLUMN ||
                    !sql_identifier_valid(column->value) || !type_name ||
                    !column->type_name || strcmp(column->type_name, type_name) != 0 ||
                    column->child_count)
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Columna del esquema SQL inválida o sin tipo soportado");
                for (size_t prior = 0; prior < c; ++prior) {
                    const ASTNode *other = node->children[prior];
                    if (other && other->value &&
                        strcmp(other->value, column->value) == 0)
                        return plan_error(error, MILENA_ERR_TYPE,
                                          "Columna SQL declarada más de una vez");
                }
            }
        } else if (node->type == AST_SQL_TYPED_SELECT) {
            ++executable_count;
            if (node->child_count != 3u || !node->children)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "SELECT tipado SQL requiere tabla, proyección y filtro");
            const ASTNode *table = node->children[0];
            const ASTNode *projection = node->children[1];
            const ASTNode *filter = node->children[2];
            if (!table || table->parent != node ||
                table->type != AST_SQL_TABLE_REFERENCE ||
                !sql_identifier_valid(table->value) || table->child_count ||
                !projection || projection->parent != node ||
                projection->type != AST_SQL_PROJECTION_LIST ||
                !projection->child_count ||
                projection->child_count > MILENA_SQL_TYPED_MAX_COLUMNS ||
                !projection->children || !filter || filter->parent != node ||
                filter->type != AST_SQL_FILTER || filter->child_count != 3u ||
                !filter->children)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Forma del SELECT tipado SQL inválida");
            const ASTNode *schema = sql_schema_before(program, i, table->value);
            if (!schema)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El SELECT referencia una tabla no declarada en el bloque");
            for (size_t p = 0; p < projection->child_count; ++p) {
                const ASTNode *field = projection->children[p];
                if (!field || field->parent != projection ||
                    field->type != AST_SQL_PROJECTED_COLUMN ||
                    !sql_identifier_valid(field->value) || field->child_count)
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "Nombre de columna proyectada SQL inválido");
                if (!sql_schema_find_column(schema, field->value))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "El SELECT proyecta una columna no declarada en el esquema");
                for (size_t prior = 0; prior < p; ++prior) {
                    const ASTNode *other = projection->children[prior];
                    if (other && other->value &&
                        strcmp(other->value, field->value) == 0)
                        return plan_error(error, MILENA_ERR_TYPE,
                                          "La proyección SQL repite una columna");
                }
            }
            const ASTNode *filter_column = filter->children[0];
            const ASTNode *filter_operator = filter->children[1];
            const ASTNode *parameter = filter->children[2];
            if (!filter_column || filter_column->parent != filter ||
                filter_column->type != AST_SQL_FILTER_COLUMN ||
                !sql_identifier_valid(filter_column->value) ||
                filter_column->child_count || !filter_operator ||
                filter_operator->parent != filter ||
                filter_operator->type != AST_SQL_FILTER_OPERATOR ||
                filter_operator->child_count || !parameter ||
                parameter->parent != filter ||
                !sql_parameter_literal_valid(parameter))
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Predicado tipado SQL inválido");
            if (filter_operator->sql_operator != AST_SQL_OPERATOR_EQUAL)
                return plan_error(error, MILENA_ERR_UNSUPPORTED,
                                  "El slice SQL tipado solo admite el operador de igualdad '='");
            const ASTNode *declared_filter = sql_schema_find_column(
                schema, filter_column->value);
            if (!declared_filter)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El filtro SQL refiere una columna no declarada en el esquema");
            if (!sql_parameter_type_matches_ast(parameter) ||
                parameter->sql_type == AST_SQL_TYPE_UNSPECIFIED ||
                parameter->sql_type != declared_filter->sql_type)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El tipo del parámetro SQL no coincide con la columna filtrada");
        } else if (node->type == AST_SQL_TYPED_INSERT) {
            ++executable_count;
            if (node->child_count != 3u || !node->children)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "INSERT tipado SQL requiere tabla, columnas y valores");
            const ASTNode *table = node->children[0];
            const ASTNode *columns = node->children[1];
            const ASTNode *values = node->children[2];
            if (!table || table->parent != node ||
                table->type != AST_SQL_TABLE_REFERENCE ||
                !sql_identifier_valid(table->value) || table->child_count ||
                !columns || columns->parent != node ||
                columns->type != AST_SQL_INSERT_COLUMN_LIST ||
                !columns->child_count ||
                columns->child_count > MILENA_SQL_TYPED_MAX_COLUMNS ||
                !columns->children || !values || values->parent != node ||
                values->type != AST_SQL_INSERT_VALUE_LIST ||
                values->child_count != columns->child_count ||
                !values->children)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Forma del INSERT tipado SQL inválida");
            const ASTNode *schema = sql_schema_before(program, i, table->value);
            if (!schema)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El INSERT referencia una tabla no declarada en el bloque");
            for (size_t c = 0; c < columns->child_count; ++c) {
                const ASTNode *column = columns->children[c];
                const ASTNode *value = values->children[c];
                if (!column || column->parent != columns ||
                    column->type != AST_SQL_INSERT_COLUMN ||
                    !sql_identifier_valid(column->value) || column->child_count ||
                    !value || value->parent != values ||
                    !sql_parameter_literal_valid(value))
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "Columna o literal del INSERT tipado inválido");
                const ASTNode *declared = sql_schema_find_column(schema, column->value);
                if (!declared)
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "El INSERT refiere una columna no declarada en el esquema");
                for (size_t prior = 0; prior < c; ++prior) {
                    const ASTNode *other = columns->children[prior];
                    if (other && other->value &&
                        strcmp(other->value, column->value) == 0)
                        return plan_error(error, MILENA_ERR_TYPE,
                                          "El INSERT tipado repite una columna");
                }
                if (!sql_parameter_type_matches_ast(value) ||
                    (!sql_parameter_is_null(value) &&
                     (value->sql_type == AST_SQL_TYPE_UNSPECIFIED ||
                      value->sql_type != declared->sql_type)))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "El tipo del valor INSERT no coincide con la columna declarada");
            }
        } else if (node->type == AST_SQL_TYPED_UPDATE) {
            ++executable_count;
            if (node->child_count != 3u || !node->children)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "UPDATE tipado SQL requiere tabla, asignaciones y filtro");
            const ASTNode *table = node->children[0];
            const ASTNode *assignments = node->children[1];
            const ASTNode *filter = node->children[2];
            if (!table || table->parent != node ||
                table->type != AST_SQL_TABLE_REFERENCE ||
                !sql_identifier_valid(table->value) || table->child_count ||
                !assignments || assignments->parent != node ||
                assignments->type != AST_SQL_UPDATE_ASSIGNMENT_LIST ||
                !assignments->child_count ||
                assignments->child_count > MILENA_SQL_TYPED_MAX_COLUMNS ||
                !assignments->children || !filter || filter->parent != node ||
                filter->type != AST_SQL_UPDATE_FILTER || filter->child_count != 3u ||
                !filter->children)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Forma del UPDATE tipado SQL inválida");
            const ASTNode *schema = sql_schema_before(program, i, table->value);
            if (!schema)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El UPDATE referencia una tabla no declarada en el bloque");
            for (size_t a = 0; a < assignments->child_count; ++a) {
                const ASTNode *assignment = assignments->children[a];
                if (!assignment || assignment->parent != assignments ||
                    assignment->type != AST_SQL_UPDATE_ASSIGNMENT ||
                    assignment->child_count != 2u || !assignment->children)
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "Asignación UPDATE tipada inválida");
                const ASTNode *column = assignment->children[0];
                const ASTNode *value = assignment->children[1];
                if (!column || column->parent != assignment ||
                    column->type != AST_SQL_UPDATE_COLUMN ||
                    !sql_identifier_valid(column->value) || column->child_count ||
                    !value || value->parent != assignment ||
                    !sql_parameter_literal_valid(value))
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "Columna o literal de asignación UPDATE inválido");
                const ASTNode *declared = sql_schema_find_column(schema, column->value);
                if (!declared)
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "El UPDATE refiere una columna no declarada en el esquema");
                for (size_t prior = 0; prior < a; ++prior) {
                    const ASTNode *other = assignments->children[prior]->children[0];
                    if (other && other->value &&
                        strcmp(other->value, column->value) == 0)
                        return plan_error(error, MILENA_ERR_TYPE,
                                          "El UPDATE tipado repite una columna");
                }
                if (!sql_parameter_type_matches_ast(value) ||
                    (!sql_parameter_is_null(value) &&
                     (value->sql_type == AST_SQL_TYPE_UNSPECIFIED ||
                      value->sql_type != declared->sql_type)))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "El tipo del literal UPDATE no coincide con la columna declarada");
            }
            const ASTNode *filter_column = filter->children[0];
            const ASTNode *filter_operator = filter->children[1];
            const ASTNode *parameter = filter->children[2];
            if (!filter_column || filter_column->parent != filter ||
                filter_column->type != AST_SQL_FILTER_COLUMN ||
                !sql_identifier_valid(filter_column->value) ||
                filter_column->child_count || !filter_operator ||
                filter_operator->parent != filter ||
                filter_operator->type != AST_SQL_FILTER_OPERATOR ||
                filter_operator->child_count || !parameter ||
                parameter->parent != filter || !sql_parameter_literal_valid(parameter))
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Predicado UPDATE tipado inválido");
            if (!sql_operator_sql_text(filter_operator->sql_operator))
                return plan_error(error, MILENA_ERR_UNSUPPORTED,
                                  "Operador del filtro UPDATE no admitido");
            const ASTNode *declared_filter = sql_schema_find_column(schema,
                                                                     filter_column->value);
            if (!declared_filter)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El filtro UPDATE refiere una columna no declarada en el esquema");
            if (!sql_parameter_type_matches_ast(parameter) ||
                parameter->sql_type == AST_SQL_TYPE_UNSPECIFIED ||
                parameter->sql_type != declared_filter->sql_type)
                return plan_error(error, MILENA_ERR_TYPE,
                                  "El tipo del parámetro del filtro UPDATE no coincide con la columna");
        } else if (node->type == AST_SQL_QUERY || node->type == AST_SQL_EXECUTE) {
            ++executable_count;
            if (!node->value || !node->value[0] || node->child_count > 999u ||
                (node->child_count && !node->children))
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Sentencia SQL cruda incompleta");
            for (size_t p = 0; p < node->child_count; ++p)
                if (!sql_parameter_literal_valid(node->children[p]))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Parámetro SQL crudo inválido");
        } else if (node->type == AST_SQL_BEGIN || node->type == AST_SQL_COMMIT ||
                   node->type == AST_SQL_ROLLBACK) {
            if (node->child_count)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Operación transaccional SQL inválida");
        } else {
            return plan_error(error, MILENA_ERR_UNSUPPORTED,
                              "Operación AST no soportada por el backend SQL");
        }
    }
    if (!executable_count)
        return plan_error(error, MILENA_ERR_PARSE,
                          "El bloque SQL requiere al menos una consulta o ejecución");
    return MILENA_OK;
}

static bool sql_plan_parameter_matches_ast(const MilenaSqlPlanParameter *value,
                                           const ASTNode *parameter) {
    if (!value || !parameter || parameter->type != AST_SQL_PARAMETER ||
        !parameter->type_name || !parameter->value) return false;
    if (strcmp(parameter->type_name, "texto") == 0)
        return value->kind == MILENA_SQL_PLAN_TEXT &&
            value->value.text.data == parameter->value &&
            value->value.text.length == strlen(parameter->value);
    if (strcmp(parameter->type_name, "nulo") == 0)
        return value->kind == MILENA_SQL_PLAN_NULL &&
            strcmp(parameter->value, "nulo") == 0;
    if (strcmp(parameter->type_name, "booleano") == 0)
        return value->kind == MILENA_SQL_PLAN_INT64 &&
            (strcmp(parameter->value, "verdadero") == 0 ||
             strcmp(parameter->value, "falso") == 0) &&
            value->value.i64 == (strcmp(parameter->value, "verdadero") == 0 ? 1 : 0);
    if (strcmp(parameter->type_name, "entero") == 0) {
        char *end = NULL;
        errno = 0;
        long long parsed = strtoll(parameter->value, &end, 10);
        return value->kind == MILENA_SQL_PLAN_INT64 && !errno && end &&
            end != parameter->value && *end == '\0' &&
            value->value.i64 == (int64_t)parsed;
    }
    if (strcmp(parameter->type_name, "real") == 0) {
        char *end = NULL;
        errno = 0;
        double parsed = strtod(parameter->value, &end);
        return value->kind == MILENA_SQL_PLAN_FLOAT64 && !errno && end &&
            end != parameter->value && *end == '\0' && isfinite(parsed) &&
            value->value.f64 == parsed;
    }
    return false;
}

static bool sql_plan_parameter_from_ast(const ASTNode *parameter,
                                        MilenaSqlPlanParameter *value) {
    if (!parameter || !value || !parameter->type_name || !parameter->value)
        return false;
    memset(value, 0, sizeof(*value));
    if (strcmp(parameter->type_name, "texto") == 0) {
        value->kind = MILENA_SQL_PLAN_TEXT;
        value->value.text = (MilenaSqlPlanText){parameter->value,
                                                strlen(parameter->value)};
    } else if (strcmp(parameter->type_name, "nulo") == 0) {
        value->kind = MILENA_SQL_PLAN_NULL;
    } else if (strcmp(parameter->type_name, "booleano") == 0) {
        if (strcmp(parameter->value, "verdadero") != 0 &&
            strcmp(parameter->value, "falso") != 0) return false;
        value->kind = MILENA_SQL_PLAN_INT64;
        value->value.i64 = strcmp(parameter->value, "verdadero") == 0 ? 1 : 0;
    } else if (strcmp(parameter->type_name, "entero") == 0) {
        char *end = NULL;
        errno = 0;
        long long parsed = strtoll(parameter->value, &end, 10);
        if (errno || !end || end == parameter->value || *end) return false;
        value->kind = MILENA_SQL_PLAN_INT64;
        value->value.i64 = (int64_t)parsed;
    } else if (strcmp(parameter->type_name, "real") == 0) {
        char *end = NULL;
        errno = 0;
        double parsed = strtod(parameter->value, &end);
        if (errno || !end || end == parameter->value || *end || !isfinite(parsed))
            return false;
        value->kind = MILENA_SQL_PLAN_FLOAT64;
        value->value.f64 = parsed;
    } else return false;
    return true;
}

static bool sql_add_size(size_t *total, size_t amount) {
    return milena_size_add(*total, amount, total);
}

static char *sql_build_typed_statement(const ASTNode *table,
                                       const ASTNode *projection,
                                       const ASTNode *filter_column) {
    if (!table || !table->value || !projection || !filter_column ||
        !filter_column->value) return NULL;
    size_t length = 1u;
    if (!sql_add_size(&length, strlen("SELECT ")) ||
        !sql_add_size(&length, strlen(" FROM ")) ||
        !sql_add_size(&length, strlen(table->value) + 2u) ||
        !sql_add_size(&length, strlen(" WHERE ")) ||
        !sql_add_size(&length, strlen(filter_column->value) + 2u) ||
        !sql_add_size(&length, strlen(" = ?"))) return NULL;
    for (size_t i = 0; i < projection->child_count; ++i) {
        const ASTNode *field = projection->children[i];
        if (!field || !field->value ||
            !sql_add_size(&length, strlen(field->value) + 2u) ||
            (i && !sql_add_size(&length, 2u))) return NULL;
    }
    if (length > MILENA_SQL_PLAN_MAX_TEXT) return NULL;
    char *statement = malloc(length);
    if (!statement) return NULL;
    char *cursor = statement;
    memcpy(cursor, "SELECT ", 7u); cursor += 7u;
    for (size_t i = 0; i < projection->child_count; ++i) {
        const ASTNode *field = projection->children[i];
        if (i) { memcpy(cursor, ", ", 2u); cursor += 2u; }
        *cursor++ = '"';
        size_t n = strlen(field->value); memcpy(cursor, field->value, n); cursor += n;
        *cursor++ = '"';
    }
    memcpy(cursor, " FROM ", 6u); cursor += 6u;
    *cursor++ = '"';
    size_t table_length = strlen(table->value);
    memcpy(cursor, table->value, table_length); cursor += table_length;
    *cursor++ = '"';
    memcpy(cursor, " WHERE ", 7u); cursor += 7u;
    *cursor++ = '"';
    size_t filter_length = strlen(filter_column->value);
    memcpy(cursor, filter_column->value, filter_length); cursor += filter_length;
    memcpy(cursor, "\" = ?", 5u); cursor += 5u;
    *cursor = '\0';
    return statement;
}

static char *sql_build_typed_insert_statement(const ASTNode *table,
                                               const ASTNode *columns) {
    if (!table || !table->value || !columns || !columns->child_count ||
        !columns->children) return NULL;
    size_t length = 1u;
    if (!sql_add_size(&length, strlen("INSERT INTO \"")) ||
        !sql_add_size(&length, strlen(table->value)) ||
        !sql_add_size(&length, strlen("\" ("))) return NULL;
    for (size_t i = 0; i < columns->child_count; ++i) {
        const ASTNode *column = columns->children[i];
        if (!column || !column->value ||
            !sql_add_size(&length, strlen(column->value) + 2u) ||
            (i && !sql_add_size(&length, 2u))) return NULL;
    }
    if (!sql_add_size(&length, strlen(") VALUES ("))) return NULL;
    for (size_t i = 0; i < columns->child_count; ++i) {
        if (!sql_add_size(&length, 1u) ||
            (i && !sql_add_size(&length, 2u))) return NULL;
    }
    if (!sql_add_size(&length, 1u) || length > MILENA_SQL_PLAN_MAX_TEXT)
        return NULL;
    char *statement = malloc(length);
    if (!statement) return NULL;
    char *cursor = statement;
    const char *prefix = "INSERT INTO \"";
    size_t n = strlen(prefix); memcpy(cursor, prefix, n); cursor += n;
    n = strlen(table->value); memcpy(cursor, table->value, n); cursor += n;
    const char *between = "\" (";
    n = strlen(between); memcpy(cursor, between, n); cursor += n;
    for (size_t i = 0; i < columns->child_count; ++i) {
        const ASTNode *column = columns->children[i];
        if (i) { memcpy(cursor, ", ", 2u); cursor += 2u; }
        *cursor++ = '\"';
        n = strlen(column->value); memcpy(cursor, column->value, n); cursor += n;
        *cursor++ = '\"';
    }
    const char *values_prefix = ") VALUES (";
    n = strlen(values_prefix); memcpy(cursor, values_prefix, n); cursor += n;
    for (size_t i = 0; i < columns->child_count; ++i) {
        if (i) { memcpy(cursor, ", ", 2u); cursor += 2u; }
        *cursor++ = '?';
    }
    *cursor++ = ')';
    *cursor = '\0';
    return statement;
}

static const char *sql_operator_sql_text(ASTSqlOperator op) {
    switch (op) {
        case AST_SQL_OPERATOR_EQUAL: return "=";
        case AST_SQL_OPERATOR_NOT_EQUAL: return "!=";
        case AST_SQL_OPERATOR_LESS: return "<";
        case AST_SQL_OPERATOR_LESS_EQUAL: return "<=";
        case AST_SQL_OPERATOR_GREATER: return ">";
        case AST_SQL_OPERATOR_GREATER_EQUAL: return ">=";
        default: return NULL;
    }
}

static char *sql_build_typed_update_statement(const ASTNode *table,
                                              const ASTNode *assignments,
                                              const ASTNode *filter_column,
                                              const ASTNode *filter_operator) {
    const char *operator_text = filter_operator ?
        sql_operator_sql_text(filter_operator->sql_operator) : NULL;
    if (!table || !table->value || !assignments || !assignments->child_count ||
        !assignments->children || !filter_column || !filter_column->value ||
        !operator_text) return NULL;
    size_t length = 1u;
    if (!sql_add_size(&length, strlen("UPDATE \"")) ||
        !sql_add_size(&length, strlen(table->value)) ||
        !sql_add_size(&length, strlen("\" SET "))) return NULL;
    for (size_t i = 0; i < assignments->child_count; ++i) {
        const ASTNode *assignment = assignments->children[i];
        const ASTNode *column = assignment && assignment->child_count == 2u ?
            assignment->children[0] : NULL;
        if (!column || !column->value ||
            !sql_add_size(&length, strlen(column->value) + strlen("\" = ?\"")) ||
            (i && !sql_add_size(&length, 2u))) return NULL;
    }
    if (!sql_add_size(&length, strlen(" WHERE \"")) ||
        !sql_add_size(&length, strlen(filter_column->value)) ||
        !sql_add_size(&length, 2u) || !sql_add_size(&length, strlen(operator_text)) ||
        !sql_add_size(&length, 2u) || length > MILENA_SQL_PLAN_MAX_TEXT)
        return NULL;
    char *statement = malloc(length);
    if (!statement) return NULL;
    char *cursor = statement;
    const char *prefix = "UPDATE \"";
    size_t n = strlen(prefix); memcpy(cursor, prefix, n); cursor += n;
    n = strlen(table->value); memcpy(cursor, table->value, n); cursor += n;
    const char *set = "\" SET "; n = strlen(set); memcpy(cursor, set, n); cursor += n;
    for (size_t i = 0; i < assignments->child_count; ++i) {
        const ASTNode *column = assignments->children[i]->children[0];
        if (i) { memcpy(cursor, ", ", 2u); cursor += 2u; }
        *cursor++ = '"';
        n = strlen(column->value); memcpy(cursor, column->value, n); cursor += n;
        memcpy(cursor, "\" = ?", 5u); cursor += 5u;
    }
    const char *where = " WHERE \""; n = strlen(where); memcpy(cursor, where, n); cursor += n;
    n = strlen(filter_column->value); memcpy(cursor, filter_column->value, n); cursor += n;
    *cursor++ = '"'; *cursor++ = ' ';
    n = strlen(operator_text); memcpy(cursor, operator_text, n); cursor += n;
    memcpy(cursor, " ?", 2u); cursor += 2u;
    *cursor = '\0';
    return statement;
}

void milena_sql_execution_plan_destroy(MilenaSqlExecutionPlan *plan) {
    if (!plan) return;
    for (size_t i = 0; i < plan->operation_count; ++i) {
        free(plan->operations[i].parameters);
        free(plan->operations[i].projections);
        free(plan->operations[i].insert_columns);
        free(plan->operations[i].insert_values);
        free(plan->operations[i].update_assignments);
        free(plan->operations[i].owned_statement);
    }
    free(plan->operations);
    memset(plan, 0, sizeof(*plan));
}

MilenaStatus milena_sql_execution_plan_validate(const MilenaSqlExecutionPlan *plan,
                                                 MilenaError *error) {
    if (!plan || !plan->source || plan->source->type != AST_SQL_PROGRAM ||
        !plan->source->value || !plan->connection_path ||
        strcmp(plan->connection_path, plan->source->value) != 0 ||
        !plan->connection_path[0] || !plan->operations ||
        !plan->operation_count || !plan->source->children ||
        plan->operation_count != plan->source->child_count ||
        plan->operation_count > MILENA_SQL_PLAN_MAX_OPERATIONS ||
        !plan->max_rows || plan->max_rows > MILENA_SQL_PLAN_MAX_ROWS ||
        !plan->max_bytes || plan->max_bytes > MILENA_SQL_PLAN_MAX_BYTES ||
        !plan->timeout_ms || plan->timeout_ms > MILENA_SQL_PLAN_MAX_TIMEOUT_MS)
        return plan_error(error, MILENA_ERR_ARGUMENT,
                          "El plan SQL tiene conexión, operaciones o límites inválidos");
    MilenaStatus semantic = milena_sql_semantic_validate(plan->source, error);
    if (semantic != MILENA_OK) return semantic;
    bool transaction_active = false;
    for (size_t i = 0; i < plan->operation_count; ++i) {
        const MilenaSqlPlanOperation *op = &plan->operations[i];
        if (!op->source || op->source != plan->source->children[i] ||
            op->source->parent != plan->source ||
            op->parameter_count > MILENA_SQL_PLAN_MAX_PARAMETERS ||
            (op->parameter_count && !op->parameters))
            return plan_error(error, MILENA_ERR_PARSE,
                              "Operación del plan SQL inválida");
        if (op->kind == MILENA_SQL_PLAN_QUERY ||
            op->kind == MILENA_SQL_PLAN_EXECUTE) {
            ASTNodeType expected = op->kind == MILENA_SQL_PLAN_QUERY ?
                AST_SQL_QUERY : AST_SQL_EXECUTE;
            if (op->source->type != expected || !op->source->value ||
                !op->statement || strcmp(op->statement, op->source->value) != 0 ||
                !op->statement[0] || strlen(op->statement) > MILENA_SQL_PLAN_MAX_TEXT ||
                op->source->child_count != op->parameter_count ||
                (op->source->child_count && !op->source->children) ||
                op->owned_statement || op->projections || op->typed_table ||
                op->typed_schema || op->filter_column || op->filter_operator ||
                op->typed_parameter || op->insert_columns || op->insert_column_count ||
                op->insert_values || op->insert_value_count ||
                op->update_assignments || op->update_assignment_count)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Estructura de sentencia del plan SQL inválida");
            for (size_t j = 0; j < op->parameter_count; ++j)
                if (!sql_plan_parameter_matches_ast(&op->parameters[j],
                                                     op->source->children[j]))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Parámetro del plan SQL inválido o inconsistente");
        } else if (op->kind == MILENA_SQL_PLAN_TYPED_SELECT) {
            const ASTNode *select = op->source;
            const ASTNode *table = select->children[0];
            const ASTNode *projection = select->children[1];
            const ASTNode *filter = select->children[2];
            const ASTNode *filter_column = filter->children[0];
            const ASTNode *filter_operator = filter->children[1];
            const ASTNode *parameter = filter->children[2];
            const ASTNode *schema = sql_schema_before(plan->source, i, table->value);
            if (select->type != AST_SQL_TYPED_SELECT || !schema ||
                op->typed_table != table || op->typed_schema != schema ||
                op->filter_column != filter_column ||
                op->filter_operator != filter_operator ||
                op->typed_parameter != parameter ||
                op->insert_columns || op->insert_column_count ||
                op->insert_values || op->insert_value_count ||
                op->update_assignments || op->update_assignment_count ||
                filter_operator->sql_operator != AST_SQL_OPERATOR_EQUAL ||
                !op->projection_count ||
                op->projection_count != projection->child_count ||
                !op->projections || op->parameter_count != 1u ||
                !op->parameters || !op->owned_statement ||
                op->statement != op->owned_statement ||
                !sql_plan_parameter_matches_ast(&op->parameters[0], parameter))
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Plan de SELECT tipado incompleto o inconsistente");
            for (size_t p = 0; p < op->projection_count; ++p) {
                const ASTNode *field = projection->children[p];
                const ASTNode *column = sql_schema_find_column(schema, field->value);
                if (!column || op->projections[p].source != field ||
                    op->projections[p].schema_column != column ||
                    op->projections[p].name != field->value ||
                    op->projections[p].type != column->sql_type)
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Proyección del plan SQL no coincide con el esquema validado");
            }
            char *expected_statement = sql_build_typed_statement(table, projection,
                                                                  filter_column);
            bool statement_matches = expected_statement &&
                strcmp(expected_statement, op->statement) == 0;
            free(expected_statement);
            if (!statement_matches)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "SQL generado no coincide con el AST tipado validado");
        } else if (op->kind == MILENA_SQL_PLAN_TYPED_INSERT) {
            const ASTNode *insert = op->source;
            const ASTNode *table = insert->children[0];
            const ASTNode *columns = insert->children[1];
            const ASTNode *values = insert->children[2];
            const ASTNode *schema = sql_schema_before(plan->source, i, table->value);
            if (insert->type != AST_SQL_TYPED_INSERT || !schema ||
                op->typed_table != table || op->typed_schema != schema ||
                op->projection_count || op->projections || op->filter_column ||
                op->filter_operator || op->typed_parameter ||
                op->update_assignments || op->update_assignment_count ||
                !op->insert_column_count ||
                op->insert_column_count != columns->child_count ||
                !op->insert_columns ||
                op->insert_value_count != values->child_count ||
                !op->insert_values ||
                op->parameter_count != values->child_count || !op->parameters ||
                !op->owned_statement || op->statement != op->owned_statement)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Plan de INSERT tipado incompleto o inconsistente");
            for (size_t c = 0; c < op->insert_column_count; ++c) {
                const ASTNode *field = columns->children[c];
                const ASTNode *value = values->children[c];
                const ASTNode *column = sql_schema_find_column(schema, field->value);
                const MilenaSqlTypedInsertColumn *planned_column =
                    &op->insert_columns[c];
                const MilenaSqlTypedInsertValue *planned_value =
                    &op->insert_values[c];
                if (!column || planned_column->source != field ||
                    planned_column->schema_column != column ||
                    planned_column->name != field->value ||
                    planned_column->type != column->sql_type ||
                    planned_value->source != value ||
                    planned_value->schema_column != column ||
                    planned_value->type != column->sql_type ||
                    !sql_plan_parameter_matches_ast(&op->parameters[c], value))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Columnas o valores del plan INSERT no coinciden con el esquema tipado");
            }
            char *expected_statement = sql_build_typed_insert_statement(table, columns);
            bool statement_matches = expected_statement &&
                strcmp(expected_statement, op->statement) == 0;
            free(expected_statement);
            if (!statement_matches)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "SQL generado no coincide con el AST INSERT tipado");
        } else if (op->kind == MILENA_SQL_PLAN_TYPED_UPDATE) {
            const ASTNode *update = op->source;
            const ASTNode *table = update->children[0];
            const ASTNode *assignments = update->children[1];
            const ASTNode *filter = update->children[2];
            const ASTNode *filter_column = filter->children[0];
            const ASTNode *filter_operator = filter->children[1];
            const ASTNode *parameter = filter->children[2];
            const ASTNode *schema = sql_schema_before(plan->source, i, table->value);
            if (update->type != AST_SQL_TYPED_UPDATE || !schema ||
                op->typed_table != table || op->typed_schema != schema ||
                op->filter_column != filter_column ||
                op->filter_operator != filter_operator ||
                op->typed_parameter != parameter ||
                op->projection_count || op->projections ||
                op->insert_columns || op->insert_column_count ||
                op->insert_values || op->insert_value_count ||
                !op->update_assignment_count ||
                op->update_assignment_count != assignments->child_count ||
                !op->update_assignments ||
                op->parameter_count != op->update_assignment_count + 1u ||
                !op->parameters || !op->owned_statement ||
                op->statement != op->owned_statement)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Plan de UPDATE tipado incompleto o inconsistente");
            for (size_t a = 0; a < op->update_assignment_count; ++a) {
                const ASTNode *assignment = assignments->children[a];
                const ASTNode *field = assignment->children[0];
                const ASTNode *value = assignment->children[1];
                const ASTNode *column = sql_schema_find_column(schema, field->value);
                const MilenaSqlTypedUpdateAssignment *planned =
                    &op->update_assignments[a];
                if (!column || planned->source != assignment ||
                    planned->column_source != field ||
                    planned->value_source != value ||
                    planned->schema_column != column ||
                    planned->name != field->value ||
                    planned->type != column->sql_type ||
                    !sql_plan_parameter_matches_ast(&op->parameters[a], value))
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Asignación del plan UPDATE no coincide con el esquema tipado");
            }
            if (!sql_plan_parameter_matches_ast(
                    &op->parameters[op->update_assignment_count], parameter))
                return plan_error(error, MILENA_ERR_TYPE,
                                  "Parámetro del filtro UPDATE no coincide con el AST tipado");
            char *expected_statement = sql_build_typed_update_statement(
                table, assignments, filter_column, filter_operator);
            bool statement_matches = expected_statement &&
                strcmp(expected_statement, op->statement) == 0;
            free(expected_statement);
            if (!statement_matches)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "SQL generado no coincide con el AST UPDATE tipado");
        } else if (op->kind == MILENA_SQL_PLAN_SCHEMA) {
            if (op->source->type != AST_SQL_TABLE_SCHEMA || op->parameter_count ||
                op->parameters || op->statement || op->owned_statement ||
                op->projections || op->typed_table || op->typed_schema ||
                op->filter_column || op->filter_operator || op->typed_parameter ||
                op->insert_columns || op->insert_column_count ||
                op->insert_values || op->insert_value_count ||
                op->update_assignments || op->update_assignment_count)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Declaración de esquema inválida dentro del plan SQL");
        } else if (op->kind == MILENA_SQL_PLAN_BEGIN ||
                   op->kind == MILENA_SQL_PLAN_COMMIT ||
                   op->kind == MILENA_SQL_PLAN_ROLLBACK) {
            if (op->source->child_count || op->parameter_count || op->parameters ||
                op->statement || op->owned_statement || op->projections ||
                op->typed_table || op->typed_schema || op->filter_column ||
                op->filter_operator || op->typed_parameter ||
                op->insert_columns || op->insert_column_count ||
                op->insert_values || op->insert_value_count ||
                op->update_assignments || op->update_assignment_count)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Operación transaccional del plan SQL inválida");
            if (op->kind == MILENA_SQL_PLAN_BEGIN) {
                if (transaction_active)
                    return plan_error(error, MILENA_ERR_DATA,
                                      "No se puede iniciar una transacción SQL dentro de otra");
                transaction_active = true;
            } else {
                if (!transaction_active)
                    return plan_error(error, MILENA_ERR_DATA,
                                      "La operación de cierre SQL no tiene una transacción activa");
                transaction_active = false;
            }
            ASTNodeType expected = op->kind == MILENA_SQL_PLAN_BEGIN ? AST_SQL_BEGIN :
                op->kind == MILENA_SQL_PLAN_COMMIT ? AST_SQL_COMMIT : AST_SQL_ROLLBACK;
            if (op->source->type != expected)
                return plan_error(error, MILENA_ERR_PARSE,
                                  "Operación transaccional del plan SQL no coincide con su AST");
        } else return plan_error(error, MILENA_ERR_PARSE,
                                 "Tipo de operación del plan SQL inválido");
    }
    if (transaction_active)
        return plan_error(error, MILENA_ERR_DATA,
                          "La transacción SQL del plan está incompleta");
    return MILENA_OK;
}

MilenaStatus milena_sql_execution_plan_build(const ASTNode *program,
        MilenaSqlExecutionPlan *plan, MilenaError *error) {
    if (error) milena_error_clear(error);
    MilenaStatus semantic = milena_sql_semantic_validate(program, error);
    if (semantic != MILENA_OK) return semantic;
    if (!plan) return plan_error(error, MILENA_ERR_ARGUMENT,
                                 "El plan SQL requiere salida válida");
    memset(plan, 0, sizeof(*plan));
    plan->source = program;
    plan->connection_path = program->value;
    plan->max_rows = program->sql_max_rows ? program->sql_max_rows :
                     MILENA_SQL_PLAN_DEFAULT_ROWS;
    plan->max_bytes = program->sql_max_bytes ? program->sql_max_bytes :
                      MILENA_SQL_PLAN_DEFAULT_BYTES;
    plan->timeout_ms = (unsigned)(program->sql_timeout_ms ? program->sql_timeout_ms :
                                  MILENA_SQL_PLAN_DEFAULT_TIMEOUT_MS);
    plan->explicit_limits = program->sql_limits_explicit;
    plan->operation_count = program->child_count;
    plan->operations = calloc(plan->operation_count, sizeof(*plan->operations));
    if (!plan->operations)
        return plan_error(error, MILENA_ERR_MEMORY,
                          "Sin memoria para operaciones del plan SQL");
    for (size_t i = 0; i < plan->operation_count; ++i) {
        const ASTNode *node = program->children[i];
        MilenaSqlPlanOperation *op = &plan->operations[i];
        op->source = node;
        if (node->type == AST_SQL_TABLE_SCHEMA) {
            op->kind = MILENA_SQL_PLAN_SCHEMA;
        } else if (node->type == AST_SQL_TYPED_SELECT) {
            const ASTNode *table = node->children[0];
            const ASTNode *projection = node->children[1];
            const ASTNode *filter = node->children[2];
            const ASTNode *filter_column = filter->children[0];
            const ASTNode *filter_operator = filter->children[1];
            const ASTNode *parameter = filter->children[2];
            const ASTNode *schema = sql_schema_before(program, i, table->value);
            op->kind = MILENA_SQL_PLAN_TYPED_SELECT;
            op->typed_table = table;
            op->typed_schema = schema;
            op->filter_column = filter_column;
            op->filter_operator = filter_operator;
            op->typed_parameter = parameter;
            op->projection_count = projection->child_count;
            op->projections = calloc(op->projection_count, sizeof(*op->projections));
            op->parameters = calloc(1u, sizeof(*op->parameters));
            if (!op->projections || !op->parameters ||
                !sql_plan_parameter_from_ast(parameter, &op->parameters[0])) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_MEMORY,
                                  "No se pudo construir el SELECT tipado SQL");
            }
            op->parameter_count = 1u;
            for (size_t p = 0; p < op->projection_count; ++p) {
                const ASTNode *field = projection->children[p];
                const ASTNode *column = sql_schema_find_column(schema, field->value);
                op->projections[p] = (MilenaSqlTypedProjection){
                    field, column, field->value, column->sql_type};
            }
            op->owned_statement = sql_build_typed_statement(table, projection,
                                                             filter_column);
            if (!op->owned_statement) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_MEMORY,
                                  "No se pudo generar SQL para la consulta tipada");
            }
            op->statement = op->owned_statement;
        } else if (node->type == AST_SQL_TYPED_INSERT) {
            const ASTNode *table = node->children[0];
            const ASTNode *columns = node->children[1];
            const ASTNode *values = node->children[2];
            const ASTNode *schema = sql_schema_before(program, i, table->value);
            op->kind = MILENA_SQL_PLAN_TYPED_INSERT;
            op->typed_table = table;
            op->typed_schema = schema;
            op->insert_column_count = columns->child_count;
            op->insert_value_count = values->child_count;
            op->parameter_count = values->child_count;
            op->insert_columns = calloc(op->insert_column_count,
                                        sizeof(*op->insert_columns));
            op->insert_values = calloc(op->insert_value_count,
                                       sizeof(*op->insert_values));
            op->parameters = calloc(op->parameter_count,
                                    sizeof(*op->parameters));
            if (!op->insert_columns || !op->insert_values || !op->parameters) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_MEMORY,
                                  "Sin memoria para el plan INSERT tipado");
            }
            for (size_t c = 0; c < op->insert_column_count; ++c) {
                const ASTNode *field = columns->children[c];
                const ASTNode *value = values->children[c];
                const ASTNode *column = sql_schema_find_column(schema, field->value);
                op->insert_columns[c] = (MilenaSqlTypedInsertColumn){
                    field, column, field->value, column->sql_type};
                op->insert_values[c] = (MilenaSqlTypedInsertValue){
                    value, column, column->sql_type};
                if (!sql_plan_parameter_from_ast(value, &op->parameters[c])) {
                    milena_sql_execution_plan_destroy(plan);
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Literal del plan INSERT tipado inválido");
                }
            }
            op->owned_statement = sql_build_typed_insert_statement(table, columns);
            if (!op->owned_statement) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_MEMORY,
                                  "No se pudo generar SQL para el INSERT tipado");
            }
            op->statement = op->owned_statement;
        } else if (node->type == AST_SQL_TYPED_UPDATE) {
            const ASTNode *table = node->children[0];
            const ASTNode *assignments = node->children[1];
            const ASTNode *filter = node->children[2];
            const ASTNode *filter_column = filter->children[0];
            const ASTNode *filter_operator = filter->children[1];
            const ASTNode *parameter = filter->children[2];
            const ASTNode *schema = sql_schema_before(program, i, table->value);
            op->kind = MILENA_SQL_PLAN_TYPED_UPDATE;
            op->typed_table = table;
            op->typed_schema = schema;
            op->filter_column = filter_column;
            op->filter_operator = filter_operator;
            op->typed_parameter = parameter;
            op->update_assignment_count = assignments->child_count;
            op->parameter_count = assignments->child_count + 1u;
            op->update_assignments = calloc(op->update_assignment_count,
                                             sizeof(*op->update_assignments));
            op->parameters = calloc(op->parameter_count, sizeof(*op->parameters));
            if (!op->update_assignments || !op->parameters) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_MEMORY,
                                  "Sin memoria para el plan UPDATE tipado");
            }
            for (size_t a = 0; a < op->update_assignment_count; ++a) {
                const ASTNode *assignment = assignments->children[a];
                const ASTNode *field = assignment->children[0];
                const ASTNode *value = assignment->children[1];
                const ASTNode *column = sql_schema_find_column(schema, field->value);
                op->update_assignments[a] = (MilenaSqlTypedUpdateAssignment){
                    assignment, field, value, column, field->value, column->sql_type};
                if (!sql_plan_parameter_from_ast(value, &op->parameters[a])) {
                    milena_sql_execution_plan_destroy(plan);
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Literal de asignación del plan UPDATE inválido");
                }
            }
            if (!sql_plan_parameter_from_ast(parameter,
                    &op->parameters[op->update_assignment_count])) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_TYPE,
                                  "Literal del filtro del plan UPDATE inválido");
            }
            op->owned_statement = sql_build_typed_update_statement(
                table, assignments, filter_column, filter_operator);
            if (!op->owned_statement) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_MEMORY,
                                  "No se pudo generar SQL para el UPDATE tipado");
            }
            op->statement = op->owned_statement;
        } else if (node->type == AST_SQL_QUERY || node->type == AST_SQL_EXECUTE) {
            op->kind = node->type == AST_SQL_QUERY ? MILENA_SQL_PLAN_QUERY :
                       MILENA_SQL_PLAN_EXECUTE;
            op->statement = node->value;
            op->parameter_count = node->child_count;
            if (op->parameter_count > MILENA_SQL_PLAN_MAX_PARAMETERS) {
                milena_sql_execution_plan_destroy(plan);
                return plan_error(error, MILENA_ERR_ARGUMENT,
                                  "Demasiados parámetros en plan SQL");
            }
            if (op->parameter_count) {
                op->parameters = calloc(op->parameter_count,
                                        sizeof(*op->parameters));
                if (!op->parameters) {
                    milena_sql_execution_plan_destroy(plan);
                    return plan_error(error, MILENA_ERR_MEMORY,
                                      "Sin memoria para parámetros tipados SQL");
                }
            }
            for (size_t p = 0; p < op->parameter_count; ++p) {
                if (!sql_plan_parameter_from_ast(node->children[p],
                                                 &op->parameters[p])) {
                    milena_sql_execution_plan_destroy(plan);
                    return plan_error(error, MILENA_ERR_TYPE,
                                      "Literal de parámetro del plan SQL inválido");
                }
            }
        } else if (node->type == AST_SQL_BEGIN) {
            op->kind = MILENA_SQL_PLAN_BEGIN;
        } else if (node->type == AST_SQL_COMMIT) {
            op->kind = MILENA_SQL_PLAN_COMMIT;
        } else if (node->type == AST_SQL_ROLLBACK) {
            op->kind = MILENA_SQL_PLAN_ROLLBACK;
        } else {
            milena_sql_execution_plan_destroy(plan);
            return plan_error(error, MILENA_ERR_UNSUPPORTED,
                              "Operación AST no soportada por el plan SQL");
        }
    }
    MilenaStatus status = milena_sql_execution_plan_validate(plan, error);
    if (status != MILENA_OK) milena_sql_execution_plan_destroy(plan);
    return status;
}
