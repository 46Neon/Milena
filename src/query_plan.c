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
