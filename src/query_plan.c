#include "query_plan.h"

#include <string.h>

static MilenaStatus plan_error(MilenaError *error, MilenaStatus code,
                               const char *message) {
    milena_error_set(error, code, 0, 0, 0, message);
    return code;
}

static bool supported_analysis_child(ASTNodeType type) {
    return type == AST_LLAMADA_CARGAR ||
           type == AST_BLOQUE_RESUMIR ||
           type == AST_BLOQUE_AGRUPAR ||
           type == AST_BLOQUE_EXPORTAR ||
           type == AST_DECLARACION_VARIABLE ||
           type == AST_DECLARACION_ENTRADA ||
           type == AST_DECLARACION_SALIDA;
}

static bool valid_summary(const ASTNode *summary, bool allow_legacy) {
    if (!summary || summary->type != AST_BLOQUE_RESUMIR ||
        summary->child_count == 0 || summary->child_count > 64) return false;
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
                if (plan->group_key || !child->value || !child->value[0])
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "El plan agrupado requiere una única clave válida");
                plan->group_key = child;
            } else if (child->type == AST_BLOQUE_RESUMIR) {
                if (plan->group_summary)
                    return plan_error(error, MILENA_ERR_PARSE,
                                      "El plan agrupado requiere un único resumen");
                plan->group_summary = child;
            } else {
                return plan_error(error, MILENA_ERR_UNSUPPORTED,
                                  "El plan agrupado contiene un operador no soportado");
            }
        }
        if (!plan->group_key || !valid_summary(plan->group_summary, false))
            return plan_error(error, MILENA_ERR_PARSE,
                              "El plan agrupado requiere clave y métricas tipadas");
        plan->summary = plan->group_summary;
        plan->physical_operator = MILENA_PHYSICAL_CSV_STREAM_GROUPED;
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
    plan->logical_operators[2] = MILENA_LOGICAL_JSON_REPORT;
    plan->logical_operator_count = 3;
    return MILENA_OK;
}
