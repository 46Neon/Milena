#include "interpreter.h"

bool interpreter_init(Interpreter *interpreter, ASTNode *ast) {
    if (!interpreter) return false;
    
    interpreter->ast = ast;
    interpreter->symbols = symbol_table_create();
    if (!interpreter->symbols) return false;
    
    interpreter->dataset = NULL;
    interpreter->has_error = false;
    milena_error_init(&interpreter->error);
    
    return true;
}

static bool interpreter_execute_node(Interpreter *interpreter, ASTNode *node);

static bool interpreter_execute_program(Interpreter *interpreter, ASTNode *node) {
    if (!node) return true;
    
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
                      "Cargar requiere nombre de archivo", 0, 0);
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
                      "No se pudo crear dataset", 0, 0);
        return false;
    }
    
    if (!dataset_cargar_csv(interpreter->dataset, node->value)) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_IO,
                      "No se pudo cargar CSV", 0, 0);
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
                      "No hay dataset o nombre de archivo para exportar", 0, 0);
        return false;
    }
    
    if (!dataset_guardar_json(interpreter->dataset, node->value)) {
        interpreter->has_error = true;
        milena_error_set(&interpreter->error, MILENA_ERROR_IO,
                      "No se pudo exportar JSON", 0, 0);
        return false;
    }
    
    printf("Datos exportados a: %s\n", node->value);
    return true;
}

static bool interpreter_execute_node(Interpreter *interpreter, ASTNode *node) {
    if (!node) return true;
    
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

void interpreter_destroy(Interpreter *interpreter) {
    if (!interpreter) return;
    
    if (interpreter->dataset) {
        dataset_destruir(interpreter->dataset);
        free(interpreter->dataset);
    }
    
    if (interpreter->symbols) {
        symbol_table_destroy(interpreter->symbols);
    }
}
