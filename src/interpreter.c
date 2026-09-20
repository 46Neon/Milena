#include "interpreter.h"
#include <math.h>

typedef struct { char *name; double value; } Binding;
typedef struct Runtime {
    Binding *items;
    size_t count;
    struct Runtime *parent;
    unsigned depth;
} Runtime;

static bool runtime_lookup(Runtime *runtime, const char *name, double *out) {
    for (; runtime; runtime = runtime->parent) {
        for (size_t i = runtime->count; i > 0; --i) {
            if (strcmp(runtime->items[i - 1].name, name) == 0) {
                *out = runtime->items[i - 1].value;
                return true;
            }
        }
    }
    return false;
}

static bool runtime_assign(Runtime *runtime, const char *name, double value) {
    for (; runtime; runtime = runtime->parent) {
        for (size_t i = runtime->count; i > 0; --i) {
            if (strcmp(runtime->items[i - 1].name, name) == 0) {
                runtime->items[i - 1].value = value;
                return true;
            }
        }
    }
    return false;
}

static bool runtime_bind(Runtime *runtime, const char *name, double value) {
    Binding *items;
    if (!runtime || !name) return false;
    items = (Binding *)realloc(runtime->items,
                               (runtime->count + 1) * sizeof *items);
    if (!items) return false;
    runtime->items = items;
    runtime->items[runtime->count].name = milena_strdup(name);
    if (!runtime->items[runtime->count].name) return false;
    runtime->items[runtime->count].value = value;
    runtime->count++;
    return true;
}

static bool eval_expr(Interpreter *interpreter, ASTNode *node,
                      Runtime *runtime, double *out);
static bool execute_function_statement(Interpreter *interpreter, ASTNode *node,
                                       Runtime *runtime, bool *returned,
                                       double *return_value);

static bool invoke(Interpreter *interpreter, ASTNode *function, ASTNode *call,
                   Runtime *parent, double *out) {
    Runtime local = {0};
    bool returned = false;
    double value = 0.0;

    if (!function || function->child_count != 2 || !call || !parent ||
        function->children[0]->child_count != call->child_count ||
        parent->depth >= 1000) {
        return false;
    }

    local.parent = parent;
    local.depth = parent->depth + 1;
    for (size_t i = 0; i < call->child_count; ++i) {
        if (!eval_expr(interpreter, call->children[i], parent, &value) ||
            !runtime_bind(&local,
                          function->children[0]->children[i]->value,
                          value)) {
            goto fail;
        }
    }

    for (size_t i = 0;
         i < function->children[1]->child_count && !returned;
         ++i) {
        if (!execute_function_statement(interpreter,
                                        function->children[1]->children[i],
                                        &local, &returned, &value)) {
            goto fail;
        }
    }

    *out = returned ? value : 0.0;
    for (size_t i = 0; i < local.count; ++i) free(local.items[i].name);
    free(local.items);
    return true;

fail:
    for (size_t i = 0; i < local.count; ++i) free(local.items[i].name);
    free(local.items);
    return false;
}

static bool eval_expr(Interpreter *interpreter, ASTNode *node,
                      Runtime *runtime, double *out) {
    double left, right;
    if (!node || !out) return false;

    switch (node->type) {
        case AST_EXPRESION_LITERAL:
            *out = node->number_value;
            return true;
        case AST_EXPRESION_IDENTIFICADOR:
            return runtime_lookup(runtime, node->value, out);
        case AST_EXPRESION_LLAMADA: {
            Symbol *symbol = symbol_table_lookup(interpreter->symbols,
                                                  node->value);
            return symbol && symbol->declaration &&
                   invoke(interpreter, symbol->declaration, node,
                          runtime, out);
        }
        case AST_EXPRESION_OPERACION:
            if (node->child_count != 2 ||
                !eval_expr(interpreter, node->children[0], runtime, &left) ||
                !eval_expr(interpreter, node->children[1], runtime, &right)) {
                return false;
            }
            if (strcmp(node->value, "+") == 0) *out = left + right;
            else if (strcmp(node->value, "-") == 0) *out = left - right;
            else if (strcmp(node->value, "*") == 0) *out = left * right;
            else if (strcmp(node->value, "/") == 0) {
                if (right == 0.0) return false;
                *out = left / right;
            } else if (strcmp(node->value, "==") == 0) *out = left == right;
            else if (strcmp(node->value, "!=") == 0) *out = left != right;
            else if (strcmp(node->value, ">") == 0) *out = left > right;
            else if (strcmp(node->value, ">=") == 0) *out = left >= right;
            else if (strcmp(node->value, "<") == 0) *out = left < right;
            else if (strcmp(node->value, "<=") == 0) *out = left <= right;
            else return false;
            return isfinite(*out);
        default:
            return false;
    }
}

static bool execute_function_statement(Interpreter *interpreter, ASTNode *node,
                                       Runtime *runtime, bool *returned,
                                       double *return_value) {
    double value;
    if (!node) return true;

    if (node->type == AST_COMANDO_RETORNAR) {
        if (node->child_count != 1 ||
            !eval_expr(interpreter, node->children[0], runtime, &value)) {
            return false;
        }
        *return_value = value;
        *returned = true;
        return true;
    }

    if (node->type == AST_DECLARACION_VARIABLE) {
        return node->child_count == 1 &&
               eval_expr(interpreter, node->children[0], runtime, &value) &&
               runtime_bind(runtime, node->value, value);
    }

    if (node->type == AST_ASIGNACION_VARIABLE) {
        return node->child_count == 1 &&
               eval_expr(interpreter, node->children[0], runtime, &value) &&
               runtime_assign(runtime, node->value, value);
    }

    if (node->type == AST_CONDICION_SI) {
        if (node->child_count < 1 ||
            !eval_expr(interpreter, node->children[0], runtime, &value)) {
            return false;
        }
        if (value != 0.0) {
            size_t end = node->child_count;
            if (end > 1 &&
                node->children[end - 1]->type == AST_BLOQUE_FUNCION) {
                end--;
            }
            for (size_t i = 1; i < end && !*returned; ++i) {
                if (!execute_function_statement(interpreter, node->children[i],
                                                runtime, returned,
                                                return_value)) {
                    return false;
                }
            }
        } else if (node->child_count > 1 &&
                   node->children[node->child_count - 1]->type ==
                       AST_BLOQUE_FUNCION) {
            return execute_function_statement(
                interpreter, node->children[node->child_count - 1], runtime,
                returned, return_value);
        }
        return true;
    }

    if (node->type == AST_BLOQUE_FUNCION) {
        for (size_t i = 0; i < node->child_count && !*returned; ++i) {
            if (!execute_function_statement(interpreter, node->children[i],
                                            runtime, returned,
                                            return_value)) {
                return false;
            }
        }
        return true;
    }

    return true;
}

bool interpreter_init(Interpreter *interpreter, ASTNode *ast) {
    if (!interpreter) return false;
    
    interpreter->ast = ast;
    interpreter->symbols = symbol_table_create();
    if (!interpreter->symbols) return false;
    
    interpreter->dataset = NULL;
    interpreter->has_error = false;
    interpreter->runtime = calloc(1, sizeof(Runtime));
    milena_error_init(&interpreter->error);
    if (!interpreter->runtime) {
        symbol_table_destroy(interpreter->symbols);
        interpreter->symbols = NULL;
        return false;
    }
    
    return true;
}

static bool interpreter_execute_node(Interpreter *interpreter, ASTNode *node);

static bool interpreter_execute_program(Interpreter *interpreter, ASTNode *node) {
    if (!node) return true;

    /* Register every function before evaluating globals so recursion and
       forward calls resolve through the same symbol table. */
    for (size_t i = 0; i < node->child_count; i++) {
        ASTNode *function = node->children[i];
        if (function->type == AST_DECLARACION_FUNCION &&
            !symbol_table_lookup_local(interpreter->symbols,
                                       function->value)) {
            Symbol *symbol = symbol_create(function->value, SYMBOL_FUNCTION);
            if (!symbol) return false;
            if (!symbol_table_insert(interpreter->symbols, symbol)) {
                free(symbol->name);
                free(symbol);
                return false;
            }
            symbol->declaration = function;
        }
    }

    for (size_t i = 0; i < node->child_count; i++) {
        if (!interpreter_execute_node(interpreter, node->children[i])) {
            return false;
        }
    }
    
    return true;
}

static bool interpreter_execute_bloque(Interpreter *interpreter, ASTNode *node) {
    if (!node) return true;
    
    for (size_t i = 0; i < node->child_count; i++) {
        if (!interpreter_execute_node(interpreter, node->children[i])) {
            return false;
        }
    }
    
    return true;
}

static bool interpreter_execute_cargar(Interpreter *interpreter, ASTNode *node) {
    if (!node || !node->value) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_RUNTIME,
                         0, 0, 0, "Cargar requiere nombre de archivo");
        return false;
    }
    
    if (interpreter->dataset) {
        dataset_destruir(interpreter->dataset);
        free(interpreter->dataset);
    }
    
    interpreter->dataset = (Dataset *)calloc(1, sizeof(Dataset));
    if (!interpreter->dataset) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_MEMORY,
                         0, 0, 0, "No se pudo crear dataset");
        return false;
    }
    
    if (!dataset_cargar_csv(interpreter->dataset, node->value)) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_IO,
                         0, 0, 0, "No se pudo cargar CSV");
        return false;
    }
    
    printf("Dataset cargado: %s (%zu filas, %zu columnas)\n",
           node->value, interpreter->dataset->row_count, 
           interpreter->dataset->column_count);
    
    return true;
}

static bool interpreter_execute_exportar(Interpreter *interpreter, ASTNode *node) {
    if (!interpreter->dataset || !node->value) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_RUNTIME,
                         0, 0, 0,
                         "No hay dataset o nombre de archivo para exportar");
        return false;
    }
    
    if (!dataset_guardar_json(interpreter->dataset, node->value)) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_IO,
                         0, 0, 0, "No se pudo exportar JSON");
        return false;
    }
    
    printf("Datos exportados a: %s\n", node->value);
    return true;
}

static bool interpreter_execute_node(Interpreter *interpreter, ASTNode *node) {
    Runtime *runtime;
    if (!node) return true;
    runtime = (Runtime *)interpreter->runtime;

    if (node->type == AST_DECLARACION_FUNCION) {
        Symbol *symbol = symbol_table_lookup_local(interpreter->symbols,
                                                   node->value);
        if (!symbol) {
            symbol = symbol_create(node->value, SYMBOL_FUNCTION);
            if (!symbol) return false;
            if (!symbol_table_insert(interpreter->symbols, symbol)) {
                free(symbol->name);
                free(symbol);
                return false;
            }
        }
        symbol->declaration = node;
        return true;
    }

    if (node->type == AST_DECLARACION_VARIABLE) {
        double value;
        Symbol *symbol;
        if (node->child_count != 1 ||
            !eval_expr(interpreter, node->children[0], runtime, &value) ||
            !runtime_bind(runtime, node->value, value)) {
            return false;
        }
        symbol = symbol_table_lookup(interpreter->symbols, node->value);
        if (!symbol) {
            symbol = symbol_create(node->value, SYMBOL_VARIABLE);
            if (!symbol) return false;
            if (!symbol_table_insert(interpreter->symbols, symbol)) {
                free(symbol->name);
                free(symbol);
                return false;
            }
        }
        return true;
    }

    if (node->type == AST_ASIGNACION_VARIABLE) {
        double value;
        return node->child_count == 1 &&
               eval_expr(interpreter, node->children[0], runtime, &value) &&
               runtime_assign(runtime, node->value, value);
    }

    switch (node->type) {
        case AST_PROGRAMA:
            return interpreter_execute_program(interpreter, node);
            
        case AST_BLOQUE_ANALISIS:
        case AST_BLOQUE_LIMPIAR:
        case AST_BLOQUE_TRANSFORMAR:
        case AST_BLOQUE_FILTRAR:
        case AST_BLOQUE_AGRUPAR:
        case AST_BLOQUE_RESUMIR:
        case AST_BLOQUE_VISUALIZAR:
            return interpreter_execute_bloque(interpreter, node);
            
        case AST_LLAMADA_CARGAR:
            return interpreter_execute_cargar(interpreter, node);
            
        case AST_BLOQUE_EXPORTAR:
            return interpreter_execute_exportar(interpreter, node);
            
        default:
            for (size_t i = 0; i < node->child_count; i++) {
                if (!interpreter_execute_node(interpreter, node->children[i])) {
                    return false;
                }
            }
            return true;
    }
}

bool interpreter_run(Interpreter *interpreter) {
    if (!interpreter || !interpreter->ast) return false;
    
    return interpreter_execute_node(interpreter, interpreter->ast);
}

bool interpreter_get_number(const Interpreter *interpreter, const char *name, double *value) {
    if (!interpreter || !name || !value || !interpreter->runtime) return false;
    return runtime_lookup((Runtime *)interpreter->runtime, name, value);
}

void interpreter_destroy(Interpreter *interpreter) {
    if (!interpreter) return;
    
    if (interpreter->dataset) {
        dataset_destruir(interpreter->dataset);
        free(interpreter->dataset);
    }
    
    if (interpreter->symbols) {
        symbol_table_destroy(interpreter->symbols);
    }
    Runtime *r=(Runtime*)interpreter->runtime;
    if(r){for(size_t i=0;i<r->count;i++)free(r->items[i].name);free(r->items);free(r);}
    interpreter->runtime=NULL;
}
