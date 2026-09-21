#include "script.h"
#include "analysis.h"
#include "array.h"
#include "language_runtime.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "user_functions.h"
#include <ctype.h>

static char *read_file(const char *filename, MilenaError *error) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir script");
        return NULL;
    }
    size_t cap = 4096, len = 0;
    char *text = (char *)malloc(cap);
    if (!text) { fclose(file); return NULL; }
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (len + 1 >= cap) {
            if (cap > SIZE_MAX / 2) { free(text); fclose(file); return NULL; }
            cap *= 2;
            char *tmp = (char *)realloc(text, cap);
            if (!tmp) { free(text); fclose(file); return NULL; }
            text = tmp;
        }
        text[len++] = (char)ch;
    }
    fclose(file);
    text[len] = '\0';
    return text;
}

static char *trim_left(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static const char *find_quoted_after(const char *text, const char *needle,
                                     char *out, size_t out_size) {
    const char *p = strstr(text, needle);
    if (!p) return NULL;
    p = strchr(p, '"');
    if (!p) return NULL;
    p++;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) + 1 > out_size) return NULL;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return end + 1;
}

static bool get_quoted(const char *line, size_t number, char *out, size_t out_size) {
    const char *p = line;
    for (size_t i = 0; i <= number; i++) {
        p = strchr(p, '"');
        if (!p) return false;
        p++;
        const char *end = strchr(p, '"');
        if (!end) return false;
        if (i == number) {
            if ((size_t)(end - p) + 1 > out_size) return false;
            memcpy(out, p, (size_t)(end - p));
            out[end - p] = '\0';
            return true;
        }
        p = end + 1;
    }
    return false;
}

static bool has_text(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}

static bool absolute_path(const char *path) {
    return path && (path[0] == '/' || (isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')));
}

static bool regular_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

static bool join_path(const char *base, const char *name, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s%s%s", base && base[0] ? base : ".",
                           base && base[0] && base[strlen(base) - 1] == '/' ? "" : "/",
                           name ? name : "");
    return written >= 0 && (size_t)written < out_size;
}

static bool parent_path(const char *path, char *out, size_t out_size) {
    if (!path || !path[0]) return false;
    if (strcmp(path, "/") == 0 || strcmp(path, ".") == 0) return false;
    if (strlen(path) + 1 > out_size) return false;
    strcpy(out, path);
    while (strlen(out) > 1 && out[strlen(out) - 1] == '/') out[strlen(out) - 1] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash) { strcpy(out, "."); return true; }
    if (slash == out) out[1] = '\0';
    else *slash = '\0';
    return true;
}

/*
 * Busca los datos primero desde el directorio actual y luego desde el
 * directorio del script y sus padres. Esto permite ejecutar un .milena desde
 * cualquier carpeta sin romper scripts que usan rutas relativas al proyecto.
 */
static MilenaStatus resolve_input_path(const char *script_file, const char *requested,
                                     char *resolved, size_t resolved_size,
                                     char *base, size_t base_size,
                                     MilenaError *error) {
    if (!requested || !resolved || !base || requested[0] == '\0') return MILENA_ERR_ARGUMENT;
    if (absolute_path(requested)) {
        if (strlen(requested) + 1 > resolved_size) return MILENA_ERR_OVERFLOW;
        strcpy(resolved, requested);
        if (!regular_file_exists(resolved)) goto not_found;
        char temporary[1024];
        if (strlen(requested) + 1 > sizeof(temporary)) return MILENA_ERR_OVERFLOW;
        strcpy(temporary, requested);
        char *slash = strrchr(temporary, '/');
        if (slash) { if (slash == temporary) strcpy(base, "/"); else *slash = '\0', strcpy(base, temporary); }
        else strcpy(base, ".");
        return MILENA_OK;
    }

    char script_dir[1024] = ".";
    if (script_file && script_file[0]) {
        if (strlen(script_file) + 1 > sizeof(script_dir)) return MILENA_ERR_OVERFLOW;
        strcpy(script_dir, script_file);
        char *slash = strrchr(script_dir, '/');
        if (slash) { if (slash == script_dir) script_dir[1] = '\0'; else *slash = '\0'; }
        else strcpy(script_dir, ".");
    }
    char candidate[2048];
    if (regular_file_exists(requested)) {
        if (strlen(requested) + 1 > resolved_size || 2 > base_size) return MILENA_ERR_OVERFLOW;
        strcpy(resolved, requested); strcpy(base, "."); return MILENA_OK;
    }
    char search_base[1024];
    strcpy(search_base, script_dir);
    for (size_t depth = 0; depth < 6; depth++) {
        if (!join_path(search_base, requested, candidate, sizeof(candidate))) return MILENA_ERR_OVERFLOW;
        if (regular_file_exists(candidate)) {
            if (strlen(candidate) + 1 > resolved_size || strlen(search_base) + 1 > base_size) return MILENA_ERR_OVERFLOW;
            strcpy(resolved, candidate); strcpy(base, search_base); return MILENA_OK;
        }
        char parent[1024];
        if (!parent_path(search_base, parent, sizeof(parent))) break;
        strcpy(search_base, parent);
    }

not_found:
    milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir el CSV indicado por el script");
    return MILENA_ERR_IO;
}

static MilenaStatus resolve_output_path(const char *requested, const char *base,
                                      char *resolved, size_t resolved_size) {
    if (!requested || !resolved) return MILENA_ERR_ARGUMENT;
    if (absolute_path(requested)) {
        if (strlen(requested) + 1 > resolved_size) return MILENA_ERR_OVERFLOW;
        strcpy(resolved, requested); return MILENA_OK;
    }
    if (!join_path(base, requested, resolved, resolved_size)) return MILENA_ERR_OVERFLOW;
    return MILENA_OK;
}

static MilenaVariableType parse_type(const char *text, bool *valid) {
    *valid = true;
    if (strcmp(text, "numerica") == 0 || strcmp(text, "numeric") == 0) return MILENA_VAR_NUMERIC;
    if (strcmp(text, "categorica") == 0 || strcmp(text, "categorical") == 0) return MILENA_VAR_CATEGORICAL;
    if (strcmp(text, "binaria") == 0 || strcmp(text, "binary") == 0) return MILENA_VAR_BINARY;
    if (strcmp(text, "fecha") == 0 || strcmp(text, "date") == 0) return MILENA_VAR_TEXT;
    if (strcmp(text, "texto") == 0 || strcmp(text, "text") == 0) return MILENA_VAR_TEXT;
    *valid = false;
    return MILENA_VAR_TEXT;
}

static MilenaStatus parse_schema(const char *script, MilenaSchema *schema, MilenaError *error) {
    char *copy = milena_strdup(script);
    if (!copy) return MILENA_ERR_MEMORY;
    char *line = strtok(copy, "\n\r");
    size_t line_number = 0;
    while (line) {
        line_number++;
        char *text = trim_left(line);
        if (strncmp(text, "variable ", 9) == 0) {
            char name[256], type_name[64];
            if (sscanf(text + 9, "%255s %63s", name, type_name) != 2) {
                milena_error_set(error, MILENA_ERR_PARSE, line_number, 1, 0,
                               "Sintaxis: variable nombre tipo");
                free(copy); return MILENA_ERR_PARSE;
            }
            bool valid;
            MilenaVariableType type = parse_type(type_name, &valid);
            if (!valid) {
                milena_error_set(error, MILENA_ERR_TYPE, line_number, 1, 0,
                               "Tipo de variable desconocido");
                free(copy); return MILENA_ERR_TYPE;
            }
            MilenaStatus status = schema_add(schema, name, type, MILENA_ROLE_FEATURE, error);
            if (status != MILENA_OK) { free(copy); return status; }
        } else if (strncmp(text, "entrada categorica", 18) == 0) {
            char name[256];
            if (!find_quoted_after(text, "entrada categorica", name, sizeof(name))) {
                milena_error_set(error, MILENA_ERR_PARSE, line_number, 1, 0,
                               "Sintaxis: entrada categorica \"columna\"");
                free(copy); return MILENA_ERR_PARSE;
            }
            MilenaStatus status = schema_add(schema, name, MILENA_VAR_CATEGORICAL,
                                           MILENA_ROLE_CATEGORICAL_INPUT, error);
            if (status != MILENA_OK) { free(copy); return status; }
        } else if (strncmp(text, "salida binaria", 14) == 0) {
            char name[256];
            if (!find_quoted_after(text, "salida binaria", name, sizeof(name))) {
                milena_error_set(error, MILENA_ERR_PARSE, line_number, 1, 0,
                               "Sintaxis: salida binaria \"columna\"");
                free(copy); return MILENA_ERR_PARSE;
            }
            MilenaStatus status = schema_add(schema, name, MILENA_VAR_BINARY,
                                           MILENA_ROLE_BINARY_OUTPUT, error);
            if (status != MILENA_OK) { free(copy); return status; }
        }
        line = strtok(NULL, "\n\r");
    }
    free(copy);
    if (schema->count == 0) {
        milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                       "El script debe declarar al menos una variable");
        return MILENA_ERR_PARSE;
    }
    return MILENA_OK;
}

static bool command_known(const char *text) {
    static const char *known[] = {
        "#datos", "#estadistica", "#nulos", "#duplicados", "#total",
        "#periodo", "#condicion", "#perfil_numerico"
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strncmp(text, known[i], strlen(known[i])) == 0) return true;
    }
    return false;
}

static MilenaStatus validate_commands(const char *script, MilenaError *error) {
    char *copy = milena_strdup(script);
    if (!copy) return MILENA_ERR_MEMORY;
    char *line = strtok(copy, "\n\r");
    size_t line_number = 0;
    while (line) {
        line_number++;
        char *text = trim_left(line);
        if (text[0] == '#') {
            if (!command_known(text)) {
                milena_error_set(error, MILENA_ERR_UNSUPPORTED, line_number, 1, 0,
                               "Comando Milena no reconocido; no se ignorará silenciosamente");
                free(copy);
                return MILENA_ERR_UNSUPPORTED;
            }
        }
        line = strtok(NULL, "\n\r");
    }
    free(copy);
    return MILENA_OK;
}

typedef struct {
    char name[128];
    MilenaArray array;
} ScriptArrayBinding;

static ScriptArrayBinding *find_script_array(ScriptArrayBinding *bindings,
                                             size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(bindings[i].name, name) == 0) return &bindings[i];
    }
    return NULL;
}

static MilenaStatus script_scalar_operation(MilenaArray *out,
                                             const MilenaArray *source,
                                             char operation, double scalar,
                                             MilenaError *error) {
    if (!out || !source || (operation != '+' && operation != '-' &&
                            operation != '*' && operation != '/')) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "Operación escalar de array inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (operation == '/' && scalar == 0.0) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "División de array por cero");
        return MILENA_ERR_ARGUMENT;
    }
    size_t *shape = source->ndim ? (size_t *)malloc(source->ndim * sizeof(size_t)) : NULL;
    if (source->ndim && !shape) {
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0, "Sin memoria para shape");
        return MILENA_ERR_MEMORY;
    }
    if (source->ndim) memcpy(shape, source->shape, source->ndim * sizeof(size_t));
    bool integer_result = source->dtype == MILENA_DTYPE_INT64 &&
                          operation != '/' && scalar == (double)(int64_t)scalar;
    MilenaStatus status;
    if (integer_result) {
        int64_t *values = (int64_t *)malloc(source->size * sizeof(int64_t));
        if (!values) { free(shape); return MILENA_ERR_MEMORY; }
        const int64_t *input = (const int64_t *)milena_array_const_data(source);
        int64_t value = (int64_t)scalar;
        for (size_t i = 0; i < source->size; i++) {
            if (operation == '+') values[i] = input[i] + value;
            else if (operation == '-') values[i] = input[i] - value;
            else values[i] = input[i] * value;
        }
        status = milena_array_from_i64(out, source->ndim, shape, values, error);
        free(values);
    } else {
        double *values = (double *)malloc(source->size * sizeof(double));
        if (!values) { free(shape); return MILENA_ERR_MEMORY; }
        for (size_t i = 0; i < source->size; i++) {
            double input = source->dtype == MILENA_DTYPE_FLOAT64 ?
                ((const double *)milena_array_const_data(source))[i] :
                (double)((const int64_t *)milena_array_const_data(source))[i];
            if (operation == '+') values[i] = input + scalar;
            else if (operation == '-') values[i] = input - scalar;
            else if (operation == '*') values[i] = input * scalar;
            else values[i] = input / scalar;
        }
        status = milena_array_from_f64(out, source->ndim, shape, values, error);
        free(values);
    }
    free(shape);
    return status;
}

static void print_array_operation(const char *left, char operation,
                                  const char *right, const MilenaArray *result) {
    printf("Operacion %s %c %s: dtype=%s, shape=(", left, operation, right,
           milena_dtype_name(result->dtype));
    for (size_t i = 0; i < result->ndim; i++) {
        if (i) printf(", ");
        printf("%zu", result->shape[i]);
    }
    printf(")\n");
}

/* COMPATIBILIDAD: parser textual histórico de arrays; solo se conserva para
 * archivos antiguos que no producen un AST canónico. */
static MilenaStatus run_legacy_array_declarations(const char *script, MilenaError *error) {
    const char *cursor = script;
    size_t declarations = 0;
    ScriptArrayBinding *bindings = NULL;
    size_t binding_count = 0;
    while (true) {
        const char *english_keyword = strstr(cursor, "array");
        const char *spanish_keyword = strstr(cursor, "arreglo");
        if (!english_keyword && !spanish_keyword) break;
        const char *keyword = !english_keyword ? spanish_keyword :
            !spanish_keyword ? english_keyword :
            english_keyword < spanish_keyword ? english_keyword : spanish_keyword;
        size_t keyword_length = keyword == spanish_keyword ? 7 : 5;
        cursor = keyword;
        const char *name_start = cursor + keyword_length;
        if (*name_start && !isspace((unsigned char)*name_start)) {
            cursor = name_start;
            continue;
        }
        while (isspace((unsigned char)*name_start)) name_start++;
        const char *name_end = name_start;
        while (isalnum((unsigned char)*name_end) || *name_end == '_') name_end++;
        if (name_end == name_start) {
            milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                             "Se esperaba nombre después de array");
            return MILENA_ERR_PARSE;
        }
        char name[128];
        size_t name_length = (size_t)(name_end - name_start);
        if (name_length >= sizeof(name)) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "Nombre de array demasiado largo");
            return MILENA_ERR_OVERFLOW;
        }
        memcpy(name, name_start, name_length);
        name[name_length] = '\0';
        const char *equal = name_end;
        while (isspace((unsigned char)*equal)) equal++;
        if (*equal != '=') {
            milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                             "Se esperaba '=' en la declaración del array");
            return MILENA_ERR_PARSE;
        }
        const char *expression = equal + 1;
        while (isspace((unsigned char)*expression)) expression++;
        bool zeros = strncmp(expression, "zeros", 5) == 0 ||
                     strncmp(expression, "ceros", 5) == 0;
        const char *start = NULL;
        const char *end = NULL;
        if (zeros) {
            start = strchr(expression, '(');
            end = start ? strchr(start + 1, ')') : NULL;
        } else {
            start = strchr(expression, '[');
            end = start ? strchr(start + 1, ']') : NULL;
        }
        if (!start || !end || end <= start + 1) {
            milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                             "Literal de array vacío o sin cierre");
            return MILENA_ERR_PARSE;
        }
        size_t count = 0;
        size_t ndim = 1;
        size_t shape[8] = {0};
        if (zeros) {
            const char *scan = start + 1;
            ndim = 0;
            count = 1;
            while (scan < end) {
                while (scan < end && isspace((unsigned char)*scan)) scan++;
                char *number_end = NULL;
                unsigned long parsed = strtoul(scan, &number_end, 10);
                if (ndim >= 8 || number_end == scan || parsed == 0 || parsed > SIZE_MAX) {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "zeros requiere dimensiones positivas");
                    return MILENA_ERR_PARSE;
                }
                shape[ndim++] = (size_t)parsed;
                if (!milena_size_mul(count, (size_t)parsed, &count)) {
                    milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                     "El tamaño de zeros desborda size_t");
                    return MILENA_ERR_OVERFLOW;
                }
                scan = number_end;
                while (scan < end && isspace((unsigned char)*scan)) scan++;
                if (scan < end && *scan != ',') {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "Se esperaba ',' entre dimensiones");
                    return MILENA_ERR_PARSE;
                }
                if (scan < end) scan++;
            }
        } else {
            const char *scan = start + 1;
            while (scan < end) {
                while (scan < end && isspace((unsigned char)*scan)) scan++;
                char *number_end = NULL;
                (void)strtod(scan, &number_end);
                if (number_end == scan || number_end > end) {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "El array solo admite números");
                    return MILENA_ERR_PARSE;
                }
                count++;
                scan = number_end;
                while (scan < end && isspace((unsigned char)*scan)) scan++;
                if (scan < end && *scan != ',') {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "Se esperaba ',' entre elementos");
                    return MILENA_ERR_PARSE;
                }
                if (scan < end) scan++;
            }
        }
        if (!zeros) shape[0] = count;
        MilenaArray array = {0};
        MilenaStatus status;
        if (zeros) {
            status = milena_array_zeros(&array, MILENA_DTYPE_FLOAT64, ndim,
                                        shape, error);
        } else {
            double *values = (double *)malloc(count * sizeof(double));
            int64_t *integers = (int64_t *)malloc(count * sizeof(int64_t));
            if (!values || !integers) {
                free(values); free(integers);
                milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                                 "No se pudo reservar el literal del array");
                return MILENA_ERR_MEMORY;
            }
            bool all_integers = true;
            const char *scan = start + 1;
            for (size_t i = 0; i < count; i++) {
                char *number_end = NULL;
                values[i] = strtod(scan, &number_end);
                integers[i] = (int64_t)values[i];
                if (values[i] != (double)integers[i]) all_integers = false;
                scan = number_end;
                while (isspace((unsigned char)*scan) || *scan == ',') scan++;
            }
            if (all_integers) {
                status = milena_array_from_i64(&array, 1, shape, integers, error);
            } else {
                status = milena_array_from_f64(&array, 1, shape, values, error);
            }
            free(values); free(integers);
        }
        if (status != MILENA_OK) return status;
        printf("Array %s: dtype=%s, shape=", name, milena_dtype_name(array.dtype));
        printf("(");
        for (size_t axis = 0; axis < array.ndim; axis++) {
            if (axis) printf(", ");
            printf("%zu", array.shape[axis]);
        }
        printf("), size=%zu\n", array.size);
        ScriptArrayBinding *grown = (ScriptArrayBinding *)realloc(
            bindings, (binding_count + 1) * sizeof(*bindings));
        if (!grown) {
            milena_array_release(&array);
            for (size_t i = 0; i < binding_count; i++) milena_array_release(&bindings[i].array);
            free(bindings);
            milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                             "No se pudo registrar el array");
            return MILENA_ERR_MEMORY;
        }
        bindings = grown;
        strcpy(bindings[binding_count].name, name);
        bindings[binding_count].array = array;
        memset(&array, 0, sizeof(array));
        binding_count++;
        declarations++;
        cursor = end + 1;
    }
    if (declarations == 0) {
        free(bindings);
        milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                         "No se encontró una declaración de array");
        return MILENA_ERR_PARSE;
    }

    const char *operations[] = {"shape(", "ndim(", "size(", "sum(", "mean(", "min(", "max(", "variance(", "std(", "median(", "percentile(",
        "forma(", "dimensiones(", "tamaño(", "suma(", "media(", "minimo(", "maximo(", "varianza(", "desviacion_estandar(", "mediana(", "percentil("};
    for (size_t operation = 0; operation < 22; operation++) {
        size_t canonical_operation = operation >= 11 ? operation - 11 : operation;
        const char *display_operation = operations[operation];
        size_t display_length = strlen(display_operation) - 1;
        const char *position = script;
        while ((position = strstr(position, operations[operation])) != NULL) {
            position += strlen(operations[operation]);
            const char *name_end = strchr(position, ')');
            if (!name_end || name_end == position || (size_t)(name_end - position) >= 128) {
                milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                 "Argumento inválido en operación de array");
                goto array_cleanup_error;
            }
            char name[128];
            double requested_percentile = 50.0;
            int requested_axis = -1;
            bool requested_keepdims = false;
            size_t length = (size_t)(name_end - position);
            memcpy(name, position, length);
            name[length] = '\0';
            if (canonical_operation == 10) {
                char *comma = strchr(name, ',');
                if (!comma) {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "percentile requiere array y porcentaje");
                    goto array_cleanup_error;
                }
                *comma++ = '\0';
                while (isspace((unsigned char)*comma)) comma++;
                char *percentile_end = NULL;
                requested_percentile = strtod(comma, &percentile_end);
                while (percentile_end && isspace((unsigned char)*percentile_end)) percentile_end++;
                if (!percentile_end || percentile_end == comma || *percentile_end != '\0') {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "Porcentaje inválido");
                    goto array_cleanup_error;
                }
            } else if (canonical_operation >= 3 && canonical_operation <= 8) {
                char *comma = strchr(name, ',');
                if (comma) {
                    *comma++ = '\0';
                    while (isspace((unsigned char)*comma)) comma++;
                    if (strncmp(comma, "eje", 3) == 0 &&
                        isspace((unsigned char)comma[3])) {
                        comma += 3;
                        while (isspace((unsigned char)*comma)) comma++;
                    }
                    char *axis_end = NULL;
                    long parsed_axis = strtol(comma, &axis_end, 10);
                    while (axis_end && isspace((unsigned char)*axis_end)) axis_end++;
                    if (axis_end && *axis_end == ',') {
                        char *keepdims_text = axis_end + 1;
                        while (isspace((unsigned char)*keepdims_text)) keepdims_text++;
                        if (strcmp(keepdims_text, "true") == 0 ||
                            strcmp(keepdims_text, "conservar dimensiones") == 0)
                            requested_keepdims = true;
                        else if (strcmp(keepdims_text, "false") == 0 ||
                                 strcmp(keepdims_text, "sin conservar dimensiones") == 0)
                            requested_keepdims = false;
                        else {
                            milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                             "keepdims debe ser true o false");
                            goto array_cleanup_error;
                        }
                        axis_end = keepdims_text + strlen(keepdims_text);
                    }
                    while (axis_end && isspace((unsigned char)*axis_end)) axis_end++;
                    if (!axis_end || axis_end == comma || *axis_end != '\0' ||
                        parsed_axis < -1 || parsed_axis > INT_MAX) {
                        milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                         "Eje inválido");
                        goto array_cleanup_error;
                    }
                    requested_axis = (int)parsed_axis;
                }
            }
            ScriptArrayBinding *binding = find_script_array(bindings, binding_count, name);
            if (!binding) {
                milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                                 "Variable de array inexistente");
                goto array_cleanup_error;
            }
            if (canonical_operation == 0) {
                printf("%.*s(%s) = (", (int)display_length, display_operation, name);
                for (size_t axis = 0; axis < binding->array.ndim; axis++) {
                    if (axis) printf(", ");
                    printf("%zu", binding->array.shape[axis]);
                }
                printf(")\n");
            } else if (canonical_operation == 1) {
                printf("%.*s(%s) = %zu\n", (int)display_length, display_operation, name, binding->array.ndim);
            } else if (canonical_operation == 2) {
                printf("%.*s(%s) = %zu\n", (int)display_length, display_operation, name, binding->array.size);
            } else if (canonical_operation == 9 || canonical_operation == 10) {
                MilenaArray result = {0};
                MilenaStatus order_status;
                if (requested_axis >= 0)
                    order_status = canonical_operation == 9 ?
                        milena_array_median_axis(&result, &binding->array, requested_axis, requested_keepdims, error) :
                        milena_array_percentile_axis(&result, &binding->array, requested_percentile, requested_axis, requested_keepdims, error);
                else
                    order_status = canonical_operation == 9 ?
                        milena_array_median(&result, &binding->array, error) :
                        milena_array_percentile(&result, &binding->array, requested_percentile, error);
                if (order_status != MILENA_OK) goto array_cleanup_error;
                printf("%.*s(%s", (int)display_length, display_operation, name);
                if (canonical_operation == 10) printf(", %.17g", requested_percentile);
                printf(") = ");
                const double *ordered = milena_array_const_data(&result);
                if (result.size == 1) printf("%.17g\n", ordered[0]);
                else {
                    printf("[");
                    for (size_t i = 0; i < result.size; i++) printf("%s%.17g", i ? ", " : "", ordered[i]);
                    printf("] shape=(");
                    for (size_t i = 0; i < result.ndim; i++) printf("%s%zu", i ? ", " : "", result.shape[i]);
                    printf(")\n");
                }
                milena_array_release(&result);
            } else {
                MilenaArray result = {0};
                MilenaStatus stat_status;
                if (canonical_operation == 3) stat_status = milena_array_sum(
                    &result, &binding->array, requested_axis, requested_keepdims, error);
                else if (canonical_operation == 4 && requested_axis >= 0) stat_status = milena_array_mean_axis(
                    &result, &binding->array, requested_axis, requested_keepdims, error);
                else if (canonical_operation == 5 && requested_axis >= 0) stat_status = milena_array_min_axis(
                    &result, &binding->array, requested_axis, requested_keepdims, error);
                else if (canonical_operation == 6 && requested_axis >= 0) stat_status = milena_array_max_axis(
                    &result, &binding->array, requested_axis, requested_keepdims, error);
                else if (canonical_operation == 7 && requested_axis >= 0) stat_status = milena_array_variance_axis(
                    &result, &binding->array, requested_axis, requested_keepdims, error);
                else if (canonical_operation == 8 && requested_axis >= 0) stat_status = milena_array_std_axis(
                    &result, &binding->array, requested_axis, requested_keepdims, error);
                else if (canonical_operation == 4) stat_status = milena_array_mean(&result, &binding->array, error);
                else if (canonical_operation == 5) stat_status = milena_array_min(&result, &binding->array, error);
                else if (canonical_operation == 6) stat_status = milena_array_max(&result, &binding->array, error);
                else if (canonical_operation == 7) stat_status = milena_array_variance(&result, &binding->array, error);
                else stat_status = milena_array_std(&result, &binding->array, error);
                if (stat_status != MILENA_OK) goto array_cleanup_error;
                const char *label = canonical_operation == 3 ? (operation >= 11 ? "suma" : "sum") :
                    canonical_operation == 4 ? (operation >= 11 ? "media" : "mean") :
                    canonical_operation == 5 ? (operation >= 11 ? "minimo" : "min") :
                    canonical_operation == 6 ? (operation >= 11 ? "maximo" : "max") :
                    canonical_operation == 7 ? (operation >= 11 ? "varianza" : "variance") :
                    (operation >= 11 ? "desviacion_estandar" : "std");
                if (canonical_operation == 3 && result.dtype == MILENA_DTYPE_INT64)
                    printf("%s(%s) = %lld\n", label, name,
                           (long long)*(const int64_t *)milena_array_const_data(&result));
                else if (result.size == 1)
                    printf("%s(%s) = %.17g\n", label, name,
                           *(const double *)milena_array_const_data(&result));
                else {
                    const double *data = (const double *)milena_array_const_data(&result);
                    printf("%s(%s) = [", label, name);
                    for (size_t i = 0; i < result.size; i++) {
                        if (i) printf(", ");
                        printf("%.17g", data[i]);
                    }
                    printf("] shape=(");
                    for (size_t i = 0; i < result.ndim; i++) {
                        if (i) printf(", ");
                        printf("%zu", result.shape[i]);
                    }
                    printf(")\n");
                }
                milena_array_release(&result);
            }
            position = name_end + 1;
        }
    }
    char *operation_script = milena_strdup(script);
    if (!operation_script) {
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0, "Sin memoria para operaciones");
        goto array_cleanup_error;
    }
    char *line = strtok(operation_script, "\n\r");
    while (line) {
        char left[128] = {0}, right[128] = {0}, operation = '\0';
        if (sscanf(line, " %127s %c %127s", left, &operation, right) == 3 &&
            (operation == '+' || operation == '-' || operation == '*' || operation == '/')) {
            size_t right_length = strlen(right);
            if (right_length && right[right_length - 1] == ';') right[right_length - 1] = '\0';
            char *right_trim = right;
            ScriptArrayBinding *left_binding = find_script_array(bindings, binding_count, left);
            if (left_binding) {
                ScriptArrayBinding *right_binding = find_script_array(bindings, binding_count, right_trim);
                MilenaArray result = {0};
                MilenaStatus operation_status;
                if (right_binding) {
                    if (operation == '+') operation_status = milena_array_add(
                        &result, &left_binding->array, &right_binding->array, error);
                    else if (operation == '-') operation_status = milena_array_subtract(
                        &result, &left_binding->array, &right_binding->array, error);
                    else if (operation == '*') operation_status = milena_array_multiply(
                        &result, &left_binding->array, &right_binding->array, error);
                    else operation_status = milena_array_divide(
                        &result, &left_binding->array, &right_binding->array, error);
                } else {
                    char *number_end = NULL;
                    double scalar = strtod(right_trim, &number_end);
                    while (number_end && isspace((unsigned char)*number_end)) number_end++;
                    if (!number_end || number_end == right_trim || *number_end != '\0') {
                        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                                         "Operando de array inexistente o inválido");
                        free(operation_script);
                        goto array_cleanup_error;
                    }
                    operation_status = script_scalar_operation(&result, &left_binding->array,
                                                               operation, scalar, error);
                }
                if (operation_status != MILENA_OK) {
                    free(operation_script);
                    goto array_cleanup_error;
                }
                print_array_operation(left, operation, right_trim, &result);
                milena_array_release(&result);
            }
        }
        line = strtok(NULL, "\n\r");
    }
    free(operation_script);
    for (size_t i = 0; i < binding_count; i++) milena_array_release(&bindings[i].array);
    free(bindings);
    return MILENA_OK;

array_cleanup_error:
    for (size_t i = 0; i < binding_count; i++) milena_array_release(&bindings[i].array);
    free(bindings);
    return error && error->code ? error->code : MILENA_ERR_INTERNAL;
}


/* Execute the numeric function section of a script before the data-script path.
 * Function declarations are deliberately isolated from the legacy command parser,
 * so adding functions cannot change statistical/array semantics. */
typedef enum {
    SCRIPT_PIPELINE_CANONICAL_ARRAY,
    SCRIPT_PIPELINE_CANONICAL_DATASET,
    SCRIPT_PIPELINE_CANONICAL_FUNCTION,
    SCRIPT_PIPELINE_CANONICAL_INVALID,
    SCRIPT_PIPELINE_LEGACY_NUMERIC_FUNCTIONS,
    SCRIPT_PIPELINE_LEGACY_ARRAY,
    SCRIPT_PIPELINE_LEGACY_DATASET
} ScriptPipeline;

/*
 * Todas las decisiones de compatibilidad pasan por este punto. El objetivo
 * de la migración es reemplazar esta detección temporal por el resultado del
 * parser, sin volver a repartir condicionales por el ejecutor.
 */
static void script_scan_canonical_ast(const ASTNode *node,
                                      bool *has_dataset,
                                      bool *has_array,
                                      bool *has_function) {
    if (!node) return;
    if (node->type == AST_LLAMADA_CARGAR) *has_dataset = true;
    if (node->type == AST_DECLARACION_ARRAY ||
        node->type == AST_EXPRESION_ARRAY) *has_array = true;
    if (node->type == AST_DECLARACION_FUNCION) *has_function = true;
    for (size_t i = 0; i < node->child_count; i++)
        script_scan_canonical_ast(node->children[i], has_dataset, has_array,
                                  has_function);
}

static ScriptPipeline script_pipeline_from_ast(const char *script) {
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, script);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    bool has_dataset = false, has_array = false, has_function = false;
    if (program && !parser.has_error)
        script_scan_canonical_ast(program, &has_dataset, &has_array, &has_function);
    ast_destroy(program);
    parser_release(&parser);
    if (has_dataset) return SCRIPT_PIPELINE_CANONICAL_DATASET;
    if (has_array) return SCRIPT_PIPELINE_CANONICAL_ARRAY;
    if (has_function) return SCRIPT_PIPELINE_CANONICAL_FUNCTION;
    /* A source that declares the canonical surface must fail closed. It may
     * not be reinterpreted by the historical textual router after a parse
     * error, because that can silently change semantics. */
    if (parser.has_error && (strstr(script, "analisis") != NULL ||
                             strstr(script, "array") != NULL ||
                             strstr(script, "arreglo") != NULL))
        return SCRIPT_PIPELINE_CANONICAL_INVALID;
    return SCRIPT_PIPELINE_LEGACY_DATASET;
}

static ScriptPipeline script_pipeline_for_source(const char *script) {
    if (!script) return SCRIPT_PIPELINE_LEGACY_DATASET;
    ScriptPipeline parsed_pipeline = script_pipeline_from_ast(script);
    if (parsed_pipeline == SCRIPT_PIPELINE_CANONICAL_DATASET ||
        parsed_pipeline == SCRIPT_PIPELINE_CANONICAL_ARRAY ||
        parsed_pipeline == SCRIPT_PIPELINE_CANONICAL_INVALID) return parsed_pipeline;
    if (strstr(script, "funcion") != NULL) {
        return SCRIPT_PIPELINE_LEGACY_NUMERIC_FUNCTIONS;
    }
    if ((strstr(script, "array") != NULL || strstr(script, "arreglo") != NULL) &&
        strstr(script, "dataset cargar") == NULL) {
        return SCRIPT_PIPELINE_LEGACY_ARRAY;
    }
    return SCRIPT_PIPELINE_LEGACY_DATASET;
}

static MilenaStatus run_canonical_functions(const char *script,
                                             MilenaError *error) {
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, script);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    if (!program || parser.has_error) {
        if (error) *error = parser.error;
        ast_destroy(program);
        parser_release(&parser);
        return MILENA_ERR_PARSE;
    }
    Interpreter interpreter;
    if (!interpreter_init(&interpreter, program)) {
        ast_destroy(program);
        parser_release(&parser);
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                         "No se pudo inicializar el runtime de funciones");
        return MILENA_ERR_MEMORY;
    }
    bool ok = interpreter_run(&interpreter);
    interpreter_destroy(&interpreter);
    ast_destroy(program);
    parser_release(&parser);
    if (!ok) {
        milena_error_set(error, MILENA_ERROR_RUNTIME, 0, 0, 0,
                         "La ejecución de funciones AST falló");
        return MILENA_ERROR_RUNTIME;
    }
    return MILENA_OK;
}

/* COMPATIBILIDAD: las funciones nuevas deben usar parser + AST + Interpreter. */
static MilenaStatus run_legacy_numeric_functions(const char *script, MilenaError *error) {
    const char *p = script; size_t total = strlen(script), used = 0;
    char *decls = (char *)malloc(total + 1), message[256] = {0};
    MilenaFunctionTable table; milena_function_table_init(&table);
    if (!decls) { milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0, "Sin memoria para funciones"); return MILENA_ERR_MEMORY; }
    while ((p = strstr(p, "funcion")) != NULL) {
        const char *open = strchr(p, '{'); const char *q; int depth = 0;
        if (!open) break;
        for (q = open; *q; q++) { if (*q == '{') depth++; else if (*q == '}' && --depth == 0) { q++; break; } }
        if (depth != 0) { free(decls); milena_function_table_release(&table); milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0, "Función sin cierre"); return MILENA_ERR_PARSE; }
        memcpy(decls + used, p, (size_t)(q - p)); used += (size_t)(q - p); decls[used++] = ' '; p = q;
    }
    decls[used] = '\0';
    if (!milena_parse_numeric_functions(decls, &table, message, sizeof message)) {
        free(decls); milena_function_table_release(&table); milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0, message); return MILENA_ERR_PARSE;
    }
    free(decls);
    /* A script invokes a function with: llamar nombre(1, 2); (also llama). */
    const char *call = strstr(script, "llamar");
    if (!call) call = strstr(script, "llama");
    if (call) {
        call += (strncmp(call, "llamar", 6) == 0 ? 6 : 5); while (isspace((unsigned char)*call)) call++;
        char name[128]; size_t ni = 0; while ((isalnum((unsigned char)*call) || *call == '_') && ni + 1 < sizeof name) name[ni++] = *call++;
        name[ni] = '\0'; while (isspace((unsigned char)*call)) call++;
        if (*call != '(' || !name[0]) { milena_function_table_release(&table); milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0, "Llamada de función inválida"); return MILENA_ERR_PARSE; }
        call++; double args[64]; size_t argc = 0; while (1) { char *end; while (isspace((unsigned char)*call)) call++; if (*call == ')') { call++; break; } if (argc == 64) { milena_function_table_release(&table); return MILENA_ERR_PARSE; } args[argc] = strtod(call, &end); if (end == call) { milena_function_table_release(&table); milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0, "Argumento numérico inválido"); return MILENA_ERR_PARSE; } argc++; call = end; while (isspace((unsigned char)*call)) call++; if (*call == ',') { call++; continue; } if (*call == ')') { call++; break; } milena_function_table_release(&table); milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0, "Se esperaba ',' o ')'"); return MILENA_ERR_PARSE; }
        double result = 0.0; if (!milena_function_call(&table, name, args, argc, &result, message, sizeof message)) { milena_function_table_release(&table); milena_error_set(error, MILENA_ERROR_RUNTIME, 0, 0, 0, message); return MILENA_ERROR_RUNTIME; }
        printf("%s = %.17g\n", name, result);
    }
    milena_function_table_release(&table); return MILENA_OK;
}

MilenaStatus milena_run_script(const char *filename, MilenaError *error) {
    if (!filename) return MILENA_ERR_ARGUMENT;
    char *script = read_file(filename, error);
    if (!script) return error && error->code ? error->code : MILENA_ERR_IO;
    /* El AST decide toda ejecución oficial. El router textual que queda abajo
     * solo atiende sintaxis histórica explícita y no recibe capacidades nuevas. */
    ScriptPipeline pipeline = script_pipeline_for_source(script);
    if (pipeline == SCRIPT_PIPELINE_CANONICAL_INVALID) {
        milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                         "La sintaxis canónica no es válida; no se permite compatibilidad implícita");
        free(script);
        return MILENA_ERR_PARSE;
    }
    if (pipeline == SCRIPT_PIPELINE_CANONICAL_ARRAY) {
        MilenaStatus canonical_status = milena_run_array_program(script, stdout, error);
        free(script);
        return canonical_status;
    }
    if (pipeline == SCRIPT_PIPELINE_CANONICAL_DATASET) {
        MilenaStatus canonical_status = milena_run_dataset_program(script, filename,
                                                                    stdout, error);
        free(script);
        return canonical_status;
    }
    if (pipeline == SCRIPT_PIPELINE_CANONICAL_FUNCTION) {
        MilenaStatus function_status = run_canonical_functions(script, error);
        free(script);
        return function_status;
    }
    if (pipeline == SCRIPT_PIPELINE_LEGACY_NUMERIC_FUNCTIONS) {
        MilenaStatus fn_status = run_legacy_numeric_functions(script, error);
        free(script);
        return fn_status;
    }
    if (pipeline == SCRIPT_PIPELINE_LEGACY_ARRAY) {
        MilenaStatus array_status = run_legacy_array_declarations(script, error);
        free(script);
        return array_status;
    }
    /* Desde aquí comienza únicamente la ruta histórica de compatibilidad. */
    MilenaSchema schema; schema_init(&schema);
    MilenaStatus status = parse_schema(script, &schema, error);
    if (status == MILENA_OK) status = validate_commands(script, error);
    if (status != MILENA_OK) { free(script); schema_destroy(&schema); return status; }

    char input[1024] = {0};
    char resolved_input[2048] = {0};
    char resource_base[1024] = {0};
    char output[1024] = "reporte_dataset.json";
    char resolved_output[2048] = {0};
    if (!find_quoted_after(script, "dataset cargar", input, sizeof(input))) {
        free(script); schema_destroy(&schema); milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0, "Falta dataset cargar datos(\"...\")"); return MILENA_ERR_PARSE;
    }
    status = resolve_input_path(filename, input, resolved_input, sizeof(resolved_input),
                                resource_base, sizeof(resource_base), error);
    const char *export_pos = strstr(script, ".exportar");
    if (status == MILENA_OK && export_pos && !find_quoted_after(export_pos, ".exportar", output, sizeof(output))) status = MILENA_ERR_PARSE;
    if (status == MILENA_OK) status = resolve_output_path(output, resource_base, resolved_output, sizeof(resolved_output));
    if (has_text(script, "#total(\"precio * cantidad\")") && schema_index(&schema, "total") < 0) status = schema_add(&schema, "total", MILENA_VAR_NUMERIC, MILENA_ROLE_FEATURE, error);
    if (status == MILENA_OK && has_text(script, "#periodo extraer(\"mes de fecha\")") && schema_index(&schema, "periodo") < 0) status = schema_add(&schema, "periodo", MILENA_VAR_CATEGORICAL, MILENA_ROLE_FEATURE, error);

    Dataset dataset; dataset_init(&dataset); DatasetLimits limits = dataset_default_limits();
    if (status == MILENA_OK) status = dataset_load_csv_with_limits(&dataset, resolved_input, ',', &limits, error);
    if (status == MILENA_OK && has_text(script, "#nulos(\"eliminar\")")) status = dataset_remove_null_rows(&dataset, error);
    if (status == MILENA_OK && has_text(script, "#duplicados(\"eliminar\")")) status = dataset_remove_duplicates(&dataset, error);
    if (status == MILENA_OK && has_text(script, "#total(\"precio * cantidad\")")) status = dataset_add_product(&dataset, "precio", "cantidad", "total", error);
    if (status == MILENA_OK && has_text(script, "#periodo extraer(\"mes de fecha\")")) status = dataset_add_month(&dataset, "fecha", "periodo", error);
    if (status == MILENA_OK && has_text(script, "#condicion(\"total > 0\")")) status = dataset_filter_positive_product(&dataset, "precio", "cantidad", error);
    if (status == MILENA_OK) status = analysis_dataset_report(&dataset, &schema, resolved_output, error);
    if (status == MILENA_OK) { printf("Script ejecutado correctamente: %s\n", filename); printf("Filas: %zu | Columnas: %zu | Filas inválidas: %zu\n", dataset.row_count, dataset.column_count, dataset.invalid_rows); }
    dataset_destroy(&dataset); schema_destroy(&schema); free(script); return status;
}

