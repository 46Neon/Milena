#include "language_runtime.h"

#include "array.h"
#include "dataset.h"
#include "analysis.h"
#include "schema.h"
#include "table.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

#include <float.h>
#include <ctype.h>

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

typedef struct {
    Dataset dataset;
    bool loaded;
} MilenaDatasetRuntime;

static bool dataset_runtime_file_exists(const char *path) {
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return false;
    fclose(file);
    return true;
}

static bool dataset_runtime_absolute(const char *path) {
    return path && (path[0] == '/' ||
                    (isalpha((unsigned char)path[0]) && path[1] == ':' &&
                     (path[2] == '\\' || path[2] == '/')));
}

static MilenaStatus dataset_runtime_path(const char *requested,
                                         const char *script_filename,
                                         bool output,
                                         char *resolved, size_t resolved_size,
                                         MilenaError *error) {
    if (!requested || !resolved || resolved_size == 0) {
        runtime_error(error, MILENA_ERR_ARGUMENT, "Ruta de dataset inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (dataset_runtime_absolute(requested)) {
        if (strlen(requested) + 1 > resolved_size) {
            runtime_error(error, MILENA_ERR_OVERFLOW, "Ruta demasiado larga");
            return MILENA_ERR_OVERFLOW;
        }
        strcpy(resolved, requested);
        if (!output && !dataset_runtime_file_exists(resolved)) {
            runtime_error(error, MILENA_ERR_IO, "No se pudo abrir el dataset");
            return MILENA_ERR_IO;
        }
        return MILENA_OK;
    }
    if (!output && dataset_runtime_file_exists(requested)) {
        if (strlen(requested) + 1 > resolved_size) {
            runtime_error(error, MILENA_ERR_OVERFLOW, "Ruta demasiado larga");
            return MILENA_ERR_OVERFLOW;
        }
        strcpy(resolved, requested);
        return MILENA_OK;
    }

    char base[1024] = ".";
    if (script_filename && script_filename[0]) {
        size_t length = strlen(script_filename);
        if (length >= sizeof(base)) {
            runtime_error(error, MILENA_ERR_OVERFLOW, "Ruta del script demasiado larga");
            return MILENA_ERR_OVERFLOW;
        }
        memcpy(base, script_filename, length + 1);
        char *slash = strrchr(base, '/');
        if (slash) {
            if (slash == base) base[1] = '\0';
            else *slash = '\0';
        } else {
            strcpy(base, ".");
        }
    }
    int written = snprintf(resolved, resolved_size, "%s/%s", base, requested);
    if (written < 0 || (size_t)written >= resolved_size) {
        runtime_error(error, MILENA_ERR_OVERFLOW, "Ruta demasiado larga");
        return MILENA_ERR_OVERFLOW;
    }
    if (!output && !dataset_runtime_file_exists(resolved)) {
        runtime_error(error, MILENA_ERR_IO, "No se pudo abrir el dataset indicado");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

static const ASTNode *dataset_runtime_find_child(const ASTNode *block,
                                                 ASTNodeType type) {
    if (!block) return NULL;
    for (size_t i = 0; i < block->child_count; i++) {
        if (block->children[i] && block->children[i]->type == type) {
            return block->children[i];
        }
    }
    return NULL;
}

static MilenaStatus dataset_runtime_transform(const ASTNode *block,
                                              Dataset *dataset,
                                              MilenaError *error) {
    if (!block || !dataset) return MILENA_ERR_ARGUMENT;
    for (size_t i = 0; i < block->child_count; i++) {
        const ASTNode *command = block->children[i];
        if (!command) continue;
        if (command->type == AST_COMANDO_TOTAL) {
            /* El producto se ejecuta sobre MilenaTable después de materializar
             * el esquema; aquí solo se conserva el comando AST. */
            (void)dataset;
        } else if (command->type == AST_COMANDO_PERIODO) {
            /* La extracción se ejecuta sobre MilenaTable después de cargar
             * el esquema tipado. */
            (void)dataset;
        }
    }
    return MILENA_OK;
}

static MilenaStatus dataset_runtime_clean(const ASTNode *block,
                                          Dataset *dataset,
                                          MilenaError *error) {
    if (!block || !dataset) return MILENA_ERR_ARGUMENT;
    for (size_t i = 0; i < block->child_count; i++) {
        const ASTNode *command = block->children[i];
        if (!command || !command->value) continue;
        if (command->type == AST_COMANDO_NULOS &&
            strcmp(command->value, "eliminar") == 0) {
            /* La eliminación de nulos pertenece ahora a MilenaTable; se
             * ejecuta después de materializar el esquema tipado. */
            (void)dataset;
        } else if (command->type == AST_COMANDO_DUPLICADOS &&
                   strcmp(command->value, "eliminar") == 0) {
            MilenaStatus status = dataset_remove_duplicates(dataset, error);
            if (status != MILENA_OK) return status;
        } else if (command->type == AST_COMANDO_NULOS ||
                   command->type == AST_COMANDO_DUPLICADOS) {
            runtime_error(error, MILENA_ERR_UNSUPPORTED,
                          "La acción de limpieza todavía no está integrada");
            return MILENA_ERR_UNSUPPORTED;
        }
    }
    return MILENA_OK;
}

MilenaStatus milena_run_dataset_program(const char *source,
                                        const char *script_filename,
                                        FILE *output,
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

    const ASTNode *analysis = NULL;
    for (size_t i = 0; i < program->child_count; i++) {
        if (program->children[i]->type == AST_BLOQUE_ANALISIS) {
            analysis = program->children[i];
            break;
        }
    }
    const ASTNode *load = dataset_runtime_find_child(analysis, AST_LLAMADA_CARGAR);
    if (!analysis || !load || !load->value) {
        ast_destroy(program);
        parser_release(&parser);
        runtime_error(error, MILENA_ERR_PARSE,
                      "El análisis necesita dataset cargar datos(\"...\")");
        return MILENA_ERR_PARSE;
    }

    char input[2048];
    MilenaStatus status = dataset_runtime_path(load->value, script_filename, false,
                                               input, sizeof(input), error);
    if (status != MILENA_OK) {
        ast_destroy(program);
        parser_release(&parser);
        return status;
    }

    MilenaDatasetRuntime runtime = {0};
    dataset_init(&runtime.dataset);
    status = dataset_load_csv(&runtime.dataset, input, ',', error);
    if (status == MILENA_OK) runtime.loaded = true;
    if (status == MILENA_OK) {
        for (size_t i = 0; i < analysis->child_count; i++) {
            const ASTNode *node = analysis->children[i];
            if (!node) continue;
            if (node->type == AST_BLOQUE_LIMPIAR) {
                status = dataset_runtime_clean(node, &runtime.dataset, error);
            } else if (node->type == AST_BLOQUE_TRANSFORMAR) {
                status = dataset_runtime_transform(node, &runtime.dataset, error);
            }
            if (status != MILENA_OK) break;
        }
    }

    char output_path[2048];
    const ASTNode *export_node = dataset_runtime_find_child(analysis,
                                                              AST_BLOQUE_EXPORTAR);
    const char *requested_output = export_node && export_node->value
        ? export_node->value : "reporte_dataset.json";
    if (status == MILENA_OK) {
        status = dataset_runtime_path(requested_output, script_filename, true,
                                      output_path, sizeof(output_path), error);
    }
    MilenaSchema schema;
    schema_init(&schema);
    if (status == MILENA_OK) {
        for (size_t i = 0; i < analysis->child_count; i++) {
            const ASTNode *node = analysis->children[i];
            if (!node || !node->value) continue;
            if (node->type != AST_DECLARACION_VARIABLE &&
                node->type != AST_DECLARACION_ENTRADA &&
                node->type != AST_DECLARACION_SALIDA) continue;
            MilenaVariableType type = MILENA_VAR_TEXT;
            if (node->type_name) {
                if (strcmp(node->type_name, "numerica") == 0) type = MILENA_VAR_NUMERIC;
                else if (strcmp(node->type_name, "categorica") == 0) type = MILENA_VAR_CATEGORICAL;
                else if (strcmp(node->type_name, "binaria") == 0) type = MILENA_VAR_BINARY;
                else if (strcmp(node->type_name, "fecha") == 0 ||
                         strcmp(node->type_name, "texto") == 0) type = MILENA_VAR_TEXT;
            }
            MilenaVariableRole role = MILENA_ROLE_FEATURE;
            if (node->type == AST_DECLARACION_ENTRADA) {
                role = MILENA_ROLE_CATEGORICAL_INPUT;
                type = MILENA_VAR_CATEGORICAL;
            } else if (node->type == AST_DECLARACION_SALIDA) {
                role = MILENA_ROLE_BINARY_OUTPUT;
                type = MILENA_VAR_BINARY;
            }
            status = schema_add(&schema, node->value, type, role, error);
            if (status != MILENA_OK) break;
        }
        if (status == MILENA_OK) {
            /* Las columnas derivadas por transformar también son parte del
             * esquema canónico; no se dejan fuera del reporte. */
            for (size_t i = 0; i < runtime.dataset.column_count; i++) {
                const char *name = runtime.dataset.headers[i];
                if (schema_index(&schema, name) >= 0) continue;
                MilenaVariableType type = strcmp(name, "total") == 0
                    ? MILENA_VAR_NUMERIC : MILENA_VAR_TEXT;
                status = schema_add(&schema, name, type,
                                    MILENA_ROLE_FEATURE, error);
                if (status != MILENA_OK) break;
            }
        }
    }
    MilenaTable canonical_table;
    milena_table_init(&canonical_table);
    if (status == MILENA_OK) {
        /* El Dataset deja de ser el valor final: se valida y materializa en
         * la tabla tipada común antes de producir el reporte. */
        status = milena_table_from_dataset(&canonical_table, &runtime.dataset,
                                           &schema, error);
    }
    if (status == MILENA_OK) {
        for (size_t i = 0; i < analysis->child_count; i++) {
            const ASTNode *block = analysis->children[i];
            if (!block) continue;
            if (block->type == AST_BLOQUE_LIMPIAR) {
                for (size_t j = 0; j < block->child_count; j++) {
                    const ASTNode *command = block->children[j];
                    if (!command || !command->value ||
                        strcmp(command->value, "eliminar") != 0) continue;
                    MilenaTable cleaned;
                    milena_table_init(&cleaned);
                    if (command->type == AST_COMANDO_NULOS) {
                        status = milena_table_drop_null(&cleaned, &canonical_table, error);
                    } else if (command->type == AST_COMANDO_DUPLICADOS) {
                        status = milena_table_drop_duplicates(&cleaned, &canonical_table, error);
                    } else {
                        milena_table_destroy(&cleaned);
                        continue;
                    }
                    if (status == MILENA_OK) {
                        milena_table_swap(&canonical_table, &cleaned);
                    }
                    milena_table_destroy(&cleaned);
                    if (status != MILENA_OK) break;
                }
            } else if (block->type == AST_BLOQUE_TRANSFORMAR) {
                for (size_t j = 0; j < block->child_count; j++) {
                    const ASTNode *command = block->children[j];
                    if (!command || !command->value) continue;
                    if (command->type == AST_COMANDO_TOTAL) {
                        char left[128] = {0}, right[128] = {0};
                        if (sscanf(command->value, " %127s * %127s", left, right) != 2) {
                            runtime_error(error, MILENA_ERR_PARSE,
                                          "La transformación total debe tener la forma columna * columna");
                            status = MILENA_ERR_PARSE;
                            break;
                        }
                        status = milena_table_add_product(&canonical_table, left, right,
                                                          "total", error);
                        if (status == MILENA_OK) {
                            status = schema_add(&schema, "total", MILENA_VAR_NUMERIC,
                                                MILENA_ROLE_FEATURE, error);
                        }
                    } else if (command->type == AST_COMANDO_PERIODO) {
                        if (strcmp(command->value, "mes de fecha") != 0) {
                            runtime_error(error, MILENA_ERR_UNSUPPORTED,
                                          "Solo se admite extraer el mes de fecha");
                            status = MILENA_ERR_UNSUPPORTED;
                            break;
                        }
                        status = milena_table_add_month(&canonical_table, "fecha",
                                                        "periodo", error);
                        if (status == MILENA_OK) {
                            status = schema_add(&schema, "periodo", MILENA_VAR_TEXT,
                                                MILENA_ROLE_FEATURE, error);
                        }
                    }
                    if (status != MILENA_OK) break;
                }
            } else if (block->type == AST_BLOQUE_AGRUPAR) {
                const ASTNode *group_key = NULL;
                const ASTNode *summaries[16];
                size_t summary_count = 0;
                for (size_t j = 0; j < block->child_count; j++) {
                    if (block->children[j]->type == AST_AGRUPACION_POR) {
                        group_key = block->children[j];
                    } else if (block->children[j]->type == AST_RESUMEN_METRICA &&
                               summary_count < 16) {
                        summaries[summary_count++] = block->children[j];
                    }
                }
                MilenaAggregateSpec specifications[16];
                MilenaAggregateOp operations[16];
                char value_columns[16][128];
                char metrics[16][32];
                bool valid_specifications = group_key != NULL && summary_count > 0;
                for (size_t j = 0; valid_specifications && j < summary_count; j++) {
                    if (!summaries[j]->value ||
                        sscanf(summaries[j]->value, "%31[^:]:%127s", metrics[j],
                               value_columns[j]) != 2) {
                        valid_specifications = false;
                        break;
                    }
                    if (strcmp(metrics[j], "suma") == 0) operations[j] = MILENA_AGG_SUM;
                    else if (strcmp(metrics[j], "media") == 0) operations[j] = MILENA_AGG_MEAN;
                    else if (strcmp(metrics[j], "minimo") == 0) operations[j] = MILENA_AGG_MIN;
                    else if (strcmp(metrics[j], "maximo") == 0) operations[j] = MILENA_AGG_MAX;
                    else if (strcmp(metrics[j], "conteo") == 0) operations[j] = MILENA_AGG_COUNT;
                    else {
                        runtime_error(error, MILENA_ERR_UNSUPPORTED,
                                      "Métrica de agrupación no soportada");
                        status = MILENA_ERR_UNSUPPORTED;
                        valid_specifications = false;
                        break;
                    }
                    specifications[j].value_column = value_columns[j];
                    specifications[j].operation = operations[j];
                    specifications[j].output_name = NULL;
                }
                if (!valid_specifications && status == MILENA_OK) {
                    runtime_error(error, MILENA_ERR_PARSE,
                                  "La agrupación requiere #por y una o más métricas");
                    status = MILENA_ERR_PARSE;
                }
                if (status == MILENA_OK) {
                    const char *key_names[1] = {group_key->value};
                    MilenaTable grouped;
                    milena_table_init(&grouped);
                    status = milena_table_group_by(&grouped, &canonical_table,
                                                   key_names, 1,
                                                   specifications, summary_count,
                                                   error);
                    if (status == MILENA_OK) {
                        milena_table_swap(&canonical_table, &grouped);
                        for (size_t j = 0; j < summary_count; j++) {
                            char aggregate_name[160];
                            const char *suffix = metrics[j];
                            (void)snprintf(aggregate_name, sizeof(aggregate_name),
                                           "%s_%s", value_columns[j], suffix);
                            status = schema_add(&schema, aggregate_name,
                                                MILENA_VAR_NUMERIC,
                                                MILENA_ROLE_FEATURE, error);
                            if (status != MILENA_OK) break;
                        }
                    }
                    milena_table_destroy(&grouped);
                }
            } else if (block->type == AST_BLOQUE_RESUMIR) {
                MilenaAggregateSpec specifications[16];
                MilenaAggregateOp operations[16];
                char value_columns[16][128];
                char metrics[16][32];
                size_t summary_count = block->child_count > 16 ? 16 : block->child_count;
                bool valid_specifications = summary_count > 0;
                for (size_t j = 0; valid_specifications && j < summary_count; j++) {
                    const ASTNode *summary = block->children[j];
                    if (!summary || summary->type != AST_RESUMEN_METRICA ||
                        !summary->value ||
                        sscanf(summary->value, "%31[^:]:%127s", metrics[j],
                               value_columns[j]) != 2) {
                        valid_specifications = false;
                        break;
                    }
                    if (strcmp(metrics[j], "suma") == 0) operations[j] = MILENA_AGG_SUM;
                    else if (strcmp(metrics[j], "media") == 0) operations[j] = MILENA_AGG_MEAN;
                    else if (strcmp(metrics[j], "minimo") == 0) operations[j] = MILENA_AGG_MIN;
                    else if (strcmp(metrics[j], "maximo") == 0) operations[j] = MILENA_AGG_MAX;
                    else if (strcmp(metrics[j], "conteo") == 0) operations[j] = MILENA_AGG_COUNT;
                    else {
                        runtime_error(error, MILENA_ERR_UNSUPPORTED,
                                      "Métrica de resumen no soportada");
                        status = MILENA_ERR_UNSUPPORTED;
                        valid_specifications = false;
                        break;
                    }
                    specifications[j].value_column = value_columns[j];
                    specifications[j].operation = operations[j];
                    specifications[j].output_name = NULL;
                }
                if (!valid_specifications && status == MILENA_OK) {
                    runtime_error(error, MILENA_ERR_PARSE,
                                  "El resumen requiere una o más métricas");
                    status = MILENA_ERR_PARSE;
                }
                if (status == MILENA_OK) {
                    MilenaTable summarized;
                    milena_table_init(&summarized);
                    status = milena_table_summarize(&summarized, &canonical_table,
                                                   specifications, summary_count,
                                                   error);
                    if (status == MILENA_OK) {
                        milena_table_swap(&canonical_table, &summarized);
                        for (size_t j = 0; j < summary_count; j++) {
                            char aggregate_name[160];
                            (void)snprintf(aggregate_name, sizeof(aggregate_name),
                                           "%s_%s", value_columns[j], metrics[j]);
                            status = schema_add(&schema, aggregate_name,
                                                MILENA_VAR_NUMERIC,
                                                MILENA_ROLE_FEATURE, error);
                            if (status != MILENA_OK) break;
                        }
                    }
                    milena_table_destroy(&summarized);
                }
            } else if (block->type == AST_BLOQUE_FILTRAR) {
                const ASTNode *condition = block->child_count > 0 ? block->children[0] : NULL;
                char column[128] = {0}, operator_text[3] = {0};
                double threshold = 0.0;
                if (!condition || !condition->value ||
                    sscanf(condition->value, " %127s %2s %lf", column,
                           operator_text, &threshold) != 3) {
                    runtime_error(error, MILENA_ERR_PARSE,
                                  "La condición debe tener la forma columna operador número");
                    status = MILENA_ERR_PARSE;
                } else {
                    MilenaTable filtered;
                    milena_table_init(&filtered);
                    status = milena_table_filter_numeric(&filtered, &canonical_table,
                                                         column, operator_text,
                                                         threshold, error);
                    if (status == MILENA_OK) milena_table_swap(&canonical_table, &filtered);
                    milena_table_destroy(&filtered);
                }
            }
            if (status != MILENA_OK) break;
        }
    }
    Dataset canonical_dataset;
    dataset_init(&canonical_dataset);
    if (status == MILENA_OK) {
        /* El reporte consume el valor reconstruido desde la tabla, no la
         * representación CSV heredada que se usó para cargar. */
        status = milena_dataset_from_table(&canonical_dataset,
                                           &canonical_table, error);
    }
    if (status == MILENA_OK) {
        status = analysis_dataset_report(&canonical_dataset, &schema,
                                         output_path, error);
    }
    size_t final_rows = canonical_dataset.row_count;
    size_t final_columns = canonical_dataset.column_count;
    dataset_destroy(&canonical_dataset);
    milena_table_destroy(&canonical_table);
    if (status == MILENA_OK) {
        FILE *stream = output ? output : stdout;
        fprintf(stream, "Programa canónico ejecutado: %s\n", script_filename ? script_filename : "<memoria>");
        fprintf(stream, "Filas: %zu | Columnas: %zu | Salida: %s\n",
                final_rows, final_columns, output_path);
    }

    schema_destroy(&schema);
    if (runtime.loaded) dataset_destroy(&runtime.dataset);
    ast_destroy(program);
    parser_release(&parser);
    return status;
}
