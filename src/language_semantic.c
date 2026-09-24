#include "language_semantic.h"
#include "table.h"
#include "grouped_aggregate.h"
#include <stdio.h>
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
    milena_error_set(error, MILENA_ERR_PARSE,
                     node && node->has_source_span ? (size_t)node->line : 0,
                     node && node->has_source_span ? (size_t)node->column : 0,
                     0, message);
    return MILENA_ERR_PARSE;
}

static MilenaStatus typed_error(const ASTNode *node, MilenaError *error,
                                const char *message) {
    milena_error_set(error, MILENA_ERR_TYPE,
                     node && node->has_source_span ? (size_t)node->line : 0,
                     node && node->has_source_span ? (size_t)node->column : 0,
                     0, message);
    return MILENA_ERR_TYPE;
}

static MilenaStatus typed_error_name(const ASTNode *node, MilenaError *error,
                                     const char *prefix, const char *name) {
    char message[MILENA_ERROR_TEXT];
    (void)snprintf(message, sizeof(message), "%s: %s",
                   prefix, name ? name : "<vacío>");
    return typed_error(node, error, message);
}

static MilenaStatus semantic_memory_error(const ASTNode *node,
                                          MilenaError *error) {
    milena_error_set(error, MILENA_ERR_MEMORY,
                     node && node->has_source_span ? (size_t)node->line : 0,
                     node && node->has_source_span ? (size_t)node->column : 0,
                     0, "Memoria insuficiente durante la resolución semántica");
    return MILENA_ERR_MEMORY;
}

static bool numeric_type(ASTValueType type) {
    return type == AST_VALUE_NUMBER;
}

static bool scalar_type(ASTValueType type) {
    return type == AST_VALUE_NUMBER || type == AST_VALUE_BOOLEAN;
}

static bool comparison_operator(ASTOperatorKind operation) {
    return operation >= AST_OPERATOR_EQUAL && operation <= AST_OPERATOR_LESS_EQUAL;
}

typedef struct {
    const char *name;
    ASTValueType type;
    size_t id;
    size_t depth;
} ResolvedVariable;

typedef struct {
    const char *name;
    const ASTNode *declaration;
    size_t id;
    size_t arity;
} ResolvedFunction;

typedef struct {
    ResolvedVariable *variables;
    size_t variable_count;
    size_t variable_capacity;
    ResolvedFunction *functions;
    size_t function_count;
    size_t next_id;
    size_t depth;
} ResolutionContext;

static ResolvedVariable *find_variable(ResolutionContext *context,
                                       const char *name) {
    if (!context || !name) return NULL;
    for (size_t i = context->variable_count; i > 0; --i) {
        ResolvedVariable *variable = &context->variables[i - 1];
        if (strcmp(variable->name, name) == 0) return variable;
    }
    return NULL;
}

static ResolvedVariable *find_current_variable(ResolutionContext *context,
                                               const char *name) {
    if (!context || !name) return NULL;
    for (size_t i = context->variable_count; i > 0; --i) {
        ResolvedVariable *variable = &context->variables[i - 1];
        if (variable->depth < context->depth) break;
        if (variable->depth == context->depth &&
            strcmp(variable->name, name) == 0) return variable;
    }
    return NULL;
}

static ResolvedFunction *find_function(ResolutionContext *context,
                                       const char *name) {
    if (!context || !name) return NULL;
    for (size_t i = 0; i < context->function_count; ++i)
        if (strcmp(context->functions[i].name, name) == 0)
            return &context->functions[i];
    return NULL;
}

static void pop_scope(ResolutionContext *context) {
    if (!context || context->depth == 0) return;
    while (context->variable_count > 0 &&
           context->variables[context->variable_count - 1].depth == context->depth)
        context->variable_count--;
    context->depth--;
}

static MilenaStatus push_scope(ResolutionContext *context,
                               const ASTNode *node, MilenaError *error) {
    if (context->depth == SIZE_MAX)
        return semantic_memory_error(node, error);
    context->depth++;
    return MILENA_OK;
}

static MilenaStatus declare_variable(ResolutionContext *context,
                                     ASTNode *declaration,
                                     ASTValueType type,
                                     MilenaError *error) {
    if (!declaration->value || !declaration->value[0])
        return typed_error(declaration, error, "Declaración de variable sin nombre");
    if (find_current_variable(context, declaration->value))
        return typed_error_name(declaration, error,
                               "Variable duplicada en el mismo ámbito",
                               declaration->value);
    if (context->next_id == SIZE_MAX)
        return semantic_memory_error(declaration, error);
    if (context->variable_count == context->variable_capacity) {
        size_t capacity = context->variable_capacity ?
                          context->variable_capacity * 2 : 8;
        if (capacity < context->variable_capacity ||
            capacity > SIZE_MAX / sizeof(*context->variables))
            return semantic_memory_error(declaration, error);
        ResolvedVariable *variables = realloc(
            context->variables, capacity * sizeof(*context->variables));
        if (!variables) return semantic_memory_error(declaration, error);
        context->variables = variables;
        context->variable_capacity = capacity;
    }
    size_t id = ++context->next_id;
    context->variables[context->variable_count++] = (ResolvedVariable){
        declaration->value, type, id, context->depth
    };
    declaration->resolved_symbol_id = id;
    declaration->value_type = type;
    return MILENA_OK;
}

static MilenaStatus annotate_expression_resolved(ASTNode *node,
                                                ResolutionContext *context,
                                                MilenaError *error);
static MilenaStatus annotate_statement_resolved(ASTNode *node,
                                                ResolutionContext *context,
                                                MilenaError *error);

static MilenaStatus annotate_expression_resolved(ASTNode *node,
                                                ResolutionContext *context,
                                                MilenaError *error) {
    if (!node) return typed_error(NULL, error, "Expresión AST nula");
    switch (node->type) {
        case AST_EXPRESION_LITERAL:
            if (!isfinite(node->number_value))
                return typed_error(node, error, "Literal numérico no finito");
            if (node->value_type == AST_VALUE_UNRESOLVED)
                node->value_type = AST_VALUE_NUMBER;
            if (!scalar_type(node->value_type))
                return typed_error(node, error, "Tipo incompatible para literal escalar");
            return MILENA_OK;
        case AST_EXPRESION_IDENTIFICADOR: {
            if (!node->value || !node->value[0])
                return typed_error(node, error, "Identificador AST vacío");
            ResolvedVariable *variable = find_variable(context, node->value);
            if (!variable)
                return typed_error_name(node, error,
                    "Identificador no declarado en el ámbito visible", node->value);
            node->resolved_symbol_id = variable->id;
            node->value_type = variable->type;
            return MILENA_OK;
        }
        case AST_EXPRESION_LLAMADA:
        case AST_EXPRESION_FUNCION: {
            if (!node->value || !node->value[0])
                return typed_error(node, error, "Llamada de función sin nombre");
            ResolvedFunction *function = find_function(context, node->value);
            if (!function)
                return typed_error_name(node, error,
                    "Función no declarada", node->value);
            if (node->child_count != function->arity)
                return typed_error_name(node, error,
                    "Cantidad de argumentos incompatible para función", node->value);
            for (size_t i = 0; i < node->child_count; ++i) {
                MilenaStatus status = annotate_expression_resolved(
                    node->children[i], context, error);
                if (status != MILENA_OK) return status;
                if (!numeric_type(node->children[i]->value_type))
                    return typed_error(node->children[i], error,
                        "Los parámetros de funciones numéricas requieren argumentos numéricos");
            }
            node->resolved_symbol_id = function->id;
            node->value_type = AST_VALUE_NUMBER;
            return MILENA_OK;
        }
        case AST_EXPRESION_OPERACION: {
            if (node->child_count != 2 || !node->left_operand ||
                !node->right_operand || node->left_operand != node->children[0] ||
                node->right_operand != node->children[1] ||
                node->operator_kind == AST_OPERATOR_NONE)
                return typed_error(node, error,
                    "Operación requiere dos operandos estructurados y operador tipado");
            MilenaStatus status = annotate_expression_resolved(
                node->left_operand, context, error);
            if (status != MILENA_OK) return status;
            status = annotate_expression_resolved(node->right_operand,
                                                  context, error);
            if (status != MILENA_OK) return status;
            if (!scalar_type(node->left_operand->value_type) ||
                !scalar_type(node->right_operand->value_type))
                return typed_error(node, error, "Los operandos deben ser valores escalares");
            if (comparison_operator(node->operator_kind)) {
                node->value_type = AST_VALUE_BOOLEAN;
            } else {
                if (!numeric_type(node->left_operand->value_type) ||
                    !numeric_type(node->right_operand->value_type))
                    return typed_error(node, error,
                        "La aritmética solo admite operandos numéricos");
                node->value_type = AST_VALUE_NUMBER;
            }
            return MILENA_OK;
        }
        default:
            return typed_error(node, error,
                "Expresión fuera del subconjunto escalar de funciones numéricas");
    }
}

static ASTValueType declared_type(const ASTNode *node) {
    if (node && node->type_name) {
        if (strcmp(node->type_name, "numerica") == 0) return AST_VALUE_NUMBER;
        if (strcmp(node->type_name, "binaria") == 0) return AST_VALUE_BOOLEAN;
        if (strcmp(node->type_name, "categorica") == 0 ||
            strcmp(node->type_name, "texto") == 0 ||
            strcmp(node->type_name, "fecha") == 0) return AST_VALUE_TEXT;
    }
    return AST_VALUE_UNRESOLVED;
}

static MilenaStatus annotate_statement_list(ASTNode *block,
                                            size_t begin, size_t end,
                                            ResolutionContext *context,
                                            MilenaError *error) {
    for (size_t i = begin; i < end; ++i) {
        MilenaStatus status = annotate_statement_resolved(block->children[i],
                                                          context, error);
        if (status != MILENA_OK) return status;
    }
    return MILENA_OK;
}

static MilenaStatus annotate_statement_resolved(ASTNode *node,
                                                ResolutionContext *context,
                                                MilenaError *error) {
    if (!node) return typed_error(NULL, error, "Sentencia AST nula");
    switch (node->type) {
        case AST_DECLARACION_VARIABLE: {
            if (node->child_count > 1)
                return typed_error(node, error,
                    "Declaración escalar requiere cero o una inicialización");
            ASTValueType type = declared_type(node);
            if (node->child_count == 1) {
                MilenaStatus status = annotate_expression_resolved(
                    node->children[0], context, error);
                if (status != MILENA_OK) return status;
                type = node->children[0]->value_type;
                if (node->type_name && declared_type(node) != type)
                    return typed_error(node, error,
                        "Tipo de inicialización incompatible con la declaración");
            }
            if (type == AST_VALUE_UNRESOLVED)
                return typed_error(node, error,
                    "La declaración requiere inicializador o tipo explícito admitido");
            return declare_variable(context, node, type, error);
        }
        case AST_ASIGNACION_VARIABLE: {
            if (!node->value || !node->value[0])
                return typed_error(node, error, "Asignación sin identificador");
            ResolvedVariable *target = find_variable(context, node->value);
            if (!target)
                return typed_error_name(node, error,
                    "Asignación a variable no declarada", node->value);
            if (node->child_count != 1)
                return typed_error(node, error,
                    "Asignación escalar requiere exactamente una expresión");
            MilenaStatus status = annotate_expression_resolved(
                node->children[0], context, error);
            if (status != MILENA_OK) return status;
            if (node->children[0]->value_type != target->type)
                return typed_error(node, error,
                    "Tipo de asignación incompatible con la variable declarada");
            node->resolved_symbol_id = target->id;
            node->value_type = target->type;
            return MILENA_OK;
        }
        case AST_COMANDO_RETORNAR:
            if (node->child_count != 1)
                return typed_error(node, error,
                    "Retornar requiere exactamente una expresión");
            {
                MilenaStatus status = annotate_expression_resolved(
                    node->children[0], context, error);
                if (status != MILENA_OK) return status;
                if (!numeric_type(node->children[0]->value_type))
                    return typed_error(node->children[0], error,
                        "La ABI actual de retorno de función requiere un número");
                node->value_type = AST_VALUE_NUMBER;
            }
            return MILENA_OK;
        case AST_CONDICION_SI: {
            if (node->child_count == 0)
                return typed_error(node, error, "Condición si sin expresión");
            MilenaStatus status = annotate_expression_resolved(
                node->children[0], context, error);
            if (status != MILENA_OK) return status;
            if (!scalar_type(node->children[0]->value_type))
                return typed_error(node->children[0], error,
                    "La condición si debe ser escalar");
            size_t then_end = node->child_count;
            ASTNode *else_block = NULL;
            if (then_end > 1 &&
                node->children[then_end - 1]->type == AST_BLOQUE_FUNCION) {
                else_block = node->children[then_end - 1];
                then_end--;
            }
            status = push_scope(context, node, error);
            if (status != MILENA_OK) return status;
            status = annotate_statement_list(node, 1, then_end, context, error);
            pop_scope(context);
            if (status != MILENA_OK) return status;
            if (else_block) {
                status = push_scope(context, else_block, error);
                if (status != MILENA_OK) return status;
                status = annotate_statement_list(else_block, 0,
                    else_block->child_count, context, error);
                pop_scope(context);
                if (status != MILENA_OK) return status;
            }
            return MILENA_OK;
        }
        default:
            return typed_error(node, error,
                "Sentencia fuera del subconjunto escalar admitido en funciones");
    }
}

static MilenaStatus collect_functions(ASTNode *program,
                                      ResolutionContext *context,
                                      MilenaError *error) {
    size_t count = 0;
    for (size_t i = 0; i < program->child_count; ++i)
        if (program->children[i]->type == AST_DECLARACION_FUNCION) count++;
    if (count > SIZE_MAX / sizeof(*context->functions))
        return semantic_memory_error(program, error);
    if (count) {
        context->functions = calloc(count, sizeof(*context->functions));
        if (!context->functions) return semantic_memory_error(program, error);
    }
    for (size_t i = 0; i < program->child_count; ++i) {
        ASTNode *function = program->children[i];
        if (function->type != AST_DECLARACION_FUNCION) continue;
        if (!function->value || !function->value[0] || function->child_count != 2 ||
            !function->children[0] || !function->children[1])
            return typed_error(function, error, "Declaración de función estructuralmente inválida");
        if (find_function(context, function->value))
            return typed_error_name(function, error,
                "Declaración duplicada de función", function->value);
        if (context->next_id == SIZE_MAX)
            return semantic_memory_error(function, error);
        size_t id = ++context->next_id;
        function->resolved_symbol_id = id;
        context->functions[context->function_count++] = (ResolvedFunction){
            function->value, function, id, function->children[0]->child_count
        };
    }
    return MILENA_OK;
}

static MilenaStatus annotate_function(ASTNode *function,
                                      ResolutionContext *context,
                                      MilenaError *error) {
    ASTNode *parameters = function->children[0];
    ASTNode *body = function->children[1];
    MilenaStatus status = push_scope(context, function, error);
    if (status != MILENA_OK) return status;
    for (size_t i = 0; i < parameters->child_count; ++i) {
        ASTNode *parameter = parameters->children[i];
        if (!parameter || parameter->type != AST_EXPRESION_IDENTIFICADOR) {
            status = typed_error(function, error,
                "Parámetro de función con representación inválida");
            break;
        }
        parameter->value_type = AST_VALUE_NUMBER;
        status = declare_variable(context, parameter, AST_VALUE_NUMBER, error);
        if (status != MILENA_OK) break;
    }
    if (status == MILENA_OK)
        status = annotate_statement_list(body, 0, body->child_count,
                                         context, error);
    pop_scope(context);
    return status;
}

static MilenaStatus annotate_resolved_program(ASTNode *program,
                                             MilenaError *error) {
    ResolutionContext context = {0};
    MilenaStatus status = collect_functions(program, &context, error);
    for (size_t i = 0; status == MILENA_OK && i < program->child_count; ++i) {
        ASTNode *node = program->children[i];
        if (node->type == AST_DECLARACION_FUNCION)
            status = annotate_function(node, &context, error);
        else
            status = annotate_statement_resolved(node, &context, error);
    }
    free(context.variables);
    free(context.functions);
    return status;
}

/* Typed scalar annotations for the legacy canonical analysis AST. */
static MilenaStatus annotate_expression(ASTNode *node, MilenaError *error) {
    if (!node) return typed_error(NULL, error, "Expresión AST nula");
    switch (node->type) {
        case AST_EXPRESION_LITERAL:
            if (!isfinite(node->number_value))
                return typed_error(node, error, "Literal numérico no finito");
            if (node->value_type == AST_VALUE_UNRESOLVED)
                node->value_type = AST_VALUE_NUMBER;
            if (!scalar_type(node->value_type))
                return typed_error(node, error, "Tipo incompatible para literal escalar");
            return MILENA_OK;
        case AST_EXPRESION_IDENTIFICADOR:
            if (!node->value || !node->value[0])
                return typed_error(node, error, "Identificador AST vacío");
            /* Legacy analysis identifiers denote dataframe columns, not locals. */
            node->value_type = AST_VALUE_NUMBER;
            return MILENA_OK;
        case AST_EXPRESION_LLAMADA:
        case AST_EXPRESION_FUNCION:
            if (!node->value || !node->value[0])
                return typed_error(node, error, "Llamada de función sin nombre");
            for (size_t i = 0; i < node->child_count; ++i) {
                MilenaStatus status = annotate_expression(node->children[i], error);
                if (status != MILENA_OK) return status;
            }
            node->value_type = AST_VALUE_NUMBER;
            return MILENA_OK;
        case AST_EXPRESION_OPERACION: {
            if (node->child_count != 2 || !node->left_operand ||
                !node->right_operand || node->left_operand != node->children[0] ||
                node->right_operand != node->children[1] ||
                node->operator_kind == AST_OPERATOR_NONE)
                return typed_error(node, error,
                    "Operación requiere dos operandos estructurados y operador tipado");
            MilenaStatus status = annotate_expression(node->left_operand, error);
            if (status != MILENA_OK) return status;
            status = annotate_expression(node->right_operand, error);
            if (status != MILENA_OK) return status;
            if (!scalar_type(node->left_operand->value_type) ||
                !scalar_type(node->right_operand->value_type))
                return typed_error(node, error, "Los operandos deben ser valores escalares");
            if (comparison_operator(node->operator_kind)) {
                node->value_type = AST_VALUE_BOOLEAN;
            } else {
                if (!numeric_type(node->left_operand->value_type) ||
                    !numeric_type(node->right_operand->value_type))
                    return typed_error(node, error,
                        "La aritmética solo admite operandos numéricos");
                node->value_type = AST_VALUE_NUMBER;
            }
            return MILENA_OK;
        }
        case AST_EXPRESION_ARRAY:
            for (size_t i = 0; i < node->child_count; ++i) {
                MilenaStatus status = annotate_expression(node->children[i], error);
                if (status != MILENA_OK) return status;
                if (!numeric_type(node->children[i]->value_type))
                    return typed_error(node->children[i], error,
                        "El literal de arreglo solo admite elementos numéricos");
            }
            node->value_type = AST_VALUE_ARRAY;
            return MILENA_OK;
        default:
            return typed_error(node, error,
                "Nodo no escalar usado donde se esperaba una expresión tipada");
    }
}

static MilenaStatus annotate_typed_ast(ASTNode *node, MilenaError *error) {
    if (!node) return typed_error(NULL, error, "Nodo AST nulo");
    switch (node->type) {
        case AST_EXPRESION_LITERAL:
        case AST_EXPRESION_IDENTIFICADOR:
        case AST_EXPRESION_FUNCION:
        case AST_EXPRESION_OPERACION:
        case AST_EXPRESION_ARRAY:
        case AST_EXPRESION_LLAMADA:
            return annotate_expression(node, error);
        case AST_OPERACION_ESTADISTICA:
            if (node->child_count != 1 ||
                node->children[0]->type != AST_EXPRESION_IDENTIFICADOR ||
                !node->children[0]->value || !node->children[0]->value[0])
                return typed_error(node, error,
                    "Operación estadística requiere una columna estructurada");
            node->children[0]->value_type = AST_VALUE_ARRAY;
            node->value_type = node->axis < 0 ? AST_VALUE_NUMBER : AST_VALUE_ARRAY;
            return MILENA_OK;
        case AST_DECLARACION_VARIABLE:
            if (node->child_count == 1) {
                MilenaStatus status = annotate_expression(node->children[0], error);
                if (status != MILENA_OK) return status;
                node->value_type = node->children[0]->value_type;
            } else if (node->child_count == 0 && node->type_name) {
                node->value_type = declared_type(node);
            }
            if (node->child_count > 1)
                return typed_error(node, error,
                    "Declaración escalar requiere cero o una inicialización");
            break;
        case AST_ASIGNACION_VARIABLE:
            if (node->child_count != 1)
                return typed_error(node, error,
                    "Asignación escalar requiere exactamente una expresión");
            {
                MilenaStatus status = annotate_expression(node->children[0], error);
                if (status != MILENA_OK) return status;
                node->value_type = node->children[0]->value_type;
            }
            break;
        case AST_COMANDO_RETORNAR:
            if (node->child_count != 1)
                return typed_error(node, error,
                    "Retornar requiere exactamente una expresión");
            {
                MilenaStatus status = annotate_expression(node->children[0], error);
                if (status != MILENA_OK) return status;
                node->value_type = node->children[0]->value_type;
            }
            break;
        case AST_CONDICION_SI:
            if (node->child_count == 0)
                return typed_error(node, error, "Condición si sin expresión");
            {
                MilenaStatus status = annotate_expression(node->children[0], error);
                if (status != MILENA_OK) return status;
                if (!scalar_type(node->children[0]->value_type))
                    return typed_error(node->children[0], error,
                        "La condición si debe ser escalar");
            }
            for (size_t i = 1; i < node->child_count; ++i) {
                MilenaStatus status = annotate_typed_ast(node->children[i], error);
                if (status != MILENA_OK) return status;
            }
            return MILENA_OK;
        default:
            break;
    }
    for (size_t i = 0; i < node->child_count; ++i) {
        MilenaStatus status = annotate_typed_ast(node->children[i], error);
        if (status != MILENA_OK) return status;
    }
    return MILENA_OK;
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
    if (!ast_validate(program, error)) return error ? error->code : MILENA_ERR_ARGUMENT;
    MilenaStatus status = validate_node(program, error);
    if (status != MILENA_OK) return status;
    for (size_t i = 0; i < program->child_count; ++i) {
        if (program->children[i]->type == AST_DECLARACION_FUNCION)
            return annotate_resolved_program((ASTNode *)program, error);
    }
    return annotate_typed_ast((ASTNode *)program, error);
}
