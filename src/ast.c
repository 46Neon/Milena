#include "ast.h"

ASTNode* ast_create(ASTNodeType type) {
    ASTNode *node = (ASTNode *)calloc(1, sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = type;
    node->value = NULL;
    node->number_value = 0.0;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    node->line = 0;
    node->column = 0;
    node->parent = NULL;
    
    return node;
}

ASTNode* ast_create_leaf(ASTNodeType type, const char *value) {
    ASTNode *node = ast_create(type);
    if (!node) return NULL;
    
    if (value) {
        node->value = (char *)malloc(strlen(value) + 1);
        if (node->value) {
            strcpy(node->value, value);
        }
    }
    
    return node;
}

ASTNode* ast_create_number(double value) {
    ASTNode *node = ast_create(AST_EXPRESION_LITERAL);
    if (!node) return NULL;
    
    node->number_value = value;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%g", value);
    node->value = (char *)malloc(strlen(buffer) + 1);
    if (node->value) {
        strcpy(node->value, buffer);
    }
    
    return node;
}

void ast_add_child(ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return;
    
    if (parent->child_count >= parent->child_capacity) {
        size_t new_capacity = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        ASTNode **new_children = (ASTNode **)realloc(parent->children, new_capacity * sizeof(ASTNode *));
        if (!new_children) return;
        
        parent->children = new_children;
        parent->child_capacity = new_capacity;
    }
    
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void ast_print(ASTNode *node, int depth) {
    if (!node) return;
    
    for (int i = 0; i < depth; i++) printf("  ");
    printf("%s", ast_type_name(node->type));
    
    if (node->value) printf(" '%s'", node->value);
    if (node->number_value != 0.0) printf(" %.2f", node->number_value);
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
    free(node);
}

const char* ast_type_name(ASTNodeType type) {
    static const char *names[] = {
        "PROGRAMA",
        "BLOQUE_ANALISIS",
        "DECLARACION_DATOS",
        "DECLARACION_ESTADISTICA",
        "ASIGNACION_DATASET",
        "LLAMADA_CARGAR",
        "BLOQUE_LIMPIAR",
        "BLOQUE_TRANSFORMAR",
        "BLOQUE_FILTRAR",
        "BLOQUE_AGRUPAR",
        "BLOQUE_RESUMIR",
        "BLOQUE_VISUALIZAR",
        "BLOQUE_EXPORTAR",
        "EXPRESION_OPERACION",
        "EXPRESION_LITERAL",
        "EXPRESION_IDENTIFICADOR",
        "EXPRESION_FUNCION",
        "EXPRESION_ARRAY",
        "DECLARACION_ARRAY",
        "COMANDO_NULOS",
        "COMANDO_DUPLICADOS",
        "COMANDO_CONDICION",
        "COMANDO_EXTRAER",
        "COMANDO_TOTAL",
        "COMANDO_PERIODO",
        "AGRUPACION_POR",
        "RESUMEN_METRICA"
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    if ((size_t)type >= count) return "DESCONOCIDO";
    return names[(size_t)type];
}
