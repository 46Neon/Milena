#include "analysis.h"

#include <inttypes.h>

static void write_json_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (*p == '"') fputs("\\\"", out);
        else if (*p == '\\') fputs("\\\\", out);
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\r') fputs("\\r", out);
        else if (*p == '\t') fputs("\\t", out);
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static bool equal_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int binary_value(const char *text) {
    if (equal_ci(text, "1") || equal_ci(text, "true") ||
        equal_ci(text, "verdadero") || equal_ci(text, "yes") ||
        equal_ci(text, "si")) return 1;
    if (equal_ci(text, "0") || equal_ci(text, "false") ||
        equal_ci(text, "falso") || equal_ci(text, "no")) return 0;
    return -1;
}

typedef struct {
    const char *value;
    size_t count;
} CategoryCount;

static MilenaStatus collect_categories(const Dataset *dataset, size_t column,
                                     CategoryCount **out, size_t *out_count,
                                     size_t *nulls) {
    CategoryCount *items = NULL;
    size_t count = 0, capacity = 0;
    *nulls = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        const char *value = dataset->rows[r][column];
        if (!value || value[0] == '\0') {
            (*nulls)++;
            continue;
        }
        size_t i;
        for (i = 0; i < count; i++) {
            if (strcmp(items[i].value, value) == 0) {
                items[i].count++;
                break;
            }
        }
        if (i == count) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 16;
                CategoryCount *tmp = (CategoryCount *)realloc(items, next * sizeof(*tmp));
                if (!tmp) {
                    free(items);
                    return MILENA_ERR_MEMORY;
                }
                items = tmp;
                capacity = next;
            }
            items[count].value = value;
            items[count].count = 1;
            count++;
        }
    }
    *out = items;
    *out_count = count;
    return MILENA_OK;
}

static void write_numeric_profile(FILE *out, const Dataset *dataset, size_t column) {
    size_t valid = 0, invalid = 0;
    double sum = 0.0, min = DBL_MAX, max = -DBL_MAX;
    for (size_t r = 0; r < dataset->row_count; r++) {
        double value;
        if (milena_parse_double(dataset->rows[r][column], &value) != MILENA_OK) {
            invalid++;
            continue;
        }
        valid++;
        sum += value;
        if (value < min) min = value;
        if (value > max) max = value;
    }
    fprintf(out, "\"validos\": %zu, \"invalidos\": %zu, ", valid, invalid);
    fprintf(out, "\"promedio\": %.10g, \"minimo\": %.10g, \"maximo\": %.10g",
            valid ? sum / (double)valid : 0.0,
            valid ? min : 0.0, valid ? max : 0.0);
}

MilenaStatus analysis_dataset_report(const Dataset *dataset,
                                  const MilenaSchema *schema,
                                  const char *output_json,
                                  MilenaError *error) {
    if (!dataset || !schema || !output_json) return MILENA_ERR_ARGUMENT;
    FILE *out = fopen(output_json, "wb");
    if (!out) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir reporte de dataset");
        return MILENA_ERR_IO;
    }
    fprintf(out, "{\n  \"analisis\": \"dataset\",\n");
    fprintf(out, "  \"filas\": %zu,\n  \"columnas\": %zu,\n  \"filas_invalidas\": %zu,\n",
            dataset->row_count, dataset->column_count, dataset->invalid_rows);
    fprintf(out, "  \"variables\": [\n");
    for (size_t i = 0; i < schema->count; i++) {
        const MilenaVariable *variable = &schema->variables[i];
        if (i) fputs(",\n", out);
        fprintf(out, "    {\"nombre\": ");
        write_json_string(out, variable->name);
        fprintf(out, ", \"tipo\": ");
        write_json_string(out, schema_type_name(variable->type));
        fprintf(out, ", \"rol\": ");
        write_json_string(out, schema_role_name(variable->role));
        int index = dataset_column_index(dataset, variable->name);
        if (index < 0) {
            fputs(", \"estado\": \"columna_inexistente\"}", out);
            continue;
        }
        fputs(", \"estado\": \"ok\", \"columna\": ", out);
        fprintf(out, "%d", index);
        if (variable->type == MILENA_VAR_NUMERIC) {
            fputs(", ", out);
            write_numeric_profile(out, dataset, (size_t)index);
        }
        fputs("}", out);
    }
    fprintf(out, "\n  ],\n  \"entradas_categoricas\": [\n");
    bool first_input = true;
    for (size_t i = 0; i < schema->count; i++) {
        const MilenaVariable *variable = &schema->variables[i];
        if (variable->role != MILENA_ROLE_CATEGORICAL_INPUT) continue;
        int index = dataset_column_index(dataset, variable->name);
        if (index < 0) continue;
        CategoryCount *items = NULL;
        size_t item_count = 0, nulls = 0;
        MilenaStatus status = collect_categories(dataset, (size_t)index,
                                               &items, &item_count, &nulls);
        if (status != MILENA_OK) {
            fclose(out);
            return status;
        }
        if (!first_input) fputs(",\n", out);
        first_input = false;
        fputs("    {\"variable\": ", out);
        write_json_string(out, variable->name);
        fprintf(out, ", \"nulos\": %zu, \"categorias\": [", nulls);
        for (size_t c = 0; c < item_count; c++) {
            if (c) fputs(", ", out);
            fputs("{\"valor\": ", out);
            write_json_string(out, items[c].value);
            fprintf(out, ", \"cantidad\": %zu}", items[c].count);
        }
        fputs("]}", out);
        free(items);
    }
    fprintf(out, "\n  ],\n  \"salidas_binarias\": [\n");
    bool first_target = true;
    for (size_t i = 0; i < schema->count; i++) {
        const MilenaVariable *variable = &schema->variables[i];
        if (variable->role != MILENA_ROLE_BINARY_OUTPUT) continue;
        int index = dataset_column_index(dataset, variable->name);
        if (index < 0) continue;
        size_t zeros = 0, ones = 0, invalid = 0;
        for (size_t r = 0; r < dataset->row_count; r++) {
            int value = binary_value(dataset->rows[r][index]);
            if (value == 0) zeros++;
            else if (value == 1) ones++;
            else invalid++;
        }
        if (!first_target) fputs(",\n", out);
        first_target = false;
        fputs("    {\"variable\": ", out);
        write_json_string(out, variable->name);
        fprintf(out, ", \"ceros\": %zu, \"unos\": %zu, \"invalidos\": %zu, \"tasa_unos\": %.10g}",
                zeros, ones, invalid, dataset->row_count ? (double)ones / (double)dataset->row_count : 0.0);
    }
    fputs("\n  ]\n}\n", out);
    bool io_error = ferror(out) != 0;
    if (fclose(out) != 0) io_error = true;
    if (io_error) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "Error escribiendo reporte de dataset");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}


static void write_table_json_value(FILE *out, const MilenaTable *table,
                                   size_t column, size_t row) {
    if (milena_table_is_null(table, column, row)) {
        fputs("null", out);
        return;
    }
    const MilenaTableColumn *data = milena_table_column(table, column);
    if (data->type == MILENA_COLUMN_STRING) {
        const char *value = NULL;
        if (milena_table_get_string(table, column, row, &value, NULL) == MILENA_OK)
            write_json_string(out, value);
        else fputs("null", out);
        return;
    }
    if (data->type == MILENA_COLUMN_CATEGORICAL) {
        const char *label = NULL;
        uint32_t code = 0;
        if (milena_table_get_category(table, column, row, &code, &label, NULL) == MILENA_OK)
            write_json_string(out, label);
        else fputs("null", out);
        return;
    }
    const void *raw = NULL;
    if (milena_table_get_array_value(table, column, row, &raw, NULL) != MILENA_OK) {
        fputs("null", out);
        return;
    }
    switch (data->values.dtype) {
        case MILENA_DTYPE_BOOL: fprintf(out, "%s", *(const bool *)raw ? "true" : "false"); break;
        case MILENA_DTYPE_INT8: fprintf(out, "%d", (int)*(const int8_t *)raw); break;
        case MILENA_DTYPE_INT16: fprintf(out, "%d", (int)*(const int16_t *)raw); break;
        case MILENA_DTYPE_INT32: fprintf(out, "%" PRId32, *(const int32_t *)raw); break;
        case MILENA_DTYPE_INT64: fprintf(out, "%" PRId64, *(const int64_t *)raw); break;
        case MILENA_DTYPE_UINT8: fprintf(out, "%u", (unsigned)*(const uint8_t *)raw); break;
        case MILENA_DTYPE_UINT16: fprintf(out, "%u", (unsigned)*(const uint16_t *)raw); break;
        case MILENA_DTYPE_UINT32: fprintf(out, "%" PRIu32, *(const uint32_t *)raw); break;
        case MILENA_DTYPE_UINT64: fprintf(out, "%" PRIu64, *(const uint64_t *)raw); break;
        case MILENA_DTYPE_FLOAT32: fprintf(out, "%.9g", (double)*(const float *)raw); break;
        case MILENA_DTYPE_FLOAT64: fprintf(out, "%.17g", *(const double *)raw); break;
        default: fputs("null", out); break;
    }
}

MilenaStatus analysis_table_report(const MilenaTable *table,
                                const MilenaSchema *schema,
                                const char *output_json,
                                MilenaError *error) {
    if (!table || !schema || !output_json) return MILENA_ERR_ARGUMENT;
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;
    FILE *out = fopen(output_json, "wb");
    if (!out) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo abrir reporte tipado");
        return MILENA_ERR_IO;
    }
    fprintf(out, "{\n  \"analisis\": \"dataset\",\n");
    fprintf(out, "  \"filas\": %zu,\n  \"columnas\": %zu,\n", table->row_count, table->column_count);
    fputs("  \"variables\": [", out);
    for (size_t column = 0; column < table->column_count; column++) {
        const MilenaTableColumn *data = &table->columns[column];
        int schema_position = schema_index(schema, data->name);
        const MilenaVariable *variable = schema_position >= 0 ? &schema->variables[schema_position] : NULL;
        if (column) fputs(",", out);
        fputs("{\"nombre\": ", out); write_json_string(out, data->name);
        fputs(", \"tipo\": ", out);
        write_json_string(out, variable ? schema_type_name(variable->type) :
                          (data->type == MILENA_COLUMN_STRING ? "texto" :
                           data->type == MILENA_COLUMN_CATEGORICAL ? "categorica" : "numerica"));
        fputs(", \"rol\": ", out);
        write_json_string(out, variable ? schema_role_name(variable->role) : "variable");
        fputs(", \"estado\": \"ok\"}", out);
    }
    fputs("],\n  \"entradas_categoricas\": [", out);
    bool first = true;
    for (size_t i = 0; i < schema->count; i++) if (schema->variables[i].role == MILENA_ROLE_CATEGORICAL_INPUT) {
        if (!first) fputs(",", out); first = false;
        fputs("{\"variable\": ", out); write_json_string(out, schema->variables[i].name); fputs("}", out);
    }
    fputs("],\n  \"salidas_binarias\": [", out); first = true;
    for (size_t i = 0; i < schema->count; i++) if (schema->variables[i].role == MILENA_ROLE_BINARY_OUTPUT) {
        if (!first) fputs(",", out); first = false;
        fputs("{\"variable\": ", out); write_json_string(out, schema->variables[i].name); fputs("}", out);
    }
    fputs("],\n  \"datos\": [", out);
    for (size_t row = 0; row < table->row_count; row++) {
        if (row) fputs(",", out);
        fputs("{", out);
        for (size_t column = 0; column < table->column_count; column++) {
            if (column) fputs(",", out);
            write_json_string(out, table->columns[column].name);
            fputs(":", out); write_table_json_value(out, table, column, row);
        }
        fputs("}", out);
    }
    fputs("]\n}\n", out);
    bool io_error = ferror(out) != 0;
    if (fclose(out) != 0) io_error = true;
    if (io_error) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "Error escribiendo reporte tipado");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

MilenaStatus analysis_sales(const Dataset *dataset,
                          const char *date_column,
                          const char *price_column,
                          const char *quantity_column,
                          const char *output_json,
                          SalesSummary *summary,
                          MilenaError *error) {
    if (!dataset || !date_column || !price_column || !quantity_column ||
        !output_json || !summary) return MILENA_ERR_ARGUMENT;
    memset(summary, 0, sizeof(*summary));
    int date_i = dataset_column_index(dataset, date_column);
    int price_i = dataset_column_index(dataset, price_column);
    int quantity_i = dataset_column_index(dataset, quantity_column);
    if (date_i < 0 || price_i < 0 || quantity_i < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "Columna requerida inexistente");
        return MILENA_ERR_DATA;
    }

    summary->minimum = DBL_MAX;
    for (size_t r = 0; r < dataset->row_count; r++) {
        summary->rows_seen++;
        double price, quantity;
        if (milena_parse_double(dataset->rows[r][price_i], &price) != MILENA_OK ||
            milena_parse_double(dataset->rows[r][quantity_i], &quantity) != MILENA_OK) {
            summary->rows_rejected++;
            continue;
        }
        double total = price * quantity;
        if (!isfinite(total)) {
            summary->rows_rejected++;
            continue;
        }
        if (total <= 0.0) {
            summary->rows_rejected++;
            continue;
        }
        summary->rows_used++;
        summary->total += total;
        if (total < summary->minimum) summary->minimum = total;
        if (total > summary->maximum) summary->maximum = total;
    }
    summary->average = summary->rows_used ? summary->total / (double)summary->rows_used : 0.0;
    if (summary->rows_used == 0) summary->minimum = 0.0;

    FILE *out = fopen(output_json, "wb");
    if (!out) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir salida JSON");
        return MILENA_ERR_IO;
    }
    fprintf(out, "{\n  \"analisis\": ");
    write_json_string(out, "ventas");
    fprintf(out, ",\n  \"filas_vistas\": %zu,\n  \"filas_usadas\": %zu,\n"
                 "  \"filas_rechazadas\": %zu,\n  \"total\": %.10g,\n"
                 "  \"promedio\": %.10g,\n  \"maxima\": %.10g,\n"
                 "  \"minima\": %.10g\n}\n",
            summary->rows_seen, summary->rows_used, summary->rows_rejected,
            summary->total, summary->average, summary->maximum, summary->minimum);
    bool io_error = ferror(out) != 0;
    if (fclose(out) != 0) io_error = true;
    if (io_error) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "Error escribiendo salida JSON");
        return MILENA_ERR_IO;
    }
    (void)date_i;
    return MILENA_OK;
}

bool analysis_ventas(const Dataset *dataset, const char *date_column,
                     const char *price_column, const char *quantity_column,
                     const char *output_json) {
    SalesSummary summary;
    MilenaError error;
    MilenaStatus status = analysis_sales(dataset, date_column, price_column,
                                       quantity_column, output_json,
                                       &summary, &error);
    if (status != MILENA_OK) {
        milena_error_print(&error, stderr);
        return false;
    }
    printf("Total: %.2f | Promedio: %.2f | Máxima: %.2f | Mínima: %.2f | Usadas: %zu | Rechazadas: %zu\n",
           summary.total, summary.average, summary.maximum, summary.minimum,
           summary.rows_used, summary.rows_rejected);
    return true;
}
