#include "dataset.h"

DatasetLimits dataset_default_limits(void) {
    DatasetLimits limits = {5000, 70, 1024 * 1024};
    return limits;
}

static void free_fields(char **fields, size_t count) {
    if (!fields) return;
    for (size_t i = 0; i < count; i++) free(fields[i]);
    free(fields);
}

void dataset_init(Dataset *dataset) {
    if (!dataset) return;
    memset(dataset, 0, sizeof(*dataset));
}

void dataset_destroy(Dataset *dataset) {
    if (!dataset) return;
    free(dataset->filename);
    free_fields(dataset->headers, dataset->column_count);
    if (dataset->rows) {
        for (size_t r = 0; r < dataset->row_count; r++) {
            free_fields(dataset->rows[r], dataset->column_count);
        }
    }
    free(dataset->rows);
    dataset_init(dataset);
}

static MilenaStatus grow_bytes(char **buffer, size_t *capacity, size_t need) {
    if (need <= *capacity) return MILENA_OK;
    size_t next = *capacity ? *capacity : 256;
    while (next < need) {
        if (next > SIZE_MAX / 2) return MILENA_ERR_OVERFLOW;
        next *= 2;
    }
    char *tmp = (char *)realloc(*buffer, next);
    if (!tmp) return MILENA_ERR_MEMORY;
    *buffer = tmp;
    *capacity = next;
    return MILENA_OK;
}

/* Reads one CSV record, including newlines inside quoted fields. */
static MilenaStatus read_record(FILE *file, char **record, size_t *line,
                              MilenaError *error) {
    if (!file || !record || !line) return MILENA_ERR_ARGUMENT;
    *record = NULL;
    size_t capacity = 0, length = 0, start_line = *line;
    bool in_quotes = false;
    int ch;

    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\r') {
            int next = fgetc(file);
            if (next != '\n' && next != EOF) ungetc(next, file);
            if (in_quotes) {
                MilenaStatus st = grow_bytes(record, &capacity, length + 2);
                if (st != MILENA_OK) goto fail;
                (*record)[length++] = '\n';
                (*line)++;
            } else {
                break;
            }
        } else if (ch == '\n') {
            if (in_quotes) {
                MilenaStatus st = grow_bytes(record, &capacity, length + 2);
                if (st != MILENA_OK) goto fail;
                (*record)[length++] = '\n';
                (*line)++;
            } else {
                break;
            }
        } else {
            MilenaStatus st = grow_bytes(record, &capacity, length + 2);
            if (st != MILENA_OK) goto fail;
            (*record)[length++] = (char)ch;
            if (ch == '"') in_quotes = !in_quotes;
        }
    }

    if (ch == EOF && length == 0) {
        free(*record);
        *record = NULL;
        return MILENA_ERR_IO;
    }
    if (in_quotes) {
        milena_error_set(error, MILENA_ERR_PARSE, start_line, 1, 0,
                       "CSV con comillas sin cerrar");
        free(*record);
        *record = NULL;
        return MILENA_ERR_PARSE;
    }
    MilenaStatus st = grow_bytes(record, &capacity, length + 1);
    if (st != MILENA_OK) goto fail;
    (*record)[length] = '\0';
    return MILENA_OK;

fail:
    free(*record);
    *record = NULL;
    milena_error_set(error, MILENA_ERR_MEMORY, start_line, 1, 0,
                   "Memoria insuficiente leyendo CSV");
    return MILENA_ERR_MEMORY;
}

static MilenaStatus append_field(char ***fields, size_t *count, size_t *capacity,
                               char *field) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 8;
        if (next < *capacity) return MILENA_ERR_OVERFLOW;
        char **tmp = (char **)realloc(*fields, next * sizeof(*tmp));
        if (!tmp) return MILENA_ERR_MEMORY;
        *fields = tmp;
        *capacity = next;
    }
    (*fields)[(*count)++] = field;
    return MILENA_OK;
}

static MilenaStatus append_char(char **text, size_t *length, size_t *capacity,
                              char ch) {
    if (*length + 1 >= *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        if (next < *capacity) return MILENA_ERR_OVERFLOW;
        char *tmp = (char *)realloc(*text, next);
        if (!tmp) return MILENA_ERR_MEMORY;
        *text = tmp;
        *capacity = next;
    }
    (*text)[(*length)++] = ch;
    return MILENA_OK;
}

static MilenaStatus parse_record(const char *record, char delimiter,
                               char ***out_fields, size_t *out_count,
                               MilenaError *error) {
    if (!record || !out_fields || !out_count) return MILENA_ERR_ARGUMENT;
    char **fields = NULL;
    size_t count = 0, field_capacity = 0;
    const char *p = record;

    char *field = NULL;
    while (true) {
        size_t length = 0, capacity = 0;
        bool quoted = false, closed_quote = false;
        field = NULL;

        if (*p == '"') {
            quoted = true;
            p++;
        }

        while (*p) {
            char ch = *p;
            if (quoted) {
                if (ch == '"') {
                    if (p[1] == '"') {
                        MilenaStatus st = append_char(&field, &length, &capacity, '"');
                        if (st != MILENA_OK) goto fail;
                        p += 2;
                        continue;
                    }
                    p++;
                    closed_quote = true;
                    quoted = false;
                    continue;
                }
                MilenaStatus st = append_char(&field, &length, &capacity, ch);
                if (st != MILENA_OK) goto fail;
                p++;
            } else {
                if (ch == delimiter) break;
                if (ch == '"' && !closed_quote) {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                   "Comilla inesperada dentro de campo CSV");
                    goto fail;
                }
                if (closed_quote && ch != ' ' && ch != '\t') {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                   "Caracteres después de comilla CSV");
                    goto fail;
                }
                if (!closed_quote) {
                    MilenaStatus st = append_char(&field, &length, &capacity, ch);
                    if (st != MILENA_OK) goto fail;
                }
                p++;
            }
        }

        MilenaStatus st = append_char(&field, &length, &capacity, '\0');
        if (st != MILENA_OK) goto fail;
        st = append_field(&fields, &count, &field_capacity, field);
        if (st != MILENA_OK) goto fail;
        field = NULL;

        if (*p == delimiter) {
            p++;
            if (*p == '\0') {
                char *empty = milena_strdup("");
                if (!empty || append_field(&fields, &count, &field_capacity, empty) != MILENA_OK) {
                    free(empty);
                    goto fail;
                }
                break;
            }
            continue;
        }
        break;
    }

    *out_fields = fields;
    *out_count = count;
    return MILENA_OK;

fail:
    free(field);
    free_fields(fields, count);
    return error && error->code != MILENA_OK ? error->code : MILENA_ERR_MEMORY;
}

static MilenaStatus add_row(Dataset *dataset, char **row, MilenaError *error) {
    if (dataset->row_count == dataset->row_capacity) {
        size_t next = dataset->row_capacity ? dataset->row_capacity * 2 : 64;
        if (next < dataset->row_capacity) return MILENA_ERR_OVERFLOW;
        char ***tmp = (char ***)realloc(dataset->rows, next * sizeof(*tmp));
        if (!tmp) {
            milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, dataset->row_count,
                           "Memoria insuficiente agregando fila");
            return MILENA_ERR_MEMORY;
        }
        dataset->rows = tmp;
        dataset->row_capacity = next;
    }
    dataset->rows[dataset->row_count++] = row;
    return MILENA_OK;
}

static bool headers_valid(char **headers, size_t count, MilenaError *error) {
    for (size_t i = 0; i < count; i++) {
        if (!headers[i] || headers[i][0] == '\0') {
            milena_error_set(error, MILENA_ERR_DATA, 1, i + 1, 0,
                           "Nombre de columna vacío");
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(headers[i], headers[j]) == 0) {
                milena_error_set(error, MILENA_ERR_DATA, 1, i + 1, 0,
                               "Columnas duplicadas");
                return false;
            }
        }
    }
    return true;
}

MilenaStatus dataset_load_csv_with_limits(Dataset *dataset, const char *filename,
                                        char delimiter, const DatasetLimits *limits,
                                        MilenaError *error) {
    if (!dataset || !filename || delimiter == '\0') return MILENA_ERR_ARGUMENT;
    DatasetLimits defaults = dataset_default_limits();
    if (!limits) limits = &defaults;
    if (limits->max_rows == 0 || limits->max_columns == 0 || limits->max_field_bytes == 0) {
        return MILENA_ERR_ARGUMENT;
    }
    MilenaError local;
    if (!error) error = &local;
    milena_error_clear(error);

    FILE *file = fopen(filename, "rb");
    if (!file) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir el CSV");
        return MILENA_ERR_IO;
    }

    Dataset tmp;
    dataset_init(&tmp);
    tmp.filename = milena_strdup(filename);
    if (!tmp.filename) {
        fclose(file);
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0, "Sin memoria para nombre de archivo");
        return MILENA_ERR_MEMORY;
    }

    size_t line = 1;
    char *record = NULL;
    MilenaStatus status = read_record(file, &record, &line, error);
    if (status != MILENA_OK) {
        if (status == MILENA_ERR_IO && feof(file)) {
            milena_error_set(error, MILENA_ERR_DATA, 1, 1, 0, "CSV vacío");
            status = MILENA_ERR_DATA;
        }
        goto fail;
    }
    status = parse_record(record, delimiter, &tmp.headers, &tmp.column_count, error);
    free(record);
    record = NULL;
    if (status != MILENA_OK || tmp.column_count == 0 || !headers_valid(tmp.headers, tmp.column_count, error)) {
        status = status == MILENA_OK ? MILENA_ERR_DATA : status;
        goto fail;
    }
    if (tmp.column_count > limits->max_columns) {
        milena_error_set(error, MILENA_ERR_DATA, 1, 0, 0,
                       "El CSV supera el máximo de columnas permitido");
        status = MILENA_ERR_DATA;
        goto fail;
    }
    size_t max_record_bytes = 0;
    if (!milena_size_mul(limits->max_field_bytes, tmp.column_count, &max_record_bytes) ||
        !milena_size_add(max_record_bytes, tmp.column_count, &max_record_bytes)) {
        status = MILENA_ERR_OVERFLOW;
        goto fail;
    }

    while (true) {
        status = read_record(file, &record, &line, error);
        if (status == MILENA_ERR_IO && feof(file)) {
            status = MILENA_OK;
            break;
        }
        if (status != MILENA_OK) goto fail;
        if (strlen(record) > max_record_bytes) {
            milena_error_set(error, MILENA_ERR_DATA, line, 0, 0,
                           "El registro CSV supera el límite de tamaño");
            status = MILENA_ERR_DATA;
            goto fail;
        }
        if (tmp.row_count >= limits->max_rows) {
            milena_error_set(error, MILENA_ERR_DATA, line, 0, tmp.row_count + 1,
                           "El CSV supera el máximo de filas permitido");
            status = MILENA_ERR_DATA;
            goto fail;
        }
        if (record[0] == '\0') {
            free(record);
            record = NULL;
            continue;
        }

        char **row = NULL;
        size_t fields = 0;
        status = parse_record(record, delimiter, &row, &fields, error);
        free(record);
        record = NULL;
        if (status != MILENA_OK || fields != tmp.column_count) {
            tmp.invalid_rows++;
            free_fields(row, fields);
            milena_error_clear(error);
            continue;
        }
        status = add_row(&tmp, row, error);
        if (status != MILENA_OK) {
            free_fields(row, fields);
            goto fail;
        }
    }

    fclose(file);
    dataset_destroy(dataset);
    *dataset = tmp;
    return MILENA_OK;

fail:
    free(record);
    fclose(file);
    dataset_destroy(&tmp);
    return status;
}

MilenaStatus dataset_load_csv(Dataset *dataset, const char *filename,
                            char delimiter, MilenaError *error) {
    DatasetLimits limits = dataset_default_limits();
    return dataset_load_csv_with_limits(dataset, filename, delimiter,
                                        &limits, error);
}

int dataset_column_index(const Dataset *dataset, const char *name) {
    if (!dataset || !name) return -1;
    for (size_t i = 0; i < dataset->column_count; i++) {
        if (strcmp(dataset->headers[i], name) == 0) return (int)i;
    }
    return -1;
}

static MilenaStatus append_column(Dataset *dataset, const char *name,
                                char **values, MilenaError *error) {
    if (!dataset || !name || !values) return MILENA_ERR_ARGUMENT;
    char *copy = milena_strdup(name);
    if (!copy) return MILENA_ERR_MEMORY;

    size_t new_column_count = dataset->column_count + 1;
    char **new_headers = (char **)calloc(new_column_count, sizeof(*new_headers));
    char ***new_rows = dataset->row_count
        ? (char ***)calloc(dataset->row_count, sizeof(*new_rows)) : NULL;
    if (!new_headers || (dataset->row_count && !new_rows)) {
        free(new_headers);
        free(new_rows);
        free(copy);
        return MILENA_ERR_MEMORY;
    }
    for (size_t c = 0; c < dataset->column_count; c++) {
        new_headers[c] = dataset->headers[c];
    }
    new_headers[dataset->column_count] = copy;

    for (size_t r = 0; r < dataset->row_count; r++) {
        new_rows[r] = (char **)calloc(new_column_count, sizeof(*new_rows[r]));
        if (!new_rows[r]) {
            for (size_t j = 0; j < r; j++) free(new_rows[j]);
            free(new_rows);
            free(new_headers);
            free(copy);
            milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, r + 1,
                           "Sin memoria agregando columna");
            return MILENA_ERR_MEMORY;
        }
        memcpy(new_rows[r], dataset->rows[r],
               dataset->column_count * sizeof(*new_rows[r]));
        new_rows[r][dataset->column_count] = values[r];
    }

    for (size_t r = 0; r < dataset->row_count; r++) free(dataset->rows[r]);
    free(dataset->rows);
    free(dataset->headers);
    free(values);
    dataset->headers = new_headers;
    dataset->rows = new_rows;
    dataset->column_count = new_column_count;
    return MILENA_OK;
}

MilenaStatus dataset_add_product(Dataset *dataset, const char *left,
                               const char *right, const char *output,
                               MilenaError *error) {
    if (!dataset || !left || !right || !output) return MILENA_ERR_ARGUMENT;
    if (dataset_column_index(dataset, output) >= 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "La columna de salida ya existe");
        return MILENA_ERR_DATA;
    }
    int li = dataset_column_index(dataset, left);
    int ri = dataset_column_index(dataset, right);
    if (li < 0 || ri < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "Columna de producto inexistente");
        return MILENA_ERR_DATA;
    }
    char **values = (char **)calloc(dataset->row_count, sizeof(*values));
    if (dataset->row_count && !values) return MILENA_ERR_MEMORY;
    for (size_t r = 0; r < dataset->row_count; r++) {
        double a, b;
        if (milena_parse_double(dataset->rows[r][li], &a) != MILENA_OK ||
            milena_parse_double(dataset->rows[r][ri], &b) != MILENA_OK) {
            free_fields(values, r);
            milena_error_set(error, MILENA_ERR_TYPE, 0, 0, r + 1,
                           "Valor no numérico en producto");
            return MILENA_ERR_TYPE;
        }
        char buffer[96];
        int written = snprintf(buffer, sizeof(buffer), "%.10g", a * b);
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            free_fields(values, r);
            return MILENA_ERR_OVERFLOW;
        }
        values[r] = milena_strdup(buffer);
        if (!values[r]) {
            free_fields(values, r);
            return MILENA_ERR_MEMORY;
        }
    }
    return append_column(dataset, output, values, error);
}

MilenaStatus dataset_add_month(Dataset *dataset, const char *date_column,
                             const char *output, MilenaError *error) {
    if (!dataset || !date_column || !output) return MILENA_ERR_ARGUMENT;
    if (dataset_column_index(dataset, output) >= 0) return MILENA_ERR_DATA;
    int di = dataset_column_index(dataset, date_column);
    if (di < 0) return MILENA_ERR_DATA;
    char **values = (char **)calloc(dataset->row_count, sizeof(*values));
    if (dataset->row_count && !values) return MILENA_ERR_MEMORY;
    for (size_t r = 0; r < dataset->row_count; r++) {
        int year, month, day;
        if (sscanf(dataset->rows[r][di], "%d-%d-%d", &year, &month, &day) != 3 ||
            year < 1 || month < 1 || month > 12 || day < 1 || day > 31) {
            free_fields(values, r);
            milena_error_set(error, MILENA_ERR_TYPE, 0, 0, r + 1,
                           "Fecha inválida");
            return MILENA_ERR_TYPE;
        }
        char buffer[32];
        (void)snprintf(buffer, sizeof(buffer), "%04d-%02d", year, month);
        values[r] = milena_strdup(buffer);
        if (!values[r]) {
            free_fields(values, r);
            return MILENA_ERR_MEMORY;
        }
    }
    return append_column(dataset, output, values, error);
}

MilenaStatus dataset_filter_positive_product(Dataset *dataset,
                                           const char *left,
                                           const char *right,
                                           MilenaError *error) {
    if (!dataset || !left || !right) return MILENA_ERR_ARGUMENT;
    int li = dataset_column_index(dataset, left);
    int ri = dataset_column_index(dataset, right);
    if (li < 0 || ri < 0) return MILENA_ERR_DATA;
    size_t write = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        double a, b;
        MilenaStatus sa = milena_parse_double(dataset->rows[r][li], &a);
        MilenaStatus sb = milena_parse_double(dataset->rows[r][ri], &b);
        if (sa != MILENA_OK || sb != MILENA_OK) {
            milena_error_set(error, MILENA_ERR_TYPE, 0, 0, r + 1,
                           "Valor no numérico en filtro");
            return MILENA_ERR_TYPE;
        }
        if (a * b > 0.0) {
            dataset->rows[write++] = dataset->rows[r];
        } else {
            free_fields(dataset->rows[r], dataset->column_count);
        }
    }
    dataset->row_count = write;
    return MILENA_OK;
}

MilenaStatus dataset_remove_null_rows(Dataset *dataset, MilenaError *error) {
    if (!dataset) return MILENA_ERR_ARGUMENT;
    size_t write = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        bool has_null = false;
        for (size_t c = 0; c < dataset->column_count; c++) {
            if (!dataset->rows[r][c] || dataset->rows[r][c][0] == '\0') {
                has_null = true;
                break;
            }
        }
        if (has_null) free_fields(dataset->rows[r], dataset->column_count);
        else dataset->rows[write++] = dataset->rows[r];
    }
    dataset->row_count = write;
    (void)error;
    return MILENA_OK;
}

static bool rows_equal(const Dataset *dataset, size_t a, size_t b) {
    for (size_t c = 0; c < dataset->column_count; c++) {
        if (strcmp(dataset->rows[a][c], dataset->rows[b][c]) != 0) return false;
    }
    return true;
}

MilenaStatus dataset_remove_duplicates(Dataset *dataset, MilenaError *error) {
    if (!dataset) return MILENA_ERR_ARGUMENT;
    size_t write = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        bool duplicate = false;
        for (size_t p = 0; p < write; p++) {
            if (rows_equal(dataset, r, p)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) free_fields(dataset->rows[r], dataset->column_count);
        else dataset->rows[write++] = dataset->rows[r];
    }
    dataset->row_count = write;
    (void)error;
    return MILENA_OK;
}

MilenaStatus dataset_save_json(const Dataset *dataset, const char *filename,
                             MilenaError *error) {
    if (!dataset || !filename) return MILENA_ERR_ARGUMENT;
    FILE *out = fopen(filename, "wb");
    if (!out) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo crear JSON");
        return MILENA_ERR_IO;
    }
    fprintf(out, "{\n  \"dataset\": ");
    milena_json_write_string(out, dataset->filename);
    fprintf(out, ",\n  \"columnas\": [");
    for (size_t c = 0; c < dataset->column_count; c++) {
        if (c) fputs(", ", out);
        milena_json_write_string(out, dataset->headers[c]);
    }
    fprintf(out, "],\n  \"filas_invalidas\": %zu,\n  \"filas\": [\n", dataset->invalid_rows);
    for (size_t r = 0; r < dataset->row_count; r++) {
        fprintf(out, "    [");
        for (size_t c = 0; c < dataset->column_count; c++) {
            if (c) fputs(", ", out);
            milena_json_write_string(out, dataset->rows[r][c]);
        }
        fprintf(out, "]%s\n", r + 1 < dataset->row_count ? "," : "");
    }
    fputs("  ]\n}\n", out);
    bool io_error = ferror(out) != 0;
    if (fclose(out) != 0) io_error = true;
    if (io_error) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "Error escribiendo JSON");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

void dataset_print(const Dataset *dataset, size_t max_rows, FILE *stream) {
    if (!dataset) return;
    if (!stream) stream = stdout;
    fprintf(stream, "Dataset: %s\nColumnas: %zu | Filas: %zu | Inválidas: %zu\n",
            dataset->filename ? dataset->filename : "", dataset->column_count,
            dataset->row_count, dataset->invalid_rows);
    for (size_t c = 0; c < dataset->column_count; c++) {
        fprintf(stream, "%s%s", dataset->headers[c], c + 1 < dataset->column_count ? " | " : "\n");
    }
    size_t limit = dataset->row_count < max_rows ? dataset->row_count : max_rows;
    for (size_t r = 0; r < limit; r++) {
        for (size_t c = 0; c < dataset->column_count; c++) {
            fprintf(stream, "%s%s", dataset->rows[r][c], c + 1 < dataset->column_count ? " | " : "\n");
        }
    }
}

bool dataset_cargar_csv(Dataset *dataset, const char *filename) {
    MilenaError error;
    return dataset_load_csv(dataset, filename, ',', &error) == MILENA_OK;
}

bool dataset_guardar_json(const Dataset *dataset, const char *filename) {
    MilenaError error;
    return dataset_save_json(dataset, filename, &error) == MILENA_OK;
}

void dataset_destruir(Dataset *dataset) { dataset_destroy(dataset); }
int dataset_indice_columna(const Dataset *dataset, const char *name) {
    return dataset_column_index(dataset, name);
}
