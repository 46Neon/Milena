#define _POSIX_C_SOURCE 200809L
#include "bytecode.h"

#include "analysis.h"
#include "canonical_compiler.h"
#include "dataset.h"
#include "schema.h"
#include "table.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static MilenaStatus data_runtime_fail(MilenaError *error, MilenaStatus status,
                                      const MilenaBytecodeDataPlanView *plan,
                                      const char *message) {
    size_t line = plan && plan->span_present ? plan->source_line : 0u;
    size_t column = plan && plan->span_present ? plan->source_column : 0u;
    milena_error_set(error, status, line, column, 0u, message);
    return status;
}

static MilenaStatus data_runtime_verifier_error(
    MilenaBytecodeStatus bc_status, const MilenaBytecodeDiagnostic *diagnostic,
    MilenaError *error) {
    MilenaStatus status;
    switch (bc_status) {
        case MILENA_BC_ARGUMENT: status = MILENA_ERR_ARGUMENT; break;
        case MILENA_BC_LIMIT_EXCEEDED: status = MILENA_ERR_LIMIT; break;
        case MILENA_BC_OUT_OF_MEMORY: status = MILENA_ERR_MEMORY; break;
        case MILENA_BC_BAD_TYPE: status = MILENA_ERR_TYPE; break;
        default: status = MILENA_ERR_DATA; break;
    }
    char message[MILENA_ERROR_TEXT];
    (void)snprintf(message, sizeof(message),
                   "Plan de bytecode de datos inválido en byte %zu: %s",
                   diagnostic ? diagnostic->byte_offset : 0u,
                   diagnostic && diagnostic->message[0]
                       ? diagnostic->message : milena_bytecode_status_name(bc_status));
    milena_error_set(error, status, 0u, 0u, 0u, message);
    return status;
}

static char *data_copy_string(MilenaBytecodeDataStringView view) {
    if (view.length == SIZE_MAX) return NULL;
    char *copy = (char *)malloc(view.length + 1u);
    if (!copy) return NULL;
    memcpy(copy, view.data, view.length);
    copy[view.length] = '\0';
    return copy;
}

static size_t data_effective_limit(uint32_t encoded, uint32_t requested) {
    return requested == 0u || requested > encoded ? (size_t)encoded
                                                   : (size_t)requested;
}

static bool data_is_numeric_dtype(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_INT8:
        case MILENA_DTYPE_INT16:
        case MILENA_DTYPE_INT32:
        case MILENA_DTYPE_INT64:
        case MILENA_DTYPE_UINT8:
        case MILENA_DTYPE_UINT16:
        case MILENA_DTYPE_UINT32:
        case MILENA_DTYPE_UINT64:
        case MILENA_DTYPE_FLOAT32:
        case MILENA_DTYPE_FLOAT64:
            return true;
        default:
            return false;
    }
}

static bool data_path_separator(char ch) {
#if defined(_WIN32)
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

/* Lexically canonicalize separators, `.` and resolvable `..` components. */
static char *data_lexical_path(const char *path) {
    if (!path) return NULL;
    size_t input_length = strlen(path);
    size_t allocation, marks_bytes;
    if (!milena_size_add(input_length, 2u, &allocation) ||
        !milena_size_mul(allocation, sizeof(size_t), &marks_bytes)) return NULL;
    char *result = (char *)malloc(allocation);
    size_t *marks = (size_t *)malloc(marks_bytes);
    if (!result || !marks) {
        free(result);
        free(marks);
        return NULL;
    }
    size_t used = 0u, depth = 0u, i = 0u, root_length = 0u;
    bool absolute = input_length != 0u && data_path_separator(path[0]);
#if defined(_WIN32)
    if (input_length >= 3u && path[1] == ':' && data_path_separator(path[2])) {
        result[used++] = path[0];
        result[used++] = ':';
        result[used++] = '/';
        i = 3u;
        root_length = 3u;
        absolute = true;
    } else
#endif
    if (absolute) {
        result[used++] = '/';
        root_length = 1u;
        while (i < input_length && data_path_separator(path[i])) ++i;
    }
    while (i < input_length) {
        while (i < input_length && data_path_separator(path[i])) ++i;
        if (i >= input_length) break;
        size_t start = i;
        while (i < input_length && !data_path_separator(path[i])) ++i;
        size_t length = i - start;
        if (length == 1u && path[start] == '.') continue;
        if (length == 2u && path[start] == '.' && path[start + 1u] == '.') {
            if (depth != 0u) {
                size_t previous_start = marks[depth - 1u];
                size_t previous_length = used - previous_start;
                if (!(previous_length == 2u && result[previous_start] == '.' &&
                      result[previous_start + 1u] == '.')) {
                    used = previous_start;
                    if (used > root_length && result[used - 1u] == '/') --used;
                    --depth;
                    continue;
                }
            }
            if (absolute) continue;
        }
        if (used != 0u && result[used - 1u] != '/') result[used++] = '/';
        marks[depth++] = used;
        memcpy(result + used, path + start, length);
        used += length;
    }
    if (used == 0u) result[used++] = absolute ? '/' : '.';
    result[used] = '\0';
    free(marks);
    return result;
}

static bool data_same_existing_file(const char *input_path,
                                    const char *output_path,
                                    bool *same, MilenaError *error) {
    struct stat input_stat, output_stat;
    *same = false;
    if (stat(input_path, &input_stat) != 0) {
        milena_error_set(error, MILENA_ERR_IO, 0u, 0u, 0u,
                         "No se pudo validar el archivo CSV de entrada");
        return false;
    }
    if (stat(output_path, &output_stat) == 0) {
        *same = input_stat.st_dev == output_stat.st_dev &&
                input_stat.st_ino == output_stat.st_ino;
        return true;
    }
    if (errno == ENOENT || errno == ENOTDIR) return true;
    milena_error_set(error, MILENA_ERR_IO, 0u, 0u, 0u,
                     "No se pudo validar el destino de exportación");
    return false;
}

#if defined(_WIN32)
static bool data_create_temp(const char *output_path, char **temp_path,
                             int *descriptor) {
    char directory[MAX_PATH];
    size_t length = strlen(output_path);
    if (length >= sizeof(directory)) return false;
    memcpy(directory, output_path, length + 1u);
    char *slash = strrchr(directory, '/');
    char *backslash = strrchr(directory, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (!slash) strcpy(directory, ".");
    else if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    char generated[MAX_PATH];
    if (GetTempFileNameA(directory, "mln", 0u, generated) == 0u) return false;
    *temp_path = milena_strdup(generated);
    if (!*temp_path) {
        (void)DeleteFileA(generated);
        return false;
    }
    *descriptor = -1;
    return true;
}
static int data_close_temp(int descriptor) { (void)descriptor; return 0; }
static bool data_publish_temp(const char *temp_path, const char *output_path) {
    return MoveFileExA(temp_path, output_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static int data_remove_temp(const char *path) { return DeleteFileA(path) ? 0 : -1; }
#else
static bool data_create_temp(const char *output_path, char **temp_path,
                             int *descriptor) {
    static const char suffix[] = ".milena-tmp-XXXXXX";
    size_t length = strlen(output_path), total;
    if (!milena_size_add(length, sizeof(suffix), &total)) return false;
    char *name = (char *)malloc(total);
    if (!name) return false;
    memcpy(name, output_path, length);
    memcpy(name + length, suffix, sizeof(suffix));
    int fd = mkstemp(name);
    if (fd < 0) {
        free(name);
        return false;
    }
    *temp_path = name;
    *descriptor = fd;
    return true;
}
static int data_close_temp(int descriptor) { return close(descriptor); }
static bool data_publish_temp(const char *temp_path, const char *output_path) {
    return rename(temp_path, output_path) == 0;
}
static int data_remove_temp(const char *path) { return unlink(path); }
#endif

MilenaStatus milena_bytecode_run_data(
    const uint8_t *bytes,
    size_t length,
    const char *source_filename,
    const MilenaBytecodeDataRunLimits *run_limits,
    MilenaError *error) {
    MilenaError local_error;
    if (!error) error = &local_error;
    milena_error_clear(error);

    MilenaBytecodeDataPlanView plan;
    MilenaBytecodeDiagnostic diagnostic;
    MilenaBytecodeStatus bc_status = milena_bytecode_verify_data(
        bytes, length, &plan, &diagnostic);
    if (bc_status != MILENA_BC_OK)
        return data_runtime_verifier_error(bc_status, &diagnostic, error);

    char *source_path = data_copy_string(plan.source_path);
    char *export_path = data_copy_string(plan.export_path);
    char *input_name = data_copy_string(plan.input_column);
    char *result_name = data_copy_string(plan.result_column);
    if (!source_path || !export_path || !input_name || !result_name) {
        free(source_path); free(export_path); free(input_name); free(result_name);
        return data_runtime_fail(error, MILENA_ERR_MEMORY, &plan,
                                 "Sin memoria para copiar el plan de datos");
    }

    size_t input_capacity, output_capacity;
    size_t context_length = source_filename ? strlen(source_filename) : 0u;
    if (!milena_size_add(context_length, plan.source_path.length, &input_capacity) ||
        !milena_size_add(input_capacity, 16u, &input_capacity) ||
        !milena_size_add(context_length, plan.export_path.length, &output_capacity) ||
        !milena_size_add(output_capacity, 16u, &output_capacity)) {
        free(source_path); free(export_path); free(input_name); free(result_name);
        return data_runtime_fail(error, MILENA_ERR_OVERFLOW, &plan,
                                 "La ruta resuelta excede el tamaño representable");
    }
    char *resolved_input = (char *)malloc(input_capacity);
    char *resolved_output = (char *)malloc(output_capacity);
    if (!resolved_input || !resolved_output) {
        free(source_path); free(export_path); free(input_name); free(result_name);
        free(resolved_input); free(resolved_output);
        return data_runtime_fail(error, MILENA_ERR_MEMORY, &plan,
                                 "Sin memoria para resolver las rutas del plan");
    }

    MilenaStatus status = dataset_resolve_runtime_path(
        source_path, source_filename, false, resolved_input, input_capacity, error);
    if (status == MILENA_OK)
        status = dataset_resolve_runtime_path(export_path, source_filename, true,
                                              resolved_output, output_capacity, error);
    char *lexical_input = NULL, *lexical_output = NULL;
    if (status == MILENA_OK) {
        lexical_input = data_lexical_path(resolved_input);
        lexical_output = data_lexical_path(resolved_output);
        if (!lexical_input || !lexical_output) {
            status = data_runtime_fail(error, MILENA_ERR_MEMORY, &plan,
                                       "Sin memoria al validar alias de rutas");
        } else if (strcmp(lexical_input, lexical_output) == 0) {
            status = data_runtime_fail(error, MILENA_ERR_ARGUMENT, &plan,
                                       "La salida no puede ser el mismo archivo que la entrada");
        }
    }
    bool same_file = false;
    if (status == MILENA_OK &&
        !data_same_existing_file(resolved_input, resolved_output, &same_file, error))
        status = error->code == MILENA_OK ? MILENA_ERR_IO : error->code;
    if (status == MILENA_OK && same_file)
        status = data_runtime_fail(error, MILENA_ERR_ARGUMENT, &plan,
                                   "La salida no puede ser un alias del archivo de entrada");

    Dataset dataset;
    dataset_init(&dataset);
    MilenaSchema schema;
    schema_init(&schema);
    MilenaTable input_table = {0};
    MilenaTable summary_table = {0};
    milena_table_init(&input_table);
    milena_table_init(&summary_table);
    char *temporary_path = NULL;
    int temporary_descriptor = -1;
    bool temporary_exists = false;

    size_t max_file_bytes = data_effective_limit(
        plan.limits.max_input_file_bytes,
        run_limits ? run_limits->max_input_file_bytes : 0u);
    size_t max_rows = data_effective_limit(
        plan.limits.max_input_data_rows,
        run_limits ? run_limits->max_input_data_rows : 0u);
    size_t max_columns = data_effective_limit(
        plan.limits.max_input_columns,
        run_limits ? run_limits->max_input_columns : 0u);
    size_t max_field_bytes = data_effective_limit(
        plan.limits.max_csv_field_bytes,
        run_limits ? run_limits->max_csv_field_bytes : 0u);
    DatasetLimits loader_limits = {max_rows, max_columns, max_field_bytes};

    if (status == MILENA_OK)
        status = dataset_load_csv_with_limits_and_byte_budget(
            &dataset, resolved_input, ',', &loader_limits, max_file_bytes, error);
    int input_column = status == MILENA_OK
        ? dataset_column_index(&dataset, input_name) : -1;
    if (status == MILENA_OK && input_column < 0) {
        status = data_runtime_fail(error, MILENA_ERR_DATA, &plan,
                                   "El CSV no contiene el encabezado numérico declarado");
    }
    /* Table conversion intentionally maps malformed numeric cells to null for
     * ordinary data workflows. A bytecode plan declares this input numeric, so
     * reject non-empty malformed values before that lossy conversion. */
    for (size_t row = 0u; status == MILENA_OK && row < dataset.row_count; ++row) {
        const char *value = dataset.rows[row][(size_t)input_column];
        double parsed;
        if (value && value[0] != '\0' &&
            milena_parse_double(value, &parsed) != MILENA_OK) {
            milena_error_set(error, MILENA_ERR_TYPE,
                             plan.span_present ? plan.source_line : 0u,
                             plan.span_present ? plan.source_column : 0u,
                             row + 1u,
                             "La columna numérica declarada contiene un valor no numérico");
            status = MILENA_ERR_TYPE;
        }
    }
    if (status == MILENA_OK)
        status = schema_add(&schema, input_name, MILENA_VAR_NUMERIC,
                            MILENA_ROLE_FEATURE, error);
    if (status == MILENA_OK)
        status = milena_table_from_dataset(&input_table, &dataset, &schema, error);
    if (status == MILENA_OK) {
        int table_column = milena_table_column_index(&input_table, input_name);
        const MilenaTableColumn *column = table_column >= 0
            ? milena_table_column(&input_table, (size_t)table_column) : NULL;
        if (!column || column->type != MILENA_COLUMN_ARRAY ||
            !data_is_numeric_dtype(column->values.dtype)) {
            status = data_runtime_fail(error, MILENA_ERR_TYPE, &plan,
                                       "La columna declarada no se materializó como numérica");
        }
    }
    if (status == MILENA_OK)
        status = milena_table_set_metadata(&input_table,
            MILENA_HIR_DATASET_PATH_METADATA, source_path, error);
    if (status == MILENA_OK) {
        const char *stamped_source = milena_table_get_metadata(
            &input_table, MILENA_HIR_DATASET_PATH_METADATA);
        if (!stamped_source || strcmp(stamped_source, source_path) != 0)
            status = data_runtime_fail(error, MILENA_ERR_DATA, &plan,
                                       "La tabla no acredita la ruta original del plan");
    }
    if (status == MILENA_OK) {
        MilenaAggregateSpec aggregate = {
            .value_column = input_name,
            .operation = plan.operation == MILENA_BYTECODE_DATA_SUM
                ? MILENA_AGG_SUM : MILENA_AGG_COUNT,
            .output_name = result_name
        };
        status = milena_table_summarize(&summary_table, &input_table,
                                        &aggregate, 1u, error);
    }
    if (status == MILENA_OK) {
        const MilenaTableColumn *column = summary_table.column_count == 1u
            ? milena_table_column(&summary_table, 0u) : NULL;
        if (summary_table.row_count != MILENA_BYTECODE_DATA_MAX_OUTPUT_ROWS ||
            summary_table.column_count != 1u || !column ||
            strcmp(column->name, result_name) != 0 ||
            column->type != MILENA_COLUMN_ARRAY ||
            !data_is_numeric_dtype(column->values.dtype)) {
            status = data_runtime_fail(error, MILENA_ERR_INTERNAL, &plan,
                                       "El kernel no produjo la forma tipada fijada por v1.3");
        }
    }

    if (status == MILENA_OK &&
        !data_create_temp(resolved_output, &temporary_path,
                          &temporary_descriptor)) {
        status = data_runtime_fail(error, MILENA_ERR_IO, &plan,
                                   "No se pudo crear el temporal junto al destino JSON");
    } else if (status == MILENA_OK) {
        temporary_exists = true;
        if (temporary_descriptor >= 0) {
            int close_status = data_close_temp(temporary_descriptor);
            temporary_descriptor = -1;
            if (close_status != 0)
                status = data_runtime_fail(error, MILENA_ERR_IO, &plan,
                                           "No se pudo cerrar el temporal del reporte");
        }
    }
    if (status == MILENA_OK)
        status = analysis_table_report(&summary_table, &schema,
                                       temporary_path, error);
    if (status == MILENA_OK && !data_publish_temp(temporary_path, resolved_output))
        status = data_runtime_fail(error, MILENA_ERR_IO, &plan,
                                   "No se pudo publicar atómicamente el reporte JSON");
    if (status == MILENA_OK) temporary_exists = false;

    if (temporary_descriptor >= 0) {
        if (data_close_temp(temporary_descriptor) != 0 && status == MILENA_OK)
            status = data_runtime_fail(error, MILENA_ERR_IO, &plan,
                                       "No se pudo cerrar el temporal del reporte");
        temporary_descriptor = -1;
    }
    if (temporary_exists && temporary_path && data_remove_temp(temporary_path) != 0 &&
        status == MILENA_OK)
        status = data_runtime_fail(error, MILENA_ERR_IO, &plan,
                                   "No se pudo retirar el reporte temporal fallido");

    if (status != MILENA_OK && error->code == MILENA_OK)
        milena_error_set(error, status,
                         plan.span_present ? plan.source_line : 0u,
                         plan.span_present ? plan.source_column : 0u,
                         0u, "Falló la ejecución del plan de datos v1.3");
    milena_table_destroy(&summary_table);
    milena_table_destroy(&input_table);
    schema_destroy(&schema);
    dataset_destroy(&dataset);
    free(source_path); free(export_path); free(input_name); free(result_name);
    free(resolved_input); free(resolved_output);
    free(lexical_input); free(lexical_output); free(temporary_path);
    return status;
}
