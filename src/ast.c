#include "ast.h"

#include <stdint.h>

typedef struct {
    const ASTNode *node;
    const ASTNode *expected_parent;
} ASTValidationEntry;

typedef struct {
    ASTValidationEntry *items;
    size_t length;
    size_t capacity;
} ASTValidationStack;

typedef struct {
    const ASTNode **slots;
    size_t length;
    size_t capacity;
} ASTValidationSet;

static bool ast_validation_error(MilenaError *error, MilenaStatus code,
                                 const ASTNode *node, const char *message) {
    milena_error_set(error, code,
                     node && node->has_source_span ? (size_t)node->line : 0,
                     node && node->has_source_span ? (size_t)node->column : 0,
                     0, message);
    return false;
}

static bool ast_validation_stack_push(ASTValidationStack *stack,
                                     const ASTNode *node,
                                     const ASTNode *expected_parent) {
    if (stack->length == stack->capacity) {
        size_t capacity = stack->capacity == 0 ? 16 : stack->capacity * 2;
        size_t bytes;
        if (capacity < stack->capacity ||
            !milena_size_mul(capacity, sizeof(*stack->items), &bytes)) {
            return false;
        }
        ASTValidationEntry *items = realloc(stack->items, bytes);
        if (!items) return false;
        stack->items = items;
        stack->capacity = capacity;
    }
    stack->items[stack->length++] = (ASTValidationEntry){node, expected_parent};
    return true;
}

static size_t ast_pointer_hash(const ASTNode *node) {
    uintptr_t value = (uintptr_t)node;
    value ^= value >> 17;
    value *= (uintptr_t)0xed5ad4bbU;
    value ^= value >> 11;
    value *= (uintptr_t)0xac4c1b51U;
    value ^= value >> 15;
    return (size_t)value;
}

static bool ast_validation_set_grow(ASTValidationSet *set) {
    size_t capacity = set->capacity == 0 ? 16 : set->capacity * 2;
    size_t bytes;
    if (capacity < set->capacity ||
        !milena_size_mul(capacity, sizeof(*set->slots), &bytes)) {
        return false;
    }
    const ASTNode **slots = calloc(1, bytes);
    if (!slots) return false;
    for (size_t i = 0; i < set->capacity; i++) {
        const ASTNode *node = set->slots[i];
        if (!node) continue;
        size_t slot = ast_pointer_hash(node) & (capacity - 1);
        while (slots[slot]) slot = (slot + 1) & (capacity - 1);
        slots[slot] = node;
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;
    return true;
}

/* Returns false for an already-visited node; keeps traversal O(n) on average. */
static bool ast_validation_set_insert(ASTValidationSet *set,
                                     const ASTNode *node, bool *inserted) {
    if (set->capacity == 0 || set->length >= set->capacity / 2) {
        if (!ast_validation_set_grow(set)) return false;
    }
    size_t slot = ast_pointer_hash(node) & (set->capacity - 1);
    while (set->slots[slot]) {
        if (set->slots[slot] == node) {
            *inserted = false;
            return true;
        }
        slot = (slot + 1) & (set->capacity - 1);
    }
    set->slots[slot] = node;
    set->length++;
    *inserted = true;
    return true;
}

bool ast_validate(const ASTNode *root, MilenaError *error) {
    milena_error_clear(error);
    if (!root) {
        return ast_validation_error(error, MILENA_ERR_ARGUMENT, NULL,
                                    "La raíz del AST no puede ser nula");
    }

    ASTValidationStack stack = {0};
    ASTValidationSet visited = {0};
    if (!ast_validation_stack_push(&stack, root, NULL)) {
        free(stack.items);
        return ast_validation_error(error, MILENA_ERR_MEMORY, root,
                                    "Sin memoria para validar el AST");
    }

    bool valid = true;
    while (stack.length > 0) {
        ASTValidationEntry entry = stack.items[--stack.length];
        const ASTNode *node = entry.node;
        bool inserted = false;
        if (!ast_validation_set_insert(&visited, node, &inserted)) {
            valid = ast_validation_error(error, MILENA_ERR_MEMORY, node,
                                         "Sin memoria para validar el AST");
            break;
        }
        if (!inserted) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "El AST contiene un ciclo o un nodo compartido");
            break;
        }
        if ((unsigned)node->type >= (unsigned)AST_NODE_TYPE_COUNT) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Tipo de nodo AST fuera de rango");
            break;
        }
        if ((unsigned)node->value_type >= (unsigned)AST_VALUE_TYPE_COUNT ||
            (unsigned)node->operator_kind >= (unsigned)AST_OPERATOR_COUNT) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Anotación de tipo u operador AST fuera de rango");
            break;
        }
        if (node->parent != entry.expected_parent) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Relación padre-hijo inconsistente en el AST");
            break;
        }
        if (node->child_count > node->child_capacity ||
            ((node->child_capacity == 0) != (node->children == NULL))) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Almacenamiento de hijos inconsistente en el AST");
            break;
        }
        if (node->type == AST_EXPRESION_OPERACION &&
            (node->child_count != 2 || !node->value ||
             node->operator_kind == AST_OPERATOR_NONE ||
             node->operator_kind != ast_operator_kind_from_name(node->value) ||
             node->left_operand != node->children[0] ||
             node->right_operand != node->children[1])) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Operación AST sin operador u operandos estructurados coherentes");
            break;
        }
        if (node->type != AST_EXPRESION_OPERACION &&
            (node->left_operand || node->right_operand)) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Operandos estructurados en un nodo que no es operación");
            break;
        }
        if (node->has_source_span &&
            (node->line < 1 || node->column < 1 || node->end_line < node->line ||
             node->end_column < 1 || node->end_offset < node->start_offset ||
             (node->end_line == node->line && node->end_column < node->column))) {
            valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                         "Rango de fuente inválido en el AST");
            break;
        }

        for (size_t i = 0; i < node->child_count; i++) {
            const ASTNode *child = node->children[i];
            if (!child) {
                valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                             "El AST contiene un hijo nulo");
                break;
            }
            if (node->has_source_span && child->has_source_span &&
                (child->start_offset < node->start_offset ||
                 child->end_offset > node->end_offset)) {
                valid = ast_validation_error(error, MILENA_ERR_ARGUMENT, node,
                                             "El rango del nodo no contiene el de su hijo");
                break;
            }
            if (!ast_validation_stack_push(&stack, child, node)) {
                valid = ast_validation_error(error, MILENA_ERR_MEMORY, node,
                                             "Sin memoria para validar el AST");
                break;
            }
        }
        if (!valid) break;
    }

    free(stack.items);
    free(visited.slots);
    return valid;
}

ASTNode *ast_create(ASTNodeType type) {
    ASTNode *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->type = type;
    node->axis = -1;
    node->value_type = AST_VALUE_UNRESOLVED;
    node->operator_kind = AST_OPERATOR_NONE;
    return node;
}

bool ast_set_source_span(ASTNode *node, const Token *start, const Token *end) {
    if (!node || !start || !end || end->end_offset < start->start_offset) {
        return false;
    }
    node->line = start->line;
    node->column = start->column;
    node->end_line = end->end_line;
    node->end_column = end->end_column;
    node->start_offset = start->start_offset;
    node->end_offset = end->end_offset;
    node->has_source_span = true;
    return true;
}

bool ast_set_source_span_from_nodes(ASTNode *node, const ASTNode *first,
                                    const ASTNode *last) {
    if (!node || !first || !last || !first->has_source_span ||
        !last->has_source_span || last->end_offset < first->start_offset) {
        return false;
    }
    node->line = first->line;
    node->column = first->column;
    node->end_line = last->end_line;
    node->end_column = last->end_column;
    node->start_offset = first->start_offset;
    node->end_offset = last->end_offset;
    node->has_source_span = true;
    return true;
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
    if (type == AST_EXPRESION_OPERACION)
        node->operator_kind = ast_operator_kind_from_name(value);
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
    node->value_type = AST_VALUE_NUMBER;
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
    if (parent->type == AST_EXPRESION_OPERACION) {
        if (parent->child_count == 1) parent->left_operand = child;
        else if (parent->child_count == 2) parent->right_operand = child;
    }
    if (child->has_source_span) {
        if (!parent->has_source_span) {
            parent->line = child->line;
            parent->column = child->column;
            parent->end_line = child->end_line;
            parent->end_column = child->end_column;
            parent->start_offset = child->start_offset;
            parent->end_offset = child->end_offset;
            parent->has_source_span = true;
        } else {
            if (child->start_offset < parent->start_offset) {
                parent->start_offset = child->start_offset;
                parent->line = child->line;
                parent->column = child->column;
            }
            if (child->end_offset > parent->end_offset) {
                parent->end_offset = child->end_offset;
                parent->end_line = child->end_line;
                parent->end_column = child->end_column;
            }
        }
    }
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
        "AGRUPACION_SPILL", "RESUMEN_METRICA", "OPERACION_ESTADISTICA",
        "DECLARACION_ENTRADA", "DECLARACION_SALIDA",
        "BLOQUE_SELECCIONAR", "COMANDO_COLUMNAS",
        "BLOQUE_UNIR", "COMANDO_DERECHA", "COMANDO_CLAVE",
        "COMANDO_SST", "STREAM_FILTER", "COLUMNAR_PROJECT",
        "COLUMNAR_FIELD", "SQL_PROGRAM", "SQL_QUERY", "SQL_EXECUTE",
        "SQL_BEGIN", "SQL_COMMIT", "SQL_ROLLBACK", "SQL_PARAMETER",
        "SQL_TABLE_SCHEMA", "SQL_SCHEMA_COLUMN", "SQL_TYPED_SELECT",
        "SQL_TABLE_REFERENCE", "SQL_PROJECTION_LIST",
        "SQL_PROJECTED_COLUMN", "SQL_FILTER", "SQL_FILTER_COLUMN",
        "SQL_FILTER_OPERATOR", "SQL_TYPED_INSERT", "SQL_INSERT_COLUMN_LIST",
        "SQL_INSERT_COLUMN", "SQL_INSERT_VALUE_LIST", "SQL_TYPED_UPDATE",
        "SQL_UPDATE_ASSIGNMENT_LIST", "SQL_UPDATE_ASSIGNMENT",
        "SQL_UPDATE_COLUMN", "SQL_UPDATE_FILTER"
    };
    if ((unsigned)type >= (unsigned)AST_NODE_TYPE_COUNT) return "DESCONOCIDO";
    return names[type];
}

ASTOperatorKind ast_operator_kind_from_name(const char *name) {
    if (!name) return AST_OPERATOR_NONE;
    if (strcmp(name, "+") == 0) return AST_OPERATOR_ADD;
    if (strcmp(name, "-") == 0) return AST_OPERATOR_SUBTRACT;
    if (strcmp(name, "*") == 0) return AST_OPERATOR_MULTIPLY;
    if (strcmp(name, "/") == 0) return AST_OPERATOR_DIVIDE;
    if (strcmp(name, "==") == 0) return AST_OPERATOR_EQUAL;
    if (strcmp(name, "!=") == 0) return AST_OPERATOR_NOT_EQUAL;
    if (strcmp(name, ">") == 0) return AST_OPERATOR_GREATER;
    if (strcmp(name, ">=") == 0) return AST_OPERATOR_GREATER_EQUAL;
    if (strcmp(name, "<") == 0) return AST_OPERATOR_LESS;
    if (strcmp(name, "<=") == 0) return AST_OPERATOR_LESS_EQUAL;
    return AST_OPERATOR_NONE;
}

const char *ast_operator_kind_name(ASTOperatorKind operation) {
    static const char *const names[AST_OPERATOR_COUNT] = {
        "NINGUNO", "SUMA", "RESTA", "MULTIPLICACION", "DIVISION",
        "IGUAL", "DISTINTO", "MAYOR", "MAYOR_IGUAL", "MENOR",
        "MENOR_IGUAL"
    };
    if ((unsigned)operation >= (unsigned)AST_OPERATOR_COUNT)
        return "DESCONOCIDO";
    return names[operation];
}

const char *ast_value_type_name(ASTValueType type) {
    static const char *const names[AST_VALUE_TYPE_COUNT] = {
        "SIN_RESOLVER", "NUMERO", "BOOLEANO", "TEXTO", "ARREGLO", "DATASET"
    };
    if ((unsigned)type >= (unsigned)AST_VALUE_TYPE_COUNT) return "DESCONOCIDO";
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
