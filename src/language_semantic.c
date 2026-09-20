#include "language_semantic.h"
#include <string.h>

static bool known_sst_command(const char *name) {
    static const char *const commands[] = {
        "perfil_avanzado", "histograma", "normalidad", "tasa", "poisson",
        "correlacion", "wilcoxon", "chi_cuadrado", "riesgo", "modelo_sst"
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

static MilenaStatus validate_node(const ASTNode *node, MilenaError *error) {
    if (!node) return semantic_error(node, error, "Nodo AST nulo");
    if (node->type == AST_COMANDO_SST) {
        if (!known_sst_command(node->type_name))
            return semantic_error(node, error, "Comando SST no registrado en el runtime común");
        if (!node->value || !node->value[0])
            return semantic_error(node, error, "Comando SST sin argumento");
    }
    switch (node->type) {
        case AST_LLAMADA_CARGAR:
        case AST_AGRUPACION_POR:
        case AST_RESUMEN_METRICA:
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

MilenaStatus milena_validate_ast(const ASTNode *program, MilenaError *error) {
    if (!program || program->type != AST_PROGRAMA)
        return semantic_error(program, error, "El programa no tiene una raíz AST válida");
    return validate_node(program, error);
}
