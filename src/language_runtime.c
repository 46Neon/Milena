#include "language_runtime.h"

#include "array.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

#include <float.h>

#define MILENA_RUNTIME_MAX_ARRAYS 128u

typedef struct {
    const char *name;
    MilenaArray value;
} MilenaRuntimeArray;

typedef struct {
    MilenaRuntimeArray arrays[MILENA_RUNTIME_MAX_ARRAYS];
    size_t count;
    FILE *output;
} MilenaArrayRuntime;

static void runtime_error(MilenaError *error, MilenaStatus code,
                          const char *message) {
    if (error) milena_error_set(error, code, 0, 0, 0, message);
}

static void runtime_release(MilenaArrayRuntime *runtime) {
    if (!runtime) return;
    for (size_t i = 0; i < runtime->count; i++) {
        milena_array_release(&runtime->arrays[i].value);
    }
    runtime->count = 0;
}

static MilenaRuntimeArray *runtime_find(MilenaArrayRuntime *runtime,
                                        const char *name) {
    if (!runtime || !name) return NULL;
    for (size_t i = 0; i < runtime->count; i++) {
        if (runtime->arrays[i].name &&
            strcmp(runtime->arrays[i].name, name) == 0) {
            return &runtime->arrays[i];
        }
    }
    return NULL;
}

static MilenaStatus runtime_declare_array(MilenaArrayRuntime *runtime,
                                          const ASTNode *node,
                                          MilenaError *error) {
    if (!runtime || !node || node->type != AST_DECLARACION_ARRAY ||
        !node->value || node->child_count != 1 || !node->children[0] ||
        node->children[0]->type != AST_EXPRESION_ARRAY) {
        runtime_error(error, MILENA_ERR_INTERNAL,
                      "Declaración de arreglo inválida en el AST");
        return MILENA_ERR_INTERNAL;
    }
    if (runtime->count >= MILENA_RUNTIME_MAX_ARRAYS) {
        runtime_error(error, MILENA_ERR_OVERFLOW,
                      "Se excedió el límite temporal de arreglos del runtime");
        return MILENA_ERR_OVERFLOW;
    }
    if (runtime_find(runtime, node->value)) {
        runtime_error(error, MILENA_ERR_DATA,
                      "El arreglo ya fue declarado en el runtime");
        return MILENA_ERR_DATA;
    }

    const ASTNode *literal = node->children[0];
    if (literal->child_count == 0) {
        runtime_error(error, MILENA_ERR_DATA,
                      "Un literal de arreglo no puede estar vacío");
        return MILENA_ERR_DATA;
    }

    MilenaArray value = {0};
    MilenaStatus status;
    if (literal->zeros_constructor) {
        size_t *shape = calloc(literal->child_count, sizeof(*shape));
        if (!shape) {
            runtime_error(error, MILENA_ERR_MEMORY,
                          "Sin memoria para las dimensiones de ceros");
            return MILENA_ERR_MEMORY;
        }
        for (size_t i = 0; i < literal->child_count; i++) {
            const ASTNode *dimension = literal->children[i];
            if (!dimension || dimension->type != AST_EXPRESION_LITERAL ||
                !isfinite(dimension->number_value) ||
                dimension->number_value <= 0.0 ||
                dimension->number_value > (double)SIZE_MAX ||
                floor(dimension->number_value) != dimension->number_value) {
                free(shape);
                runtime_error(error, MILENA_ERR_TYPE,
                              "Las dimensiones de ceros deben ser enteros positivos");
                return MILENA_ERR_TYPE;
            }
            shape[i] = (size_t)dimension->number_value;
        }
        status = milena_array_zeros(&value, MILENA_DTYPE_FLOAT64,
                                    literal->child_count, shape, error);
        free(shape);
    } else {
        size_t shape[] = {literal->child_count};
        double *reals = calloc(literal->child_count, sizeof(*reals));
        int64_t *integers = calloc(literal->child_count, sizeof(*integers));
        if (!reals || !integers) {
            free(reals);
            free(integers);
            runtime_error(error, MILENA_ERR_MEMORY,
                          "Sin memoria para materializar el arreglo");
            return MILENA_ERR_MEMORY;
        }

        bool all_integers = true;
        for (size_t i = 0; i < literal->child_count; i++) {
            const ASTNode *element = literal->children[i];
            if (!element || element->type != AST_EXPRESION_LITERAL ||
                !isfinite(element->number_value)) {
                free(reals);
                free(integers);
                runtime_error(error, MILENA_ERR_TYPE,
                              "El literal de arreglo solo admite números finitos");
                return MILENA_ERR_TYPE;
            }
            double number = element->number_value;
            reals[i] = number;
            /* INT64_MAX rounds to 2^63 in double; use an exclusive upper
             * boundary to avoid an out-of-range floating-to-integer cast. */
            if (number < (double)INT64_MIN || number >= -(double)INT64_MIN ||
                trunc(number) != number) {
                all_integers = false;
            } else {
                integers[i] = (int64_t)number;
            }
        }

        status = all_integers
            ? milena_array_from_i64(&value, 1, shape, integers, error)
            : milena_array_from_f64(&value, 1, shape, reals, error);
        free(reals);
        free(integers);
    }
    if (status != MILENA_OK) return status;

    runtime->arrays[runtime->count].name = node->value;
    runtime->arrays[runtime->count].value = value;
    runtime->count++;
    return MILENA_OK;
}

static void runtime_print_shape(FILE *output, const MilenaArray *value) {
    fputc('(', output);
    for (size_t i = 0; i < value->ndim; i++) {
        if (i) fputs(", ", output);
        fprintf(output, "%zu", value->shape[i]);
    }
    fputc(')', output);
}

static void runtime_print_value(FILE *output, const MilenaArray *value) {
    const void *data = milena_array_const_data(value);
    if (value->size == 1) {
        if (value->dtype == MILENA_DTYPE_INT64) {
            fprintf(output, "%lld", (long long)*(const int64_t *)data);
        } else if (value->dtype == MILENA_DTYPE_FLOAT64) {
            fprintf(output, "%.17g", *(const double *)data);
        } else {
            fprintf(output, "<dtype %s>", milena_dtype_name(value->dtype));
        }
        return;
    }

    fputc('[', output);
    for (size_t i = 0; i < value->size; i++) {
        if (i) fputs(", ", output);
        if (value->dtype == MILENA_DTYPE_INT64) {
            fprintf(output, "%lld", (long long)((const int64_t *)data)[i]);
        } else if (value->dtype == MILENA_DTYPE_FLOAT64) {
            fprintf(output, "%.17g", ((const double *)data)[i]);
        } else {
            fputs("?", output);
        }
    }
    fputc(']', output);
}

static MilenaStatus runtime_statistic(MilenaArrayRuntime *runtime,
                                      const ASTNode *node,
                                      MilenaError *error) {
    if (!runtime || !node || node->type != AST_OPERACION_ESTADISTICA ||
        node->child_count != 1 || !node->children[0] ||
        !node->children[0]->value) {
        runtime_error(error, MILENA_ERR_INTERNAL,
                      "Operación estadística inválida en el AST");
        return MILENA_ERR_INTERNAL;
    }

    MilenaRuntimeArray *binding = runtime_find(runtime, node->children[0]->value);
    if (!binding) {
        runtime_error(error, MILENA_ERR_DATA,
                      "El arreglo solicitado no existe en el runtime");
        return MILENA_ERR_DATA;
    }

    MilenaArray result = {0};
    MilenaStatus status = MILENA_ERR_UNSUPPORTED;
    switch (node->statistical_operation) {
        case AST_ESTADISTICA_SUMA:
            status = milena_array_sum(&result, &binding->value,
                                      node->axis, node->keepdims, error);
            break;
        case AST_ESTADISTICA_MEDIA:
            status = node->axis >= 0
                ? milena_array_mean_axis(&result, &binding->value,
                                         node->axis, node->keepdims, error)
                : milena_array_mean(&result, &binding->value, error);
            break;
        case AST_ESTADISTICA_MINIMO:
            status = node->axis >= 0
                ? milena_array_min_axis(&result, &binding->value,
                                        node->axis, node->keepdims, error)
                : milena_array_min(&result, &binding->value, error);
            break;
        case AST_ESTADISTICA_MAXIMO:
            status = node->axis >= 0
                ? milena_array_max_axis(&result, &binding->value,
                                        node->axis, node->keepdims, error)
                : milena_array_max(&result, &binding->value, error);
            break;
        case AST_ESTADISTICA_VARIANZA:
            status = node->axis >= 0
                ? milena_array_variance_axis(&result, &binding->value,
                                             node->axis, node->keepdims, error)
                : milena_array_variance(&result, &binding->value, error);
            break;
        case AST_ESTADISTICA_DESVIACION:
            status = node->axis >= 0
                ? milena_array_std_axis(&result, &binding->value,
                                        node->axis, node->keepdims, error)
                : milena_array_std(&result, &binding->value, error);
            break;
        case AST_ESTADISTICA_MEDIANA:
            status = node->axis >= 0
                ? milena_array_median_axis(&result, &binding->value,
                                           node->axis, node->keepdims, error)
                : milena_array_median(&result, &binding->value, error);
            break;
        case AST_ESTADISTICA_PERCENTIL:
            status = node->axis >= 0
                ? milena_array_percentile_axis(&result, &binding->value,
                                               node->percentile, node->axis,
                                               node->keepdims, error)
                : milena_array_percentile(&result, &binding->value,
                                          node->percentile, error);
            break;
        default:
            runtime_error(error, MILENA_ERR_UNSUPPORTED,
                          "Operación estadística no soportada por el runtime");
            break;
    }
    if (status != MILENA_OK) {
        milena_array_release(&result);
        return status;
    }

    FILE *output = runtime->output ? runtime->output : stdout;
    fprintf(output, "%s(%s) = ",
            ast_stat_operation_name(node->statistical_operation), binding->name);
    runtime_print_value(output, &result);
    fprintf(output, " dtype=%s shape=", milena_dtype_name(result.dtype));
    runtime_print_shape(output, &result);
    fputc('\n', output);
    milena_array_release(&result);
    return MILENA_OK;
}

static MilenaStatus runtime_execute_block(MilenaArrayRuntime *runtime,
                                          const ASTNode *block,
                                          MilenaError *error) {
    if (!runtime || !block || block->type != AST_BLOQUE_ANALISIS) {
        runtime_error(error, MILENA_ERR_UNSUPPORTED,
                      "El programa no contiene un bloque de análisis ejecutable");
        return MILENA_ERR_UNSUPPORTED;
    }

    bool saw_array = false;
    for (size_t i = 0; i < block->child_count; i++) {
        const ASTNode *node = block->children[i];
        MilenaStatus status;
        if (node->type == AST_DECLARACION_ARRAY) {
            saw_array = true;
            status = runtime_declare_array(runtime, node, error);
        } else if (node->type == AST_OPERACION_ESTADISTICA) {
            status = runtime_statistic(runtime, node, error);
        } else {
            continue;
        }
        if (status != MILENA_OK) return status;
    }
    if (!saw_array) {
        runtime_error(error, MILENA_ERR_UNSUPPORTED,
                      "El bloque no contiene arreglos ejecutables");
        return MILENA_ERR_UNSUPPORTED;
    }
    return MILENA_OK;
}

MilenaStatus milena_run_array_program(const char *source, FILE *output,
                                      MilenaError *error) {
    if (!source) {
        runtime_error(error, MILENA_ERR_ARGUMENT,
                      "El programa Milena no puede ser nulo");
        return MILENA_ERR_ARGUMENT;
    }
    if (error) milena_error_clear(error);

    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    if (!program || parser.has_error) {
        if (error) *error = parser.error;
        ast_destroy(program);
        parser_release(&parser);
        return error && error->code != MILENA_OK ? error->code : MILENA_ERR_PARSE;
    }

    MilenaArrayRuntime runtime = {0};
    runtime.output = output ? output : stdout;
    MilenaStatus status = MILENA_ERR_UNSUPPORTED;
    for (size_t i = 0; i < program->child_count; i++) {
        if (program->children[i]->type == AST_BLOQUE_ANALISIS) {
            status = runtime_execute_block(&runtime, program->children[i], error);
            break;
        }
    }

    runtime_release(&runtime);
    ast_destroy(program);
    parser_release(&parser);
    return status;
}
