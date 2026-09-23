#include "query_plan.h"
#include "stream.h"

#include <string.h>
#include <math.h>

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
