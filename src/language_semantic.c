#include "language_semantic.h"
#include "table.h"
#include "grouped_aggregate.h"
#include <string.h>

static bool known_sst_command(const char *name) {
    static const char *const commands[] = {
        "perfil_avanzado", "histograma", "normalidad", "tasa", "poisson",
        "correlacion", "wilcoxon", "chi_cuadrado", "riesgo", "modelo_sst",
        "interes_simple"
    };
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strcmp(name, commands[i]) == 0) return true;
    return false;
}

static MilenaStatus semantic_error(const ASTNode *node, MilenaError *error,
                                   const char *message) {
    milena_error_set(error, MILENA_ERR_PARSE, node ? (size_t)node->line : 0,
                     node ? (size_t)node->column : 0, 0, message);
    return MILENA_ERR_PARSE;
}


static size_t sst_argument_count(const char *value, bool *valid) {
    size_t count = 1;
    *valid = value && value[0];
    if (!*valid) return 0;
    bool field_has_text = false;
    for (const char *p = value;; p++) {
        if (*p == ',' || *p == '\0') {
            if (!field_has_text) *valid = false;
            if (*p == '\0') break;
            count++;
            field_has_text = false;
        } else if (*p != ' ' && *p != '\t') {
            field_has_text = true;
        }
    }
    return count;
}

static MilenaStatus validate_sst_arguments(const ASTNode *node,
                                           MilenaError *error) {
    bool valid = false;
    size_t count = sst_argument_count(node->value, &valid);
    size_t expected = 1;
    if (strcmp(node->type_name, "tasa") == 0 ||
        strcmp(node->type_name, "poisson") == 0) expected = 3;
    else if (strcmp(node->type_name, "correlacion") == 0 ||
             strcmp(node->type_name, "wilcoxon") == 0 ||
             strcmp(node->type_name, "chi_cuadrado") == 0) expected = 2;
    else if (strcmp(node->type_name, "riesgo") == 0) expected = 4;
    else if (strcmp(node->type_name, "modelo_sst") == 0 ||
             strcmp(node->type_name, "interes_simple") == 0) expected = 3;
    if (!valid || count != expected)
        return semantic_error(node, error, "Cantidad de argumentos SST inválida");
    return MILENA_OK;
}

static bool known_stream_operation(ASTStreamOperation operation) {
    return operation == AST_STREAM_OPERATION_SUM ||
           operation == AST_STREAM_OPERATION_MEAN ||
           operation == AST_STREAM_OPERATION_MIN ||
           operation == AST_STREAM_OPERATION_MAX ||
           operation == AST_STREAM_OPERATION_COUNT ||
           operation == AST_STREAM_OPERATION_VARIANCE ||
           operation == AST_STREAM_OPERATION_STDDEV;
}

static const ASTNode *stream_find_column_declaration(const ASTNode *analysis,
                                                       const char *name) {
    if (!analysis || !name) return NULL;
    for (size_t i = 0; i < analysis->child_count; ++i) {
        const ASTNode *child = analysis->children[i];
        if (child && child->type == AST_DECLARACION_VARIABLE &&
            child->value && strcmp(child->value, name) == 0)
            return child;
    }
    return NULL;
}

static const ASTNode *stream_find_load(const ASTNode *analysis) {
    if (!analysis) return NULL;
    for (size_t i = 0; i < analysis->child_count; ++i) {
        const ASTNode *child = analysis->children[i];
        if (child && child->type == AST_LLAMADA_CARGAR && child->type_name &&
            strcmp(child->type_name, "flujo") == 0) return child;
    }
    return NULL;
}

static bool grouped_spill_operation(ASTStreamOperation operation) {
    return operation == AST_STREAM_OPERATION_SUM ||
           operation == AST_STREAM_OPERATION_MEAN ||
           operation == AST_STREAM_OPERATION_MIN ||
           operation == AST_STREAM_OPERATION_MAX ||
           operation == AST_STREAM_OPERATION_COUNT;
}

static MilenaStatus validate_node(const ASTNode *node, MilenaError *error) {
    if (!node) return semantic_error(node, error, "Nodo AST nulo");
    if (node->type == AST_COMANDO_SST) {
        if (!known_sst_command(node->type_name))
            return semantic_error(node, error, "Comando SST no registrado en el runtime común");
        if (!node->value || !node->value[0])
            return semantic_error(node, error, "Comando SST sin argumento");
        MilenaStatus argument_status = validate_sst_arguments(node, error);
        if (argument_status != MILENA_OK) return argument_status;
    }
    switch (node->type) {
        case AST_LLAMADA_CARGAR:
            if (node->type_name && strcmp(node->type_name, "flujo") == 0 &&
                node->number_value != 0.0 &&
                (node->number_value < 1.0 || node->number_value > 1000000.0 ||
                 floor(node->number_value) != node->number_value))
                return semantic_error(node, error, "Tamaño de lote de flujo inválido");
            if (node->stream_record_limit > 64u * 1024u * 1024u ||
                (node->stream_record_limit != 0 &&
                 node->stream_record_limit < 4096u))
                return semantic_error(node, error, "Límite de registro de flujo inválido");
            if (node->stream_column_limit > 4096u ||
                node->stream_group_limit > 100000u ||
                node->stream_row_limit > 1000000000u ||
                (node->stream_time_limit_ms != 0.0 &&
                 (!isfinite(node->stream_time_limit_ms) ||
                  node->stream_time_limit_ms < 1.0 ||
                  node->stream_time_limit_ms > 3600000.0)))
                return semantic_error(node, error,
                    "Límite de columnas, grupos, filas o tiempo de flujo inválido");
            if (!node->value || !node->value[0])
                return semantic_error(node, error, "Carga de dataset sin archivo");
            break;
        case AST_BLOQUE_UNIR: {
            size_t right_count = 0, key_count = 0;
            for (size_t i = 0; i < node->child_count; ++i) {
                const ASTNode *child = node->children[i];
                if (!child) return semantic_error(node, error,
                    "Join con nodo AST nulo");
                if (child->type == AST_COMANDO_DERECHA) right_count++;
                else if (child->type == AST_COMANDO_CLAVE) key_count++;
                else return semantic_error(child, error,
                    "El join solo admite #derecha, #clave y #limites");
            }
            if (right_count != 1 || key_count != 1 ||
                node->join_memory_budget_bytes < 4096u ||
                node->join_memory_budget_bytes >
                    MILENA_TABLE_JOIN_HARD_MEMORY_BYTES ||
                node->join_max_output_rows == 0 ||
                node->join_max_output_rows >
                    MILENA_TABLE_JOIN_HARD_MAX_OUTPUT_ROWS)
                return semantic_error(node, error,
                    "Join requiere origen, clave y límites de memoria/salida válidos");
            break;
        }
        case AST_BLOQUE_AGRUPAR:
            if (node->type_name && strcmp(node->type_name, "flujo") == 0) {
                size_t keys = 0, summaries = 0, policies = 0;
                const ASTNode *key = NULL, *summary = NULL, *policy = NULL;
                for (size_t i = 0; i < node->child_count; i++) {
                    const ASTNode *child = node->children[i];
                    if (!child) return semantic_error(node, error,
                        "Agrupación de flujo con nodo AST nulo");
                    if (child->type == AST_AGRUPACION_POR) { keys++; key = child; }
                    else if (child->type == AST_BLOQUE_RESUMIR) {
                        summaries++;
                        summary = child;
                    } else if (child->type == AST_AGRUPACION_SPILL) {
                        policies++;
                        policy = child;
                    } else return semantic_error(node, error,
                        "La agrupación de flujo solo admite clave, spill opcional y resumen tipado");
                }
                if (keys != 1 || summaries != 1 || policies > 1 || !summary ||
                    summary->child_count == 0 || summary->child_count > 64)
                    return semantic_error(node, error,
                        "La agrupación de flujo requiere una clave, un resumen tipado y como máximo una política spill");
                for (size_t i = 0; i < summary->child_count; i++) {
                    const ASTNode *metric = summary->children[i];
                    if (!metric || metric->type != AST_RESUMEN_METRICA ||
                        !metric->value || !metric->value[0] ||
                        !known_stream_operation(metric->stream_operation) ||
                        metric->stream_operation == AST_STREAM_OPERATION_NONE)
                        return semantic_error(metric, error,
                            "La métrica agrupada debe usar una operación de flujo tipada");
                }
                if (policies) {
                    if (summary->child_count != 1)
                        return semantic_error(summary, error,
                            "#spill de flujo admite una sola clave y una sola métrica por operación");
                    const ASTNode *metric = summary->children[0];
                    if (!grouped_spill_operation(metric->stream_operation))
                        return semantic_error(metric, error,
                            "#spill de flujo solo admite suma, media, minimo, maximo o conteo");
                    const ASTNode *key_decl = stream_find_column_declaration(
                        node->parent, key->value);
                    const ASTNode *metric_decl = stream_find_column_declaration(
                        node->parent, metric->value);
                    if (!key_decl || !key_decl->type_name ||
                        strcmp(key_decl->type_name, "texto") != 0)
                        return semantic_error(key, error,
                            "La clave de #spill en flujo debe declararse variable <columna> texto");
                    if (!metric_decl || !metric_decl->type_name)
                        return semantic_error(metric, error,
                            "La métrica de #spill en flujo debe tener declaración tipada");
                    if (metric->stream_operation != AST_STREAM_OPERATION_COUNT &&
                        strcmp(metric_decl->type_name, "numerica") != 0)
                        return semantic_error(metric, error,
                            "Las métricas numéricas de #spill requieren variable <columna> numerica (FLOAT64)");
                    if (strcmp(key->value, metric->value) == 0 &&
                        metric->stream_operation != AST_STREAM_OPERATION_COUNT)
                        return semantic_error(metric, error,
                            "La clave textual de #spill no puede reutilizarse como métrica numérica");
                    const ASTNode *stream_load = stream_find_load(node->parent);
                    if (!stream_load || stream_load->stream_row_limit == 0 ||
                        stream_load->stream_time_limit_ms <= 0.0)
                        return semantic_error(node, error,
                            "#spill de flujo exige límites explícitos de filas y tiempo en datos desde");
                    if (!policy || !policy->value || !policy->value[0] ||
                        strlen(policy->value) > 220u ||
                        policy->group_memory_budget_bytes < 4096u ||
                        policy->group_memory_budget_bytes > 536870912u ||
                        policy->group_spill_quota_bytes == 0 ||
                        policy->group_spill_quota_bytes > 4294967296u ||
                        policy->group_max_key_bytes < 2u ||
                        policy->group_max_key_bytes > 1048576u ||
                        policy->group_max_output_groups == 0 ||
                        policy->group_max_output_groups > 1000000u ||
                        policy->group_max_output_bytes > 1073741824u ||
                        policy->group_max_runs == 0 ||
                        policy->group_max_runs > MILENA_GROUPED_HARD_MAX_RUNS)
                        return semantic_error(policy, error,
                            "Política #spill de flujo fuera de los límites duros de recursos");
                }
            } else {
                size_t policies = 0, summaries = 0, keys = 0;
                for (size_t i = 0; i < node->child_count; i++) {
                    const ASTNode *child = node->children[i];
                    if (!child) return semantic_error(node, error,
                        "Agrupación con nodo AST nulo");
                    if (child->type == AST_AGRUPACION_SPILL) {
                        policies++;
                        if (child->group_output_limit_explicit)
                            return semantic_error(child, error,
                                "El límite de bytes de reporte solo se admite en #spill de flujo");
                    } else if (child->type == AST_AGRUPACION_POR) keys++;
                    else if (child->type == AST_RESUMEN_METRICA) summaries++;
                }
                if (keys != 1)
                    return semantic_error(node, error,
                        "#agrupar requiere exactamente una clave #por");
                if (policies > 1)
                    return semantic_error(node, error,
                        "#agrupar admite como máximo una política #spill");
                if (policies && summaries != 1)
                    return semantic_error(node, error,
                        "La política #spill de #agrupar requiere exactamente una métrica");
            }
            break;
        case AST_AGRUPACION_POR:
        case AST_AGRUPACION_SPILL:
        case AST_RESUMEN_METRICA:
            if (node->type == AST_AGRUPACION_SPILL &&
                (!node->value || !node->value[0] || strlen(node->value) > 220u ||
                 node->group_memory_budget_bytes < 4096u ||
                 node->group_memory_budget_bytes > 536870912u ||
                 node->group_spill_quota_bytes == 0 ||
                 node->group_spill_quota_bytes > 4294967296u ||
                 node->group_max_key_bytes < 2u ||
                 node->group_max_key_bytes > 1048576u ||
                 node->group_max_output_groups == 0 ||
                 node->group_max_output_groups > 1000000u ||
                 node->group_max_output_bytes > 1073741824u ||
                 node->group_max_runs == 0 ||
                 node->group_max_runs > MILENA_GROUPED_HARD_MAX_RUNS))
                return semantic_error(node, error,
                    "Política #spill fuera de sus límites duros de recursos");
            if (node->type == AST_RESUMEN_METRICA &&
                node->stream_operation != AST_STREAM_OPERATION_NONE &&
                !known_stream_operation(node->stream_operation))
                return semantic_error(node, error,
                    "Operación de flujo no registrada en el AST canónico");
            if (!node->value || !node->value[0])
                return semantic_error(node, error, "Operación AST sin argumento");
            break;
        case AST_COMANDO_COLUMNAS:
        case AST_COMANDO_DERECHA:
        case AST_COMANDO_CLAVE:
            if (!node->value || !node->value[0])
                return semantic_error(node, error, "Operación AST sin argumento");
            break;
        default:
            break;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        MilenaStatus status = validate_node(node->children[i], error);
        if (status != MILENA_OK) return status;
    }
    return MILENA_OK;
}


static bool table_column_kind(const MilenaTable *table, const char *name,
                              MilenaColumnType *kind, int *index) {
    int found = milena_table_column_index(table, name);
    if (found < 0) return false;
    if (index) *index = found;
    if (kind) *kind = milena_table_column(table, (size_t)found)->type;
    return true;
}

static MilenaStatus validate_sst_column(const ASTNode *node,
                                        const MilenaTable *table,
                                        const char *name, bool numeric,
                                        bool categorical, MilenaError *error) {
    MilenaColumnType kind;
    if (!table_column_kind(table, name, &kind, NULL))
        return semantic_error(node, error, "La columna SST no existe en MilenaTable");
    if ((numeric && kind != MILENA_COLUMN_ARRAY) ||
        (categorical && kind != MILENA_COLUMN_STRING && kind != MILENA_COLUMN_CATEGORICAL))
        return semantic_error(node, error, "Tipo de columna incompatible con SST");
    return MILENA_OK;
}

static MilenaStatus validate_sst_table_node(const ASTNode *node,
                                            const MilenaTable *table,
                                            MilenaError *error) {
    if (!node) return MILENA_OK;
    if (node->type == AST_COMANDO_SST && node->value && node->type_name) {
        char spec[512];
        strncpy(spec, node->value, sizeof(spec) - 1);
        spec[sizeof(spec) - 1] = '\0';
        char *a = strtok(spec, ",");
        char *b = strtok(NULL, ",");
        char *c = strtok(NULL, ",");
        char *d = strtok(NULL, ",");
        MilenaStatus status = MILENA_OK;
        if (strcmp(node->type_name, "perfil_avanzado") == 0 ||
            strcmp(node->type_name, "histograma") == 0 ||
            strcmp(node->type_name, "normalidad") == 0) {
            status = validate_sst_column(node, table, a, true, false, error);
        } else if (strcmp(node->type_name, "tasa") == 0 ||
                   strcmp(node->type_name, "poisson") == 0) {
            status = validate_sst_column(node, table, a, false, false, error);
            if (status == MILENA_OK) status = validate_sst_column(node, table, b, true, false, error);
        } else if (strcmp(node->type_name, "correlacion") == 0 ||
                   strcmp(node->type_name, "wilcoxon") == 0) {
            status = validate_sst_column(node, table, a, true, false, error);
            if (status == MILENA_OK) status = validate_sst_column(node, table, b, true, false, error);
        } else if (strcmp(node->type_name, "chi_cuadrado") == 0 ||
                   strcmp(node->type_name, "riesgo") == 0) {
            status = validate_sst_column(node, table, a, false, true, error);
            if (status == MILENA_OK) status = validate_sst_column(node, table, b, false, true, error);
            (void)c; (void)d;
        } else if (strcmp(node->type_name, "interes_simple") == 0) {
            status = validate_sst_column(node, table, a, true, false, error);
            if (status == MILENA_OK) status = validate_sst_column(node, table, b, true, false, error);
        } else if (strcmp(node->type_name, "modelo_sst") == 0) {
            status = validate_sst_column(node, table, a, false, true, error);
            if (status == MILENA_OK) status = validate_sst_column(node, table, b, true, false, error);
            if (status == MILENA_OK) status = validate_sst_column(node, table, c, false, true, error);
        }
        if (status != MILENA_OK) return status;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        MilenaStatus status = validate_sst_table_node(node->children[i], table, error);
        if (status != MILENA_OK) return status;
    }
    return MILENA_OK;
}

MilenaStatus milena_validate_sst_table(const ASTNode *analysis,
                                       const MilenaTable *table,
                                       MilenaError *error) {
    if (!analysis || !table)
        return semantic_error(analysis, error, "No se puede validar SST sin análisis y tabla");
    return validate_sst_table_node(analysis, table, error);
}

MilenaStatus milena_validate_ast(const ASTNode *program, MilenaError *error) {
    if (!program || program->type != AST_PROGRAMA)
        return semantic_error(program, error, "El programa no tiene una raíz AST válida");
    return validate_node(program, error);
}
