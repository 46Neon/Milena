#include "ast.h"

ASTNode *ast_create(ASTNodeType type) {
    ASTNode *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->type = type;
    node->axis = -1;
    return node;
}

ASTNode *ast_create_leaf(ASTNodeType type, const char *value) {
    ASTNode *node = ast_create(type);
    if (!node) return NULL;
    if (value) {
        node->value = milena_strdup(value);
        if (!node->value) {
            free(node);
            return NULL;
        }
    }
    return node;
}

ASTNode *ast_create_number(double value) {
    ASTNode *node = ast_create(AST_EXPRESION_LITERAL);
    if (!node) return NULL;

    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        free(node);
        return NULL;
    }
    node->value = milena_strdup(buffer);
    if (!node->value) {
        free(node);
        return NULL;
    }
    node->number_value = value;
    return node;
}

bool ast_add_child(ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return false;
    if (parent->child_count == parent->child_capacity) {
        size_t capacity = parent->child_capacity == 0 ? 4 : parent->child_capacity;
        if (parent->child_capacity != 0) {
            if (capacity > SIZE_MAX / 2) return false;
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof(*parent->children)) return false;
        ASTNode **children = realloc(parent->children,
                                     capacity * sizeof(*parent->children));
        if (!children) return false;
        parent->children = children;
        parent->child_capacity = capacity;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return true;
}

ASTNode *ast_create_statistic(ASTStatOperation operation, ASTNode *argument,
                              int axis, bool keepdims, double percentile) {
    if ((unsigned)operation >= (unsigned)AST_STAT_OPERATION_COUNT || !argument) {
        ast_destroy(argument);
        return NULL;
    }
    ASTNode *node = ast_create(AST_OPERACION_ESTADISTICA);
    if (!node) {
        ast_destroy(argument);
        return NULL;
    }
    node->statistical_operation = operation;
    node->axis = axis;
    node->keepdims = keepdims;
    node->percentile = percentile;
    if (!ast_add_child(node, argument)) {
        ast_destroy(argument);
        free(node);
        return NULL;
    }
    return node;
}

void ast_print(ASTNode *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; i++) printf("  ");
    printf("%s", ast_type_name(node->type));
    if (node->type == AST_OPERACION_ESTADISTICA) {
        printf(" %s", ast_stat_operation_name(node->statistical_operation));
    }
    if (node->value) printf(" '%s'", node->value);
    if (node->type == AST_EXPRESION_LITERAL) printf(" %.17g", node->number_value);
    printf("\n");
    for (size_t i = 0; i < node->child_count; i++) {
        ast_print(node->children[i], depth + 1);
    }
}

void ast_destroy(ASTNode *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; i++) {
        ast_destroy(node->children[i]);
    }
    free(node->children);
    free(node->value);
    free(node->type_name);
    free(node);
}

const char *ast_type_name(ASTNodeType type) {
    static const char *const names[AST_NODE_TYPE_COUNT] = {
        "PROGRAMA", "BLOQUE_ANALISIS", "DECLARACION_DATOS",
        "DECLARACION_ESTADISTICA", "ASIGNACION_DATASET", "LLAMADA_CARGAR",
        "BLOQUE_LIMPIAR", "BLOQUE_TRANSFORMAR", "BLOQUE_FILTRAR",
        "BLOQUE_AGRUPAR", "BLOQUE_RESUMIR", "BLOQUE_VISUALIZAR",
        "BLOQUE_EXPORTAR", "EXPRESION_OPERACION", "EXPRESION_LITERAL",
        "EXPRESION_IDENTIFICADOR", "EXPRESION_FUNCION", "EXPRESION_ARRAY",
        "EXPRESION_LLAMADA", "BLOQUE_FUNCION", "COMANDO_RETORNAR",
        "CONDICION_SI", "DECLARACION_FUNCION", "DECLARACION_ARRAY",
        "DECLARACION_VARIABLE", "ASIGNACION_VARIABLE", "COMANDO_NULOS",
        "COMANDO_DUPLICADOS", "COMANDO_CONDICION", "COMANDO_EXTRAER",
        "COMANDO_TOTAL", "COMANDO_PERIODO", "AGRUPACION_POR",
        "RESUMEN_METRICA", "OPERACION_ESTADISTICA"
    };
    if ((unsigned)type >= (unsigned)AST_NODE_TYPE_COUNT) return "DESCONOCIDO";
    return names[type];
}

const char *ast_stat_operation_name(ASTStatOperation operation) {
    static const char *const names[AST_STAT_OPERATION_COUNT] = {
        "NINGUNA", "SUMA", "MEDIA", "MINIMO", "MAXIMO", "VARIANZA",
        "DESVIACION", "MEDIANA", "PERCENTIL"
    };
    if ((unsigned)operation >= (unsigned)AST_STAT_OPERATION_COUNT) {
        return "DESCONOCIDA";
    }
    return names[operation];
}
