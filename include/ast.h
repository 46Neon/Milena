#ifndef MILENA_AST_H
#define MILENA_AST_H

#include "common.h"
#include "token.h"

typedef enum {
    AST_PROGRAMA,
    AST_BLOQUE_ANALISIS,
    AST_DECLARACION_DATOS,
    AST_DECLARACION_ESTADISTICA,
    AST_ASIGNACION_DATASET,
    AST_LLAMADA_CARGAR,
    AST_BLOQUE_LIMPIAR,
    AST_BLOQUE_TRANSFORMAR,
    AST_BLOQUE_FILTRAR,
    AST_BLOQUE_AGRUPAR,
    AST_BLOQUE_RESUMIR,
    AST_BLOQUE_VISUALIZAR,
    AST_BLOQUE_EXPORTAR,
    AST_EXPRESION_OPERACION,
    AST_EXPRESION_LITERAL,
    AST_EXPRESION_IDENTIFICADOR,
    AST_EXPRESION_FUNCION,
    AST_EXPRESION_ARRAY,
    AST_DECLARACION_ARRAY,
    AST_COMANDO_NULOS,
    AST_COMANDO_DUPLICADOS,
    AST_COMANDO_CONDICION,
    AST_COMANDO_EXTRAER,
    AST_COMANDO_TOTAL,
    AST_COMANDO_PERIODO,
    AST_AGRUPACION_POR,
    AST_RESUMEN_METRICA
} ASTNodeType;

typedef struct ASTNode {
    ASTNodeType type;
    char *value;
    double number_value;
    struct ASTNode **children;
    size_t child_count;
    size_t child_capacity;
    int line;
    int column;
    struct ASTNode *parent;
} ASTNode;

ASTNode* ast_create(ASTNodeType type);
ASTNode* ast_create_leaf(ASTNodeType type, const char *value);
ASTNode* ast_create_number(double value);
void ast_add_child(ASTNode *parent, ASTNode *child);
void ast_print(ASTNode *node, int depth);
void ast_destroy(ASTNode *node);
const char* ast_type_name(ASTNodeType type);

#endif
