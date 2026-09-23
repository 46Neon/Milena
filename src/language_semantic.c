#include "language_semantic.h"
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
            if (node->stream_column_limit > 4096u)
                return semantic_error(node, error, "Límite de columnas de flujo inválido");
            if (!node->value || !node->value[0])
                return semantic_error(node, error, "Carga de dataset sin archivo");
            break;
        case AST_AGRUPACION_POR:
        case AST_RESUMEN_METRICA:
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
