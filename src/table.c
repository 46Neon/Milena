#include "table.h"
#include "dataset.h"
#include "schema.h"

#include <math.h>

#define MILENA_TABLE_MAGIC UINT64_C(0x4d494c5441424c45)

static void table_error(MilenaError *error, MilenaStatus code,
                        const char *message) {
    if (error != NULL) milena_error_set(error, code, 0, 0, 0, message);
}

static bool table_has_magic(const MilenaTable *table) {
    uint64_t value = 0;
    if (table == NULL) return false;
    memcpy(&value, &table->_table_magic, sizeof(value));
    return value == MILENA_TABLE_MAGIC;
}

static void metadata_destroy(MilenaTableMetadata *items, size_t count) {
    if (items == NULL) return;
    for (size_t i = 0; i < count; ++i) {
        free(items[i].key);
        free(items[i].value);
    }
    free(items);
}

static void column_destroy(MilenaTableColumn *column) {
    if (column == NULL) return;
    size_t rows = column->values.size;
    free(column->name);
    free(column->validity);
    if (column->strings != NULL) {
        for (size_t i = 0; i < rows; ++i) free(column->strings[i]);
        free(column->strings);
    }
    milena_array_release(&column->values);
    if (column->dictionary != NULL) {
        for (size_t i = 0; i < column->dictionary_size; ++i)
            free(column->dictionary[i]);
        free(column->dictionary);
    }
    metadata_destroy(column->metadata, column->metadata_count);
    memset(column, 0, sizeof(*column));
}

void milena_table_init(MilenaTable *table) {
    if (table == NULL) return;
    if (table_has_magic(table)) milena_table_destroy(table);
    memset(table, 0, sizeof(*table));
    table->_table_magic = MILENA_TABLE_MAGIC;
}

void milena_table_destroy(MilenaTable *table) {
    if (table == NULL) return;
    if (table_has_magic(table)) {
        for (size_t i = 0; i < table->column_count; ++i)
            column_destroy(&table->columns[i]);
        free(table->columns);
        metadata_destroy(table->metadata, table->metadata_count);
    }
    memset(table, 0, sizeof(*table));
}

void milena_table_swap(MilenaTable *left, MilenaTable *right) {
    if (left == NULL || right == NULL || left == right) return;
    MilenaTable temporary = *left;
    *left = *right;
    *right = temporary;
}

static void table_ensure_init(MilenaTable *table) {
    if (!table_has_magic(table)) {
        memset(table, 0, sizeof(*table));
        table->_table_magic = MILENA_TABLE_MAGIC;
    }
}

static MilenaStatus table_commit(MilenaTable *out, MilenaTable *temporary,
                                 MilenaError *error) {
    if (out == NULL || temporary == NULL || !table_has_magic(temporary)) {
        table_error(error, MILENA_ERR_INTERNAL, "Commit de tabla inválido");
        return MILENA_ERR_INTERNAL;
    }
    if (table_has_magic(out)) {
        MilenaStatus status = milena_table_validate(out, error);
        if (status != MILENA_OK) return status;
        milena_table_swap(out, temporary);
        milena_table_destroy(temporary);
    } else {
        *out = *temporary;
        memset(temporary, 0, sizeof(*temporary));
    }
    return MILENA_OK;
}

static bool utf8_valid(const char *text) {
    if (text == NULL) return false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p != 0u) {
        uint32_t cp = 0;
        size_t continuation = 0;
        if (*p < 0x80u) { ++p; continue; }
        if (*p >= 0xc2u && *p <= 0xdfu) {
            cp = (uint32_t)(*p & 0x1fu); continuation = 1;
        } else if (*p >= 0xe0u && *p <= 0xefu) {
            cp = (uint32_t)(*p & 0x0fu); continuation = 2;
        } else if (*p >= 0xf0u && *p <= 0xf4u) {
            cp = (uint32_t)(*p & 0x07u); continuation = 3;
        } else return false;
        ++p;
        for (size_t i = 0; i < continuation; ++i) {
            if ((p[i] & 0xc0u) != 0x80u) return false;
            cp = (cp << 6u) | (uint32_t)(p[i] & 0x3fu);
        }
        if ((continuation == 2 && cp < UINT32_C(0x800)) ||
            (continuation == 3 && cp < UINT32_C(0x10000)) ||
            cp > UINT32_C(0x10ffff) ||
            (cp >= UINT32_C(0xd800) && cp <= UINT32_C(0xdfff))) return false;
        p += continuation;
    }
    return true;
}

static bool dtype_is_code(MilenaDType dtype) {
    return dtype == MILENA_DTYPE_UINT8 || dtype == MILENA_DTYPE_UINT16 ||
           dtype == MILENA_DTYPE_UINT32 || dtype == MILENA_DTYPE_UINT64;
}

static bool dtype_is_real(MilenaDType dtype) {
    return dtype <= MILENA_DTYPE_FLOAT64;
}

static const unsigned char *array_row_const(const MilenaArray *array,
                                             size_t row) {
    const unsigned char *base =
        (const unsigned char *)milena_array_const_data(array);
    if (base == NULL || array->strides == NULL) return NULL;
    return base + (ptrdiff_t)row * array->strides[0];
}

static unsigned char *array_row_mut(MilenaArray *array, size_t row,
                                    MilenaError *error) {
    void *raw = NULL;
    if (milena_array_mut_data(array, &raw, error) != MILENA_OK) return NULL;
    return (unsigned char *)raw + (ptrdiff_t)row * array->strides[0];
}

static MilenaStatus categorical_code(const MilenaTableColumn *column,
                                     size_t row, uint32_t *code,
                                     MilenaError *error) {
    const unsigned char *p = array_row_const(&column->values, row);
    uint64_t wide = 0;
    if (p == NULL) {
        table_error(error, MILENA_ERR_DATA, "Storage categórico inválido");
        return MILENA_ERR_DATA;
    }
    switch (column->values.dtype) {
        case MILENA_DTYPE_UINT8: wide = *(const uint8_t *)p; break;
        case MILENA_DTYPE_UINT16: wide = *(const uint16_t *)p; break;
        case MILENA_DTYPE_UINT32: wide = *(const uint32_t *)p; break;
        case MILENA_DTYPE_UINT64: wide = *(const uint64_t *)p; break;
        default:
            table_error(error, MILENA_ERR_TYPE, "Los códigos categóricos deben ser unsigned");
            return MILENA_ERR_TYPE;
    }
    if (wide > UINT32_MAX || wide >= column->dictionary_size) {
        table_error(error, MILENA_ERR_DATA, "Código categórico fuera del diccionario");
        return MILENA_ERR_DATA;
    }
    *code = (uint32_t)wide;
    return MILENA_OK;
}

MilenaStatus milena_table_validate(const MilenaTable *table,
                                   MilenaError *error) {
    if (table == NULL || !table_has_magic(table)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Tabla no inicializada");
        return MILENA_ERR_ARGUMENT;
    }
    if (table->column_count > table->capacity ||
        (table->capacity != 0 && table->columns == NULL) ||
        (table->column_count == 0 && table->columns != NULL && table->capacity == 0)) {
        table_error(error, MILENA_ERR_DATA, "Descriptor de tabla incoherente");
        return MILENA_ERR_DATA;
    }
    for (size_t i = 0; i < table->column_count; ++i) {
        const MilenaTableColumn *column = &table->columns[i];
        if (column->name == NULL || column->name[0] == '\0' || !utf8_valid(column->name)) {
            table_error(error, MILENA_ERR_DATA, "Nombre de columna inválido");
            return MILENA_ERR_DATA;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(column->name, table->columns[j].name) == 0) {
                table_error(error, MILENA_ERR_DATA, "Nombres de columna duplicados");
                return MILENA_ERR_DATA;
            }
        }
        if (table->row_count > 0 && column->validity == NULL) {
            table_error(error, MILENA_ERR_DATA, "Máscara de validez ausente");
            return MILENA_ERR_DATA;
        }
        if (column->type == MILENA_COLUMN_STRING) {
            if (column->values.storage != NULL || column->values.size != table->row_count ||
                (table->row_count > 0 && column->strings == NULL)) {
                table_error(error, MILENA_ERR_DATA, "Storage string incoherente");
                return MILENA_ERR_DATA;
            }
            for (size_t row = 0; row < table->row_count; ++row) {
                if (column->validity[row] &&
                    (column->strings[row] == NULL || !utf8_valid(column->strings[row]))) {
                    table_error(error, MILENA_ERR_DATA, "String UTF-8 inválido");
                    return MILENA_ERR_DATA;
                }
            }
        } else {
            if (milena_array_validate(&column->values, error) != MILENA_OK ||
                column->values.ndim != 1 || column->values.size != table->row_count) {
                table_error(error, MILENA_ERR_DATA, "Array de columna inválido");
                return MILENA_ERR_DATA;
            }
            if (column->type == MILENA_COLUMN_CATEGORICAL) {
                if (!dtype_is_code(column->values.dtype) ||
                    (column->dictionary_size > 0 && column->dictionary == NULL)) {
                    table_error(error, MILENA_ERR_DATA, "Categorical inválido");
                    return MILENA_ERR_DATA;
                }
                for (size_t d = 0; d < column->dictionary_size; ++d) {
                    if (column->dictionary[d] == NULL ||
                        !utf8_valid(column->dictionary[d])) {
                        table_error(error, MILENA_ERR_DATA, "Diccionario UTF-8 inválido");
                        return MILENA_ERR_DATA;
                    }
                    for (size_t e = 0; e < d; ++e) {
                        if (strcmp(column->dictionary[d], column->dictionary[e]) == 0) {
                            table_error(error, MILENA_ERR_DATA, "Diccionario categórico duplicado");
                            return MILENA_ERR_DATA;
                        }
                    }
                }
                for (size_t row = 0; row < table->row_count; ++row) {
                    uint32_t code = 0;
                    if (column->validity[row] &&
                        categorical_code(column, row, &code, error) != MILENA_OK)
                        return error != NULL ? error->code : MILENA_ERR_DATA;
                }
            } else if (column->type != MILENA_COLUMN_ARRAY) {
                table_error(error, MILENA_ERR_DATA, "Tag de columna desconocido");
                return MILENA_ERR_DATA;
            }
        }
        bool has_null = false;
        for (size_t row = 0; row < table->row_count; ++row)
            if (!column->validity[row]) { has_null = true; break; }
        if (has_null && !column->nullable) {
            table_error(error, MILENA_ERR_DATA, "Columna no nullable contiene null");
            return MILENA_ERR_DATA;
        }
        for (size_t m = 0; m < column->metadata_count; ++m) {
            if (column->metadata[m].key == NULL || column->metadata[m].value == NULL) {
                table_error(error, MILENA_ERR_DATA, "Metadata de columna inválida");
                return MILENA_ERR_DATA;
            }
        }
    }
    for (size_t m = 0; m < table->metadata_count; ++m) {
        if (table->metadata[m].key == NULL || table->metadata[m].value == NULL) {
            table_error(error, MILENA_ERR_DATA, "Metadata de tabla inválida");
            return MILENA_ERR_DATA;
        }
    }
    return MILENA_OK;
}

static MilenaStatus table_reserve(MilenaTable *table, size_t required,
                                  MilenaError *error) {
    if (required <= table->capacity) return MILENA_OK;
    size_t capacity = table->capacity == 0 ? 4 : table->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            table_error(error, MILENA_ERR_OVERFLOW, "Demasiadas columnas");
            return MILENA_ERR_OVERFLOW;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(MilenaTableColumn)) {
        table_error(error, MILENA_ERR_OVERFLOW, "Columnas desbordan size_t");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaTableColumn *columns = (MilenaTableColumn *)realloc(
        table->columns, capacity * sizeof(*columns));
    if (columns == NULL) {
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar la tabla");
        return MILENA_ERR_MEMORY;
    }
    memset(columns + table->capacity, 0,
           (capacity - table->capacity) * sizeof(*columns));
    table->columns = columns;
    table->capacity = capacity;
    return MILENA_OK;
}

int milena_table_column_index(const MilenaTable *table, const char *name) {
    if (table == NULL || name == NULL || !table_has_magic(table)) return -1;
    for (size_t i = 0; i < table->column_count; ++i)
        if (table->columns[i].name != NULL &&
            strcmp(table->columns[i].name, name) == 0) {
            if (i > (size_t)INT_MAX) return -1;
            return (int)i;
        }
    return -1;
}

const MilenaTableColumn *milena_table_column(const MilenaTable *table,
                                             size_t index) {
    if (table == NULL || !table_has_magic(table) || index >= table->column_count)
        return NULL;
    return &table->columns[index];
}

static MilenaStatus validity_copy_make(bool **out, bool *nullable,
                                       size_t count, const bool *validity,
                                       MilenaError *error) {
    *out = NULL;
    *nullable = false;
    if (count == 0) return MILENA_OK;
    if (count > SIZE_MAX / sizeof(bool)) {
        table_error(error, MILENA_ERR_OVERFLOW, "Validez desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    bool *copy = (bool *)malloc(count * sizeof(bool));
    if (copy == NULL) {
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar validez");
        return MILENA_ERR_MEMORY;
    }
    for (size_t i = 0; i < count; ++i) {
        copy[i] = validity == NULL || validity[i];
        if (!copy[i]) *nullable = true;
    }
    *out = copy;
    return MILENA_OK;
}

static MilenaStatus column_name_check(MilenaTable *table, const char *name,
                                      size_t rows, MilenaError *error) {
    table_ensure_init(table);
    if (name == NULL || name[0] == '\0' || !utf8_valid(name)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Nombre de columna inválido");
        return MILENA_ERR_ARGUMENT;
    }
    if (milena_table_column_index(table, name) >= 0) {
        table_error(error, MILENA_ERR_ARGUMENT, "Nombre de columna duplicado");
        return MILENA_ERR_ARGUMENT;
    }
    if (table->column_count != 0 && table->row_count != rows) {
        table_error(error, MILENA_ERR_ARGUMENT, "Las columnas deben tener igual número de filas");
        return MILENA_ERR_ARGUMENT;
    }
    return MILENA_OK;
}

static MilenaStatus table_install_column(MilenaTable *table,
                                         MilenaTableColumn *column,
                                         size_t rows,
                                         MilenaError *error) {
    MilenaStatus status = table_reserve(table, table->column_count + 1, error);
    if (status != MILENA_OK) return status;
    table->columns[table->column_count] = *column;
    memset(column, 0, sizeof(*column));
    ++table->column_count;
    if (table->column_count == 1) table->row_count = rows;
    return MILENA_OK;
}

MilenaStatus milena_table_add_column_copy(MilenaTable *table,
                                          const char *name,
                                          const MilenaArray *values,
                                          const bool *validity,
                                          MilenaError *error) {
    if (table == NULL || values == NULL ||
        milena_array_validate(values, error) != MILENA_OK || values->ndim != 1) {
        table_error(error, MILENA_ERR_ARGUMENT, "Una columna requiere un array 1-D válido");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = column_name_check(table, name, values->size, error);
    if (status != MILENA_OK) return status;
    MilenaTableColumn column;
    memset(&column, 0, sizeof(column));
    column.name = milena_strdup(name);
    column.type = MILENA_COLUMN_ARRAY;
    size_t shape[] = {values->size};
    if (column.name == NULL) status = MILENA_ERR_MEMORY;
    if (status == MILENA_OK)
        status = milena_array_reshape_copy(&column.values, values, 1, shape, error);
    if (status == MILENA_OK)
        status = validity_copy_make(&column.validity, &column.nullable,
                                    values->size, validity, error);
    if (status == MILENA_OK)
        status = table_install_column(table, &column, values->size, error);
    if (status != MILENA_OK) {
        if (column.name == NULL && error != NULL && error->code == MILENA_OK)
            table_error(error, MILENA_ERR_MEMORY, "No se pudo copiar el nombre");
        column_destroy(&column);
    }
    return status;
}

MilenaStatus milena_table_add_string_column_copy(MilenaTable *table,
                                                 const char *name,
                                                 const char *const *values,
                                                 size_t row_count,
                                                 const bool *validity,
                                                 MilenaError *error) {
    if (table == NULL || (row_count != 0 && values == NULL)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Valores string inválidos");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = column_name_check(table, name, row_count, error);
    if (status != MILENA_OK) return status;
    MilenaTableColumn column;
    memset(&column, 0, sizeof(column));
    column.name = milena_strdup(name);
    column.type = MILENA_COLUMN_STRING;
    column.values.dtype = MILENA_DTYPE_UINT8;
    column.values.ndim = 1;
    column.values.size = row_count;
    column.values.itemsize = 1;
    if (column.name == NULL) status = MILENA_ERR_MEMORY;
    if (status == MILENA_OK)
        status = validity_copy_make(&column.validity, &column.nullable,
                                    row_count, validity, error);
    if (status == MILENA_OK && row_count != 0) {
        if (row_count > SIZE_MAX / sizeof(char *)) status = MILENA_ERR_OVERFLOW;
        else column.strings = (char **)calloc(row_count, sizeof(char *));
        if (column.strings == NULL) status = MILENA_ERR_MEMORY;
    }
    for (size_t row = 0; status == MILENA_OK && row < row_count; ++row) {
        if (!column.validity[row]) continue;
        if (values[row] == NULL || !utf8_valid(values[row])) {
            status = MILENA_ERR_DATA;
            table_error(error, status, "String no es UTF-8 válido");
            break;
        }
        column.strings[row] = milena_strdup(values[row]);
        if (column.strings[row] == NULL) status = MILENA_ERR_MEMORY;
    }
    if (status == MILENA_OK)
        status = table_install_column(table, &column, row_count, error);
    if (status != MILENA_OK) {
        if (error != NULL && error->code == MILENA_OK)
            table_error(error, status, status == MILENA_ERR_OVERFLOW ?
                        "Strings desbordan size_t" : "No se pudo copiar string");
        column_destroy(&column);
    }
    return status;
}

MilenaStatus milena_table_add_categorical_column_copy(
    MilenaTable *table, const char *name, const MilenaArray *codes,
    const char *const *dictionary, size_t dictionary_size,
    const bool *validity, MilenaError *error) {
    if (table == NULL || codes == NULL ||
        milena_array_validate(codes, error) != MILENA_OK || codes->ndim != 1 ||
        !dtype_is_code(codes->dtype) ||
        (dictionary_size != 0 && dictionary == NULL)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Categorical requiere códigos unsigned 1-D");
        return MILENA_ERR_ARGUMENT;
    }
    if (dictionary_size > UINT32_MAX) {
        table_error(error, MILENA_ERR_OVERFLOW, "Diccionario supera uint32");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaStatus status = column_name_check(table, name, codes->size, error);
    if (status != MILENA_OK) return status;
    MilenaTableColumn column;
    memset(&column, 0, sizeof(column));
    column.name = milena_strdup(name);
    column.type = MILENA_COLUMN_CATEGORICAL;
    column.dictionary_size = dictionary_size;
    size_t shape[] = {codes->size};
    if (column.name == NULL) status = MILENA_ERR_MEMORY;
    if (status == MILENA_OK)
        status = milena_array_reshape_copy(&column.values, codes, 1, shape, error);
    if (status == MILENA_OK)
        status = validity_copy_make(&column.validity, &column.nullable,
                                    codes->size, validity, error);
    if (status == MILENA_OK && dictionary_size != 0) {
        if (dictionary_size > SIZE_MAX / sizeof(char *)) status = MILENA_ERR_OVERFLOW;
        else column.dictionary = (char **)calloc(dictionary_size, sizeof(char *));
        if (column.dictionary == NULL) status = MILENA_ERR_MEMORY;
    }
    for (size_t i = 0; status == MILENA_OK && i < dictionary_size; ++i) {
        if (dictionary[i] == NULL || !utf8_valid(dictionary[i])) {
            status = MILENA_ERR_DATA;
            table_error(error, status, "Etiqueta categórica no es UTF-8");
            break;
        }
        for (size_t j = 0; j < i; ++j)
            if (strcmp(dictionary[i], dictionary[j]) == 0) status = MILENA_ERR_DATA;
        if (status != MILENA_OK) {
            table_error(error, status, "Etiqueta categórica duplicada");
            break;
        }
        column.dictionary[i] = milena_strdup(dictionary[i]);
        if (column.dictionary[i] == NULL) status = MILENA_ERR_MEMORY;
    }
    for (size_t row = 0; status == MILENA_OK && row < codes->size; ++row) {
        uint32_t code = 0;
        if (column.validity[row] &&
            categorical_code(&column, row, &code, error) != MILENA_OK)
            status = error != NULL ? error->code : MILENA_ERR_DATA;
    }
    if (status == MILENA_OK)
        status = table_install_column(table, &column, codes->size, error);
    if (status != MILENA_OK) {
        if (error != NULL && error->code == MILENA_OK)
            table_error(error, status, "No se pudo copiar categorical");
        column_destroy(&column);
    }
    return status;
}

bool milena_table_is_null(const MilenaTable *table, size_t column, size_t row) {
    return table == NULL || !table_has_magic(table) ||
           column >= table->column_count || row >= table->row_count ||
           table->columns[column].validity == NULL ||
           !table->columns[column].validity[row];
}

MilenaStatus milena_table_get_array_value(const MilenaTable *table,
                                          size_t column, size_t row,
                                          const void **value,
                                          MilenaError *error) {
    if (value == NULL || table == NULL || column >= table->column_count ||
        row >= table->row_count ||
        table->columns[column].type == MILENA_COLUMN_STRING) {
        table_error(error, MILENA_ERR_ARGUMENT, "Acceso array fuera de rango o tipo");
        return MILENA_ERR_ARGUMENT;
    }
    *value = NULL;
    if (milena_table_is_null(table, column, row)) return MILENA_OK;
    *value = array_row_const(&table->columns[column].values, row);
    return *value == NULL ? MILENA_ERR_DATA : MILENA_OK;
}

MilenaStatus milena_table_get_string(const MilenaTable *table,
                                     size_t column, size_t row,
                                     const char **value,
                                     MilenaError *error) {
    if (value == NULL || table == NULL || column >= table->column_count ||
        row >= table->row_count ||
        table->columns[column].type != MILENA_COLUMN_STRING) {
        table_error(error, MILENA_ERR_ARGUMENT, "Acceso string fuera de rango o tipo");
        return MILENA_ERR_ARGUMENT;
    }
    *value = milena_table_is_null(table, column, row) ? NULL :
             table->columns[column].strings[row];
    return MILENA_OK;
}

MilenaStatus milena_table_get_category(const MilenaTable *table,
                                       size_t column, size_t row,
                                       uint32_t *code, const char **label,
                                       MilenaError *error) {
    if (table == NULL || code == NULL || label == NULL ||
        column >= table->column_count || row >= table->row_count ||
        table->columns[column].type != MILENA_COLUMN_CATEGORICAL) {
        table_error(error, MILENA_ERR_ARGUMENT, "Acceso categorical fuera de rango o tipo");
        return MILENA_ERR_ARGUMENT;
    }
    *code = 0;
    *label = NULL;
    if (milena_table_is_null(table, column, row)) return MILENA_OK;
    MilenaStatus status = categorical_code(&table->columns[column], row,
                                            code, error);
    if (status == MILENA_OK) *label = table->columns[column].dictionary[*code];
    return status;
}

static MilenaStatus metadata_set(MilenaTableMetadata **items, size_t *count,
                                 const char *key, const char *value,
                                 MilenaError *error) {
    if (items == NULL || count == NULL || key == NULL || key[0] == '\0' ||
        value == NULL || !utf8_valid(key) || !utf8_valid(value)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Metadata clave/valor inválida");
        return MILENA_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp((*items)[i].key, key) == 0) {
            char *copy = milena_strdup(value);
            if (copy == NULL) {
                table_error(error, MILENA_ERR_MEMORY, "No se pudo copiar metadata");
                return MILENA_ERR_MEMORY;
            }
            free((*items)[i].value);
            (*items)[i].value = copy;
            return MILENA_OK;
        }
    }
    if (*count == SIZE_MAX || *count + 1 > SIZE_MAX / sizeof(**items)) {
        table_error(error, MILENA_ERR_OVERFLOW, "Metadata desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    char *key_copy = milena_strdup(key);
    char *value_copy = milena_strdup(value);
    if (key_copy == NULL || value_copy == NULL) {
        free(key_copy); free(value_copy);
        table_error(error, MILENA_ERR_MEMORY, "No se pudo copiar metadata");
        return MILENA_ERR_MEMORY;
    }
    MilenaTableMetadata *grown = (MilenaTableMetadata *)realloc(
        *items, (*count + 1) * sizeof(**items));
    if (grown == NULL) {
        free(key_copy); free(value_copy);
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar metadata");
        return MILENA_ERR_MEMORY;
    }
    *items = grown;
    grown[*count].key = key_copy;
    grown[*count].value = value_copy;
    ++*count;
    return MILENA_OK;
}

static const char *metadata_get(const MilenaTableMetadata *items, size_t count,
                                const char *key) {
    if (key == NULL) return NULL;
    for (size_t i = 0; i < count; ++i)
        if (strcmp(items[i].key, key) == 0) return items[i].value;
    return NULL;
}

MilenaStatus milena_table_set_metadata(MilenaTable *table, const char *key,
                                       const char *value,
                                       MilenaError *error) {
    if (table == NULL) return MILENA_ERR_ARGUMENT;
    table_ensure_init(table);
    return metadata_set(&table->metadata, &table->metadata_count,
                        key, value, error);
}

const char *milena_table_get_metadata(const MilenaTable *table,
                                      const char *key) {
    if (table == NULL || !table_has_magic(table)) return NULL;
    return metadata_get(table->metadata, table->metadata_count, key);
}

MilenaStatus milena_table_column_set_metadata(MilenaTable *table,
                                              const char *column_name,
                                              const char *key,
                                              const char *value,
                                              MilenaError *error) {
    int index = milena_table_column_index(table, column_name);
    if (index < 0) {
        table_error(error, MILENA_ERR_DATA, "Columna de metadata no existe");
        return MILENA_ERR_DATA;
    }
    return metadata_set(&table->columns[index].metadata,
                        &table->columns[index].metadata_count,
                        key, value, error);
}

const char *milena_table_column_get_metadata(const MilenaTable *table,
                                             const char *column_name,
                                             const char *key) {
    int index = milena_table_column_index(table, column_name);
    if (index < 0) return NULL;
    return metadata_get(table->columns[index].metadata,
                        table->columns[index].metadata_count, key);
}

static MilenaStatus metadata_clone_all(MilenaTableMetadata **out,
                                       size_t *out_count,
                                       const MilenaTableMetadata *source,
                                       size_t count,
                                       MilenaError *error) {
    for (size_t i = 0; i < count; ++i) {
        MilenaStatus status = metadata_set(out, out_count, source[i].key,
                                           source[i].value, error);
        if (status != MILENA_OK) return status;
    }
    return MILENA_OK;
}

static MilenaStatus add_column_rows(MilenaTable *destination,
                                    const MilenaTableColumn *source,
                                    const char *name,
                                    const size_t *rows, size_t row_count,
                                    MilenaError *error) {
    if (row_count != 0 && rows == NULL) return MILENA_ERR_ARGUMENT;
    bool *validity = row_count == 0 ? NULL :
        (bool *)malloc(row_count * sizeof(bool));
    if (row_count != 0 && validity == NULL) {
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar validez de filas");
        return MILENA_ERR_MEMORY;
    }
    MilenaStatus status = MILENA_OK;
    if (source->type == MILENA_COLUMN_STRING) {
        const char **strings = row_count == 0 ? NULL :
            (const char **)calloc(row_count, sizeof(char *));
        if (row_count != 0 && strings == NULL) status = MILENA_ERR_MEMORY;
        for (size_t i = 0; status == MILENA_OK && i < row_count; ++i) {
            if (rows[i] == SIZE_MAX) {
                validity[i] = false;
                strings[i] = NULL;
            } else {
                validity[i] = source->validity[rows[i]];
                strings[i] = validity[i] ? source->strings[rows[i]] : NULL;
            }
        }
        if (status == MILENA_OK)
            status = milena_table_add_string_column_copy(destination, name,
                                                          strings, row_count,
                                                          validity, error);
        free(strings);
    } else {
        size_t shape[] = {row_count};
        MilenaArray values = {0};
        status = milena_array_zeros(&values, source->values.dtype, 1, shape,
                                    error);
        for (size_t i = 0; status == MILENA_OK && i < row_count; ++i) {
            unsigned char *to = array_row_mut(&values, i, error);
            if (rows[i] == SIZE_MAX) {
                validity[i] = false;
            } else {
                const unsigned char *from = array_row_const(&source->values,
                                                            rows[i]);
                if (to == NULL || from == NULL) status = MILENA_ERR_DATA;
                else memcpy(to, from, values.itemsize);
                validity[i] = source->validity[rows[i]];
            }
        }
        if (status == MILENA_OK && source->type == MILENA_COLUMN_CATEGORICAL)
            status = milena_table_add_categorical_column_copy(
                destination, name, &values,
                (const char *const *)source->dictionary,
                source->dictionary_size, validity, error);
        else if (status == MILENA_OK)
            status = milena_table_add_column_copy(destination, name, &values,
                                                   validity, error);
        milena_array_release(&values);
    }
    free(validity);
    if (status == MILENA_OK) {
        MilenaTableColumn *added =
            &destination->columns[destination->column_count - 1];
        added->nullable = added->nullable || source->nullable;
        status = metadata_clone_all(&added->metadata, &added->metadata_count,
                                    source->metadata, source->metadata_count,
                                    error);
    }
    return status;
}

static MilenaStatus table_take_rows(MilenaTable *out,
                                    const MilenaTable *source,
                                    const size_t *rows, size_t row_count,
                                    MilenaError *error) {
    MilenaTable temporary;
    memset(&temporary, 0, sizeof(temporary));
    milena_table_init(&temporary);
    temporary.row_count = row_count;
    MilenaStatus status = metadata_clone_all(&temporary.metadata,
                                             &temporary.metadata_count,
                                             source->metadata,
                                             source->metadata_count, error);
    for (size_t c = 0; status == MILENA_OK && c < source->column_count; ++c)
        status = add_column_rows(&temporary, &source->columns[c],
                                 source->columns[c].name, rows, row_count,
                                 error);
    if (status == MILENA_OK) status = table_commit(out, &temporary, error);
    milena_table_destroy(&temporary);
    return status;
}

MilenaStatus milena_table_clone(MilenaTable *out, const MilenaTable *source,
                                MilenaError *error) {
    if (out == NULL || source == NULL || out == source) {
        table_error(error, MILENA_ERR_ARGUMENT, "Clone no permite alias de descriptor");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    size_t *rows = source->row_count == 0 ? NULL :
        (size_t *)malloc(source->row_count * sizeof(size_t));
    if (source->row_count != 0 && rows == NULL) {
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar clone");
        return MILENA_ERR_MEMORY;
    }
    for (size_t i = 0; i < source->row_count; ++i) rows[i] = i;
    status = table_take_rows(out, source, rows, source->row_count, error);
    free(rows);
    return status;
}

MilenaStatus milena_table_filter(MilenaTable *out,
                                 const MilenaTable *source,
                                 const MilenaArray *mask,
                                 MilenaError *error) {
    if (out == NULL || source == NULL || mask == NULL || out == source) {
        table_error(error, MILENA_ERR_ARGUMENT, "Filtro no permite argumentos nulos o alias");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    if (milena_array_validate(mask, error) != MILENA_OK ||
        mask->dtype != MILENA_DTYPE_BOOL || mask->ndim != 1 ||
        mask->size != source->row_count) {
        table_error(error, MILENA_ERR_ARGUMENT, "Filtro requiere máscara bool 1-D por fila");
        return MILENA_ERR_ARGUMENT;
    }
    size_t selected = 0;
    for (size_t row = 0; row < mask->size; ++row) {
        bool value = false;
        const unsigned char *p = array_row_const(mask, row);
        if (p == NULL) return MILENA_ERR_DATA;
        memcpy(&value, p, sizeof(value));
        if (value) {
            if (selected == SIZE_MAX) return MILENA_ERR_OVERFLOW;
            ++selected;
        }
    }
    size_t *rows = selected == 0 ? NULL :
        (size_t *)malloc(selected * sizeof(size_t));
    if (selected != 0 && rows == NULL) {
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar selección");
        return MILENA_ERR_MEMORY;
    }
    size_t at = 0;
    for (size_t row = 0; row < mask->size; ++row) {
        bool value = false;
        memcpy(&value, array_row_const(mask, row), sizeof(value));
        if (value) rows[at++] = row;
    }
    status = table_take_rows(out, source, rows, selected, error);
    free(rows);
    return status;
}

MilenaStatus milena_table_select_columns(MilenaTable *out,
                                         const MilenaTable *source,
                                         const char *const *names,
                                         size_t name_count,
                                         MilenaError *error) {
    if (out == NULL || source == NULL || out == source ||
        (name_count != 0 && names == NULL)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Selección de columnas inválida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    MilenaTable temporary;
    memset(&temporary, 0, sizeof(temporary));
    milena_table_init(&temporary);
    temporary.row_count = source->row_count;
    status = metadata_clone_all(&temporary.metadata, &temporary.metadata_count,
                                source->metadata, source->metadata_count, error);
    size_t *rows = source->row_count == 0 ? NULL :
        (size_t *)malloc(source->row_count * sizeof(size_t));
    if (source->row_count != 0 && rows == NULL) status = MILENA_ERR_MEMORY;
    for (size_t row = 0; row < source->row_count; ++row) rows[row] = row;
    for (size_t i = 0; status == MILENA_OK && i < name_count; ++i) {
        int index = milena_table_column_index(source, names[i]);
        if (index < 0) {
            table_error(error, MILENA_ERR_DATA, "Columna seleccionada no existe");
            status = MILENA_ERR_DATA;
        } else {
            status = add_column_rows(&temporary, &source->columns[index],
                                     source->columns[index].name, rows,
                                     source->row_count, error);
        }
    }
    free(rows);
    if (status == MILENA_OK) status = table_commit(out, &temporary, error);
    milena_table_destroy(&temporary);
    return status;
}

static bool signed_range(MilenaDType dtype, int64_t value) {
    switch (dtype) {
        case MILENA_DTYPE_INT8: return value >= INT8_MIN && value <= INT8_MAX;
        case MILENA_DTYPE_INT16: return value >= INT16_MIN && value <= INT16_MAX;
        case MILENA_DTYPE_INT32: return value >= INT32_MIN && value <= INT32_MAX;
        case MILENA_DTYPE_INT64: return true;
        default: return false;
    }
}

static bool unsigned_range(MilenaDType dtype, uint64_t value) {
    switch (dtype) {
        case MILENA_DTYPE_BOOL: return value <= 1u;
        case MILENA_DTYPE_UINT8: return value <= UINT8_MAX;
        case MILENA_DTYPE_UINT16: return value <= UINT16_MAX;
        case MILENA_DTYPE_UINT32: return value <= UINT32_MAX;
        case MILENA_DTYPE_UINT64: return true;
        default: return false;
    }
}

static MilenaStatus write_fill_value(MilenaTableColumn *column, size_t row,
                                     const MilenaTableValue *value,
                                     MilenaError *error) {
    unsigned char *p = array_row_mut(&column->values, row, error);
    if (p == NULL) return error != NULL ? error->code : MILENA_ERR_ARGUMENT;
    switch (column->values.dtype) {
        case MILENA_DTYPE_BOOL: memcpy(p, &value->as.boolean, sizeof(bool)); break;
        case MILENA_DTYPE_INT8: { int8_t v = (int8_t)value->as.i64; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_INT16: { int16_t v = (int16_t)value->as.i64; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_INT32: { int32_t v = (int32_t)value->as.i64; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_INT64: memcpy(p, &value->as.i64, sizeof(int64_t)); break;
        case MILENA_DTYPE_UINT8: { uint8_t v = (uint8_t)value->as.u64; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT16: { uint16_t v = (uint16_t)value->as.u64; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT32: { uint32_t v = (uint32_t)value->as.u64; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT64: memcpy(p, &value->as.u64, sizeof(uint64_t)); break;
        case MILENA_DTYPE_FLOAT32: memcpy(p, &value->as.f32, sizeof(float)); break;
        case MILENA_DTYPE_FLOAT64: memcpy(p, &value->as.f64, sizeof(double)); break;
        default:
            table_error(error, MILENA_ERR_UNSUPPORTED, "Fill-null no soporta complejos");
            return MILENA_ERR_UNSUPPORTED;
    }
    return MILENA_OK;
}

MilenaStatus milena_table_fill_null(MilenaTable *table,
                                    const char *column_name,
                                    const MilenaTableValue *value,
                                    MilenaError *error) {
    if (table == NULL || column_name == NULL || value == NULL ||
        milena_table_validate(table, error) != MILENA_OK) {
        table_error(error, MILENA_ERR_ARGUMENT, "Fill-null inválido");
        return MILENA_ERR_ARGUMENT;
    }
    int index = milena_table_column_index(table, column_name);
    if (index < 0) {
        table_error(error, MILENA_ERR_DATA, "Columna de fill-null no existe");
        return MILENA_ERR_DATA;
    }
    MilenaTableColumn *column = &table->columns[index];
    if (value->type != column->type ||
        (column->type == MILENA_COLUMN_ARRAY && value->dtype != column->values.dtype)) {
        table_error(error, MILENA_ERR_TYPE, "Valor de fill-null no coincide con la columna");
        return MILENA_ERR_TYPE;
    }
    if (column->type == MILENA_COLUMN_STRING &&
        (value->as.string == NULL || !utf8_valid(value->as.string))) {
        table_error(error, MILENA_ERR_DATA, "Fill string requiere UTF-8");
        return MILENA_ERR_DATA;
    }
    if (column->type == MILENA_COLUMN_CATEGORICAL &&
        value->as.category_code >= column->dictionary_size) {
        table_error(error, MILENA_ERR_DATA, "Código de fill fuera del diccionario");
        return MILENA_ERR_DATA;
    }
    if ((value->dtype >= MILENA_DTYPE_INT8 && value->dtype <= MILENA_DTYPE_INT64 &&
         !signed_range(value->dtype, value->as.i64)) ||
        (value->dtype >= MILENA_DTYPE_UINT8 && value->dtype <= MILENA_DTYPE_UINT64 &&
         !unsigned_range(value->dtype, value->as.u64))) {
        table_error(error, MILENA_ERR_OVERFLOW, "Valor de fill fuera de rango");
        return MILENA_ERR_OVERFLOW;
    }
    char **copies = NULL;
    if (column->type == MILENA_COLUMN_STRING && table->row_count != 0) {
        copies = (char **)calloc(table->row_count, sizeof(char *));
        if (copies == NULL) return MILENA_ERR_MEMORY;
        for (size_t row = 0; row < table->row_count; ++row) {
            if (!column->validity[row]) {
                copies[row] = milena_strdup(value->as.string);
                if (copies[row] == NULL) {
                    for (size_t j = 0; j < row; ++j) free(copies[j]);
                    free(copies);
                    table_error(error, MILENA_ERR_MEMORY, "No se pudo copiar fill string");
                    return MILENA_ERR_MEMORY;
                }
            }
        }
    }
    for (size_t row = 0; row < table->row_count; ++row) {
        if (column->validity[row]) continue;
        MilenaStatus status = MILENA_OK;
        if (column->type == MILENA_COLUMN_STRING) {
            free(column->strings[row]);
            column->strings[row] = copies[row];
            copies[row] = NULL;
        } else if (column->type == MILENA_COLUMN_CATEGORICAL) {
            unsigned char *p = array_row_mut(&column->values, row, error);
            uint32_t code = value->as.category_code;
            if (p == NULL) status = MILENA_ERR_ARGUMENT;
            else {
                switch (column->values.dtype) {
                    case MILENA_DTYPE_UINT8: { uint8_t v = (uint8_t)code; memcpy(p, &v, sizeof(v)); break; }
                    case MILENA_DTYPE_UINT16: { uint16_t v = (uint16_t)code; memcpy(p, &v, sizeof(v)); break; }
                    case MILENA_DTYPE_UINT32: memcpy(p, &code, sizeof(code)); break;
                    case MILENA_DTYPE_UINT64: { uint64_t v = code; memcpy(p, &v, sizeof(v)); break; }
                    default: status = MILENA_ERR_TYPE; break;
                }
            }
        } else status = write_fill_value(column, row, value, error);
        if (status != MILENA_OK) {
            if (copies != NULL) {
                for (size_t j = 0; j < table->row_count; ++j) free(copies[j]);
                free(copies);
            }
            return status;
        }
        column->validity[row] = true;
    }
    free(copies);
    return MILENA_OK;
}

MilenaStatus milena_table_fill_null_f64(MilenaTable *table,
                                        const char *column_name,
                                        double value,
                                        MilenaError *error) {
    MilenaTableValue fill;
    memset(&fill, 0, sizeof(fill));
    fill.type = MILENA_COLUMN_ARRAY;
    fill.dtype = MILENA_DTYPE_FLOAT64;
    fill.as.f64 = value;
    return milena_table_fill_null(table, column_name, &fill, error);
}

MilenaStatus milena_table_drop_null_columns(MilenaTable *out,
                                            const MilenaTable *source,
                                            const char *const *column_names,
                                            size_t column_count,
                                            MilenaError *error) {
    if (out == NULL || source == NULL || out == source ||
        (column_count != 0 && column_names == NULL)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Drop-null inválido");
        return MILENA_ERR_ARGUMENT;
    }
    if (milena_table_validate(source, error) != MILENA_OK)
        return error != NULL ? error->code : MILENA_ERR_DATA;
    size_t *indices = column_count == 0 ? NULL :
        (size_t *)malloc(column_count * sizeof(size_t));
    if (column_count != 0 && indices == NULL) return MILENA_ERR_MEMORY;
    for (size_t i = 0; i < column_count; ++i) {
        int index = milena_table_column_index(source, column_names[i]);
        if (index < 0) {
            free(indices);
            table_error(error, MILENA_ERR_DATA, "Columna de drop-null no existe");
            return MILENA_ERR_DATA;
        }
        indices[i] = (size_t)index;
    }
    size_t kept = 0;
    for (size_t row = 0; row < source->row_count; ++row) {
        bool keep = true;
        size_t checks = column_count == 0 ? source->column_count : column_count;
        for (size_t i = 0; i < checks; ++i) {
            size_t c = column_count == 0 ? i : indices[i];
            if (!source->columns[c].validity[row]) { keep = false; break; }
        }
        if (keep) ++kept;
    }
    size_t *rows = kept == 0 ? NULL : (size_t *)malloc(kept * sizeof(size_t));
    if (kept != 0 && rows == NULL) { free(indices); return MILENA_ERR_MEMORY; }
    size_t at = 0;
    for (size_t row = 0; row < source->row_count; ++row) {
        bool keep = true;
        size_t checks = column_count == 0 ? source->column_count : column_count;
        for (size_t i = 0; i < checks; ++i) {
            size_t c = column_count == 0 ? i : indices[i];
            if (!source->columns[c].validity[row]) { keep = false; break; }
        }
        if (keep) rows[at++] = row;
    }
    free(indices);
    MilenaStatus status = table_take_rows(out, source, rows, kept, error);
    free(rows);
    return status;
}

MilenaStatus milena_table_drop_null(MilenaTable *out,
                                    const MilenaTable *source,
                                    MilenaError *error) {
    return milena_table_drop_null_columns(out, source, NULL, 0, error);
}

static int compare_real_cells(const MilenaTableColumn *column,
                              size_t left, size_t right,
                              bool *left_nan, bool *right_nan) {
    const unsigned char *a = array_row_const(&column->values, left);
    const unsigned char *b = array_row_const(&column->values, right);
    *left_nan = false;
    *right_nan = false;
#define TABLE_COMPARE_VALUE(type_) do { \
    type_ av; type_ bv; \
    memcpy(&av, a, sizeof(av)); memcpy(&bv, b, sizeof(bv)); \
    return av < bv ? -1 : (av > bv ? 1 : 0); \
} while (0)
    switch (column->values.dtype) {
        case MILENA_DTYPE_BOOL: TABLE_COMPARE_VALUE(bool);
        case MILENA_DTYPE_INT8: TABLE_COMPARE_VALUE(int8_t);
        case MILENA_DTYPE_INT16: TABLE_COMPARE_VALUE(int16_t);
        case MILENA_DTYPE_INT32: TABLE_COMPARE_VALUE(int32_t);
        case MILENA_DTYPE_INT64: TABLE_COMPARE_VALUE(int64_t);
        case MILENA_DTYPE_UINT8: TABLE_COMPARE_VALUE(uint8_t);
        case MILENA_DTYPE_UINT16: TABLE_COMPARE_VALUE(uint16_t);
        case MILENA_DTYPE_UINT32: TABLE_COMPARE_VALUE(uint32_t);
        case MILENA_DTYPE_UINT64: TABLE_COMPARE_VALUE(uint64_t);
        case MILENA_DTYPE_FLOAT32: {
            float av; float bv;
            memcpy(&av, a, sizeof(av)); memcpy(&bv, b, sizeof(bv));
            *left_nan = isnan(av); *right_nan = isnan(bv);
            return av < bv ? -1 : (av > bv ? 1 : 0);
        }
        case MILENA_DTYPE_FLOAT64: {
            double av; double bv;
            memcpy(&av, a, sizeof(av)); memcpy(&bv, b, sizeof(bv));
            *left_nan = isnan(av); *right_nan = isnan(bv);
            return av < bv ? -1 : (av > bv ? 1 : 0);
        }
        default: return 0;
    }
#undef TABLE_COMPARE_VALUE
}

static int compare_column_rows(const MilenaTableColumn *column,
                               size_t left, size_t right, bool ascending) {
    bool left_valid = column->validity[left];
    bool right_valid = column->validity[right];
    /* Nulls remain last in both directions. */
    if (left_valid != right_valid) return left_valid ? -1 : 1;
    if (!left_valid) return 0;
    int comparison = 0;
    bool left_nan = false;
    bool right_nan = false;
    if (column->type == MILENA_COLUMN_STRING) {
        comparison = strcmp(column->strings[left], column->strings[right]);
        comparison = comparison < 0 ? -1 : (comparison > 0 ? 1 : 0);
    } else if (column->type == MILENA_COLUMN_CATEGORICAL) {
        uint32_t lc = 0; uint32_t rc = 0;
        (void)categorical_code(column, left, &lc, NULL);
        (void)categorical_code(column, right, &rc, NULL);
        comparison = strcmp(column->dictionary[lc], column->dictionary[rc]);
        comparison = comparison < 0 ? -1 : (comparison > 0 ? 1 : 0);
    } else {
        comparison = compare_real_cells(column, left, right,
                                        &left_nan, &right_nan);
        /* NaNs remain after finite values in both directions; NaNs tie. */
        if (left_nan != right_nan) return left_nan ? 1 : -1;
        if (left_nan) return 0;
    }
    return ascending ? comparison : -comparison;
}

static int compare_key_rows(const MilenaTable *source,
                            const size_t *indices,
                            const MilenaSortKey *keys, size_t key_count,
                            size_t left, size_t right) {
    for (size_t i = 0; i < key_count; ++i) {
        int comparison = compare_column_rows(&source->columns[indices[i]],
                                             left, right,
                                             keys[i].ascending);
        if (comparison != 0) return comparison;
    }
    return 0;
}

MilenaStatus milena_table_sort_keys(MilenaTable *out,
                                    const MilenaTable *source,
                                    const MilenaSortKey *keys,
                                    size_t key_count,
                                    MilenaError *error) {
    if (out == NULL || source == NULL || out == source ||
        key_count == 0 || keys == NULL) {
        table_error(error, MILENA_ERR_ARGUMENT, "Sort requiere una o más claves sin alias");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    size_t *indices = (size_t *)malloc(key_count * sizeof(size_t));
    if (indices == NULL) return MILENA_ERR_MEMORY;
    for (size_t i = 0; i < key_count; ++i) {
        int index = milena_table_column_index(source, keys[i].column);
        if (index < 0) {
            free(indices);
            table_error(error, MILENA_ERR_DATA, "Clave de sort no existe");
            return MILENA_ERR_DATA;
        }
        const MilenaTableColumn *column = &source->columns[index];
        if (column->type == MILENA_COLUMN_ARRAY &&
            !dtype_is_real(column->values.dtype)) {
            free(indices);
            table_error(error, MILENA_ERR_UNSUPPORTED, "Sort no soporta complejos");
            return MILENA_ERR_UNSUPPORTED;
        }
        indices[i] = (size_t)index;
    }
    size_t n = source->row_count;
    size_t *order_storage = n == 0 ? NULL : (size_t *)malloc(n * sizeof(size_t));
    size_t *work_storage = n == 0 ? NULL : (size_t *)malloc(n * sizeof(size_t));
    if (n != 0 && (order_storage == NULL || work_storage == NULL)) {
        free(indices); free(order_storage); free(work_storage);
        table_error(error, MILENA_ERR_MEMORY, "No se pudo reservar mergesort");
        return MILENA_ERR_MEMORY;
    }
    size_t *order = order_storage;
    size_t *work = work_storage;
    for (size_t i = 0; i < n; ++i) order[i] = i;
    for (size_t width = 1; width < n;) {
        for (size_t begin = 0; begin < n;) {
            size_t middle = begin + (width < n - begin ? width : n - begin);
            size_t end = middle + (width < n - middle ? width : n - middle);
            size_t i = begin; size_t j = middle; size_t k = begin;
            while (i < middle && j < end) {
                if (compare_key_rows(source, indices, keys, key_count,
                                     order[i], order[j]) <= 0)
                    work[k++] = order[i++];
                else work[k++] = order[j++];
            }
            while (i < middle) work[k++] = order[i++];
            while (j < end) work[k++] = order[j++];
            begin = end;
        }
        size_t *swap = order; order = work; work = swap;
        if (width > n / 2) width = n;
        else width *= 2;
    }
    status = table_take_rows(out, source, order, n, error);
    free(indices); free(order_storage); free(work_storage);
    return status;
}

MilenaStatus milena_table_sort(MilenaTable *out,
                               const MilenaTable *source,
                               const char *column_name,
                               bool ascending,
                               MilenaError *error) {
    MilenaSortKey key = {column_name, ascending};
    return milena_table_sort_keys(out, source, &key, 1, error);
}

typedef struct {
    uint64_t hash;
    size_t value_plus_one;
} TableHashSlot;

static uint64_t hash_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t hash_bytes(uint64_t hash, const unsigned char *bytes,
                           size_t count) {
    for (size_t i = 0; i < count; ++i) hash = hash_mix(hash, bytes[i]);
    return hash;
}

static uint64_t cell_hash(const MilenaTableColumn *column, size_t row) {
    if (!column->validity[row]) return UINT64_C(0x9e3779b97f4a7c15);
    uint64_t hash = UINT64_C(1469598103934665603);
    if (column->type == MILENA_COLUMN_STRING)
        return hash_bytes(hash, (const unsigned char *)column->strings[row],
                          strlen(column->strings[row]));
    if (column->type == MILENA_COLUMN_CATEGORICAL) {
        uint32_t code = 0;
        (void)categorical_code(column, row, &code, NULL);
        return hash_bytes(hash,
                          (const unsigned char *)column->dictionary[code],
                          strlen(column->dictionary[code]));
    }
    const unsigned char *p = array_row_const(&column->values, row);
    if (column->values.dtype == MILENA_DTYPE_FLOAT32) {
        float value; memcpy(&value, p, sizeof(value));
        if (isnan(value)) return hash_mix(hash, UINT64_C(0x7fc00000));
        if (value == 0.0f) value = 0.0f;
        return hash_bytes(hash, (const unsigned char *)&value, sizeof(value));
    }
    if (column->values.dtype == MILENA_DTYPE_FLOAT64) {
        double value; memcpy(&value, p, sizeof(value));
        if (isnan(value)) return hash_mix(hash, UINT64_C(0x7ff8000000000000));
        if (value == 0.0) value = 0.0;
        return hash_bytes(hash, (const unsigned char *)&value, sizeof(value));
    }
    return hash_bytes(hash, p, column->values.itemsize);
}

static bool cell_equal_cross(const MilenaTableColumn *left, size_t left_row,
                             const MilenaTableColumn *right, size_t right_row,
                             bool null_equal) {
    bool lv = left->validity[left_row];
    bool rv = right->validity[right_row];
    if (!lv || !rv) return null_equal && !lv && !rv;
    if (left->type != right->type) return false;
    if (left->type == MILENA_COLUMN_STRING)
        return strcmp(left->strings[left_row], right->strings[right_row]) == 0;
    if (left->type == MILENA_COLUMN_CATEGORICAL) {
        uint32_t lc = 0; uint32_t rc = 0;
        if (categorical_code(left, left_row, &lc, NULL) != MILENA_OK ||
            categorical_code(right, right_row, &rc, NULL) != MILENA_OK) return false;
        return strcmp(left->dictionary[lc], right->dictionary[rc]) == 0;
    }
    if (left->values.dtype != right->values.dtype) return false;
    const unsigned char *a = array_row_const(&left->values, left_row);
    const unsigned char *b = array_row_const(&right->values, right_row);
    if (left->values.dtype == MILENA_DTYPE_FLOAT32) {
        float av; float bv; memcpy(&av, a, sizeof(av)); memcpy(&bv, b, sizeof(bv));
        return (isnan(av) && isnan(bv)) || av == bv;
    }
    if (left->values.dtype == MILENA_DTYPE_FLOAT64) {
        double av; double bv; memcpy(&av, a, sizeof(av)); memcpy(&bv, b, sizeof(bv));
        return (isnan(av) && isnan(bv)) || av == bv;
    }
    return memcmp(a, b, left->values.itemsize) == 0;
}

static uint64_t key_hash(const MilenaTable *table, const size_t *keys,
                         size_t key_count, size_t row) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < key_count; ++i)
        hash = hash_mix(hash, cell_hash(&table->columns[keys[i]], row));
    return hash == 0 ? UINT64_C(1) : hash;
}

static bool key_equal_same(const MilenaTable *table, const size_t *keys,
                           size_t key_count, size_t left, size_t right) {
    for (size_t i = 0; i < key_count; ++i)
        if (!cell_equal_cross(&table->columns[keys[i]], left,
                              &table->columns[keys[i]], right, true)) return false;
    return true;
}

static MilenaStatus hash_capacity(size_t rows, size_t *capacity,
                                  MilenaError *error) {
    size_t needed = 8;
    if (rows > SIZE_MAX / 2) {
        table_error(error, MILENA_ERR_OVERFLOW, "Hash desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    size_t target = rows * 2;
    while (needed < target) {
        if (needed > SIZE_MAX / 2) {
            table_error(error, MILENA_ERR_OVERFLOW, "Hash desborda size_t");
            return MILENA_ERR_OVERFLOW;
        }
        needed *= 2;
    }
    *capacity = needed;
    return MILENA_OK;
}

static const char *aggregate_suffix(MilenaAggregateOp operation) {
    switch (operation) {
        case MILENA_AGG_COUNT: return "count";
        case MILENA_AGG_SUM: return "sum";
        case MILENA_AGG_MEAN: return "mean";
        case MILENA_AGG_MIN: return "min";
        case MILENA_AGG_MAX: return "max";
        default: return "invalid";
    }
}

static char *aggregate_default_name(const char *column,
                                    MilenaAggregateOp operation,
                                    MilenaError *error) {
    size_t a = strlen(column); size_t b = strlen(aggregate_suffix(operation));
    size_t length = 0;
    if (!milena_size_add(a, b, &length) ||
        !milena_size_add(length, 2, &length)) {
        table_error(error, MILENA_ERR_OVERFLOW, "Nombre agregado desborda size_t");
        return NULL;
    }
    char *name = (char *)malloc(length);
    if (name == NULL) return NULL;
    (void)snprintf(name, length, "%s_%s", column, aggregate_suffix(operation));
    return name;
}

static bool dtype_signed_integer(MilenaDType dtype) {
    return dtype >= MILENA_DTYPE_INT8 && dtype <= MILENA_DTYPE_INT64;
}

static bool dtype_unsigned_integer(MilenaDType dtype) {
    return dtype >= MILENA_DTYPE_UINT8 && dtype <= MILENA_DTYPE_UINT64;
}

static int64_t read_signed(const MilenaTableColumn *column, size_t row) {
    const unsigned char *p = array_row_const(&column->values, row);
    switch (column->values.dtype) {
        case MILENA_DTYPE_INT8: { int8_t v; memcpy(&v, p, sizeof(v)); return v; }
        case MILENA_DTYPE_INT16: { int16_t v; memcpy(&v, p, sizeof(v)); return v; }
        case MILENA_DTYPE_INT32: { int32_t v; memcpy(&v, p, sizeof(v)); return v; }
        case MILENA_DTYPE_INT64: { int64_t v; memcpy(&v, p, sizeof(v)); return v; }
        default: return 0;
    }
}

static uint64_t read_unsigned(const MilenaTableColumn *column, size_t row) {
    const unsigned char *p = array_row_const(&column->values, row);
    switch (column->values.dtype) {
        case MILENA_DTYPE_BOOL: { bool v; memcpy(&v, p, sizeof(v)); return v ? 1u : 0u; }
        case MILENA_DTYPE_UINT8: { uint8_t v; memcpy(&v, p, sizeof(v)); return v; }
        case MILENA_DTYPE_UINT16: { uint16_t v; memcpy(&v, p, sizeof(v)); return v; }
        case MILENA_DTYPE_UINT32: { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }
        case MILENA_DTYPE_UINT64: { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }
        default: return 0;
    }
}

static double read_float_value(const MilenaTableColumn *column, size_t row) {
    const unsigned char *p = array_row_const(&column->values, row);
    if (column->values.dtype == MILENA_DTYPE_FLOAT32) {
        float v; memcpy(&v, p, sizeof(v)); return (double)v;
    }
    double v; memcpy(&v, p, sizeof(v)); return v;
}

static bool checked_signed_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool checked_unsigned_add(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return false;
    *out = a + b;
    return true;
}

static void write_signed_to_array(MilenaArray *array, size_t row,
                                  int64_t value) {
    unsigned char *p = array_row_mut(array, row, NULL);
    switch (array->dtype) {
        case MILENA_DTYPE_INT8: { int8_t v = (int8_t)value; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_INT16: { int16_t v = (int16_t)value; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_INT32: { int32_t v = (int32_t)value; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_INT64: memcpy(p, &value, sizeof(value)); break;
        default: break;
    }
}

static void write_unsigned_to_array(MilenaArray *array, size_t row,
                                    uint64_t value) {
    unsigned char *p = array_row_mut(array, row, NULL);
    switch (array->dtype) {
        case MILENA_DTYPE_BOOL: { bool v = value != 0; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT8: { uint8_t v = (uint8_t)value; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT16: { uint16_t v = (uint16_t)value; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT32: { uint32_t v = (uint32_t)value; memcpy(p, &v, sizeof(v)); break; }
        case MILENA_DTYPE_UINT64: memcpy(p, &value, sizeof(value)); break;
        default: break;
    }
}

static void write_float_to_array(MilenaArray *array, size_t row,
                                 double value) {
    unsigned char *p = array_row_mut(array, row, NULL);
    if (array->dtype == MILENA_DTYPE_FLOAT32) {
        float v = (float)value; memcpy(p, &v, sizeof(v));
    } else memcpy(p, &value, sizeof(value));
}

static bool value_supported_for_aggregate(const MilenaTableColumn *column,
                                          MilenaAggregateOp operation) {
    if (operation == MILENA_AGG_COUNT) return true;
    if (column->type != MILENA_COLUMN_ARRAY ||
        !dtype_is_real(column->values.dtype)) return false;
    if (operation == MILENA_AGG_SUM && column->values.dtype == MILENA_DTYPE_BOOL)
        return true;
    return operation >= MILENA_AGG_SUM && operation <= MILENA_AGG_MAX;
}

static MilenaDType aggregate_dtype(const MilenaTableColumn *column,
                                   MilenaAggregateOp operation) {
    if (operation == MILENA_AGG_COUNT) return MILENA_DTYPE_INT64;
    if (operation == MILENA_AGG_MEAN) return MILENA_DTYPE_FLOAT64;
    if (operation == MILENA_AGG_SUM && column->values.dtype == MILENA_DTYPE_BOOL)
        return MILENA_DTYPE_UINT64;
    return column->values.dtype;
}

static MilenaStatus build_one_aggregate(MilenaTable *destination,
                                        const MilenaTable *source,
                                        const MilenaAggregateSpec *spec,
                                        const size_t *group_of,
                                        size_t group_count,
                                        const char *name,
                                        MilenaError *error) {
    int value_index = milena_table_column_index(source, spec->value_column);
    if (value_index < 0) return MILENA_ERR_DATA;
    const MilenaTableColumn *column = &source->columns[value_index];
    MilenaDType out_dtype = aggregate_dtype(column, spec->operation);
    size_t shape[] = {group_count};
    MilenaArray values = {0};
    MilenaStatus status = milena_array_zeros(&values, out_dtype, 1, shape, error);
    bool *validity = group_count == 0 ? NULL :
        (bool *)calloc(group_count, sizeof(bool));
    size_t *counts = group_count == 0 ? NULL :
        (size_t *)calloc(group_count, sizeof(size_t));
    int64_t *signed_acc = group_count == 0 ? NULL :
        (int64_t *)calloc(group_count, sizeof(int64_t));
    uint64_t *unsigned_acc = group_count == 0 ? NULL :
        (uint64_t *)calloc(group_count, sizeof(uint64_t));
    double *float_acc = group_count == 0 ? NULL :
        (double *)calloc(group_count, sizeof(double));
    double *compensation = group_count == 0 ? NULL :
        (double *)calloc(group_count, sizeof(double));
    if (status == MILENA_OK && group_count != 0 &&
        (validity == NULL || counts == NULL || signed_acc == NULL ||
         unsigned_acc == NULL || float_acc == NULL || compensation == NULL))
        status = MILENA_ERR_MEMORY;
    for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row) {
        if (!column->validity[row]) continue;
        size_t g = group_of[row];
        if (counts[g] == SIZE_MAX) { status = MILENA_ERR_OVERFLOW; break; }
        ++counts[g];
        if (spec->operation == MILENA_AGG_COUNT) continue;
        bool first = counts[g] == 1;
        if (dtype_signed_integer(column->values.dtype)) {
            int64_t current = read_signed(column, row);
            if (spec->operation == MILENA_AGG_SUM) {
                if (!checked_signed_add(signed_acc[g], current, &signed_acc[g]))
                    status = MILENA_ERR_OVERFLOW;
            } else if (first ||
                       (spec->operation == MILENA_AGG_MIN && current < signed_acc[g]) ||
                       (spec->operation == MILENA_AGG_MAX && current > signed_acc[g]))
                signed_acc[g] = current;
            if (spec->operation == MILENA_AGG_MEAN) {
                double y = (double)current - compensation[g];
                double t = float_acc[g] + y;
                compensation[g] = (t - float_acc[g]) - y;
                float_acc[g] = t;
            }
        } else if (column->values.dtype == MILENA_DTYPE_BOOL ||
                   dtype_unsigned_integer(column->values.dtype)) {
            uint64_t current = read_unsigned(column, row);
            if (spec->operation == MILENA_AGG_SUM) {
                if (!checked_unsigned_add(unsigned_acc[g], current,
                                          &unsigned_acc[g]))
                    status = MILENA_ERR_OVERFLOW;
            } else if (first ||
                       (spec->operation == MILENA_AGG_MIN && current < unsigned_acc[g]) ||
                       (spec->operation == MILENA_AGG_MAX && current > unsigned_acc[g]))
                unsigned_acc[g] = current;
            if (spec->operation == MILENA_AGG_MEAN) {
                double y = (double)current - compensation[g];
                double t = float_acc[g] + y;
                compensation[g] = (t - float_acc[g]) - y;
                float_acc[g] = t;
            }
        } else {
            double current = read_float_value(column, row);
            if (spec->operation == MILENA_AGG_SUM ||
                spec->operation == MILENA_AGG_MEAN) {
                double y = current - compensation[g];
                double t = float_acc[g] + y;
                compensation[g] = (t - float_acc[g]) - y;
                float_acc[g] = t;
            } else if (first || isnan(current) ||
                       (!isnan(float_acc[g]) &&
                        ((spec->operation == MILENA_AGG_MIN && current < float_acc[g]) ||
                         (spec->operation == MILENA_AGG_MAX && current > float_acc[g]))))
                float_acc[g] = current;
        }
    }
    for (size_t g = 0; status == MILENA_OK && g < group_count; ++g) {
        validity[g] = spec->operation == MILENA_AGG_COUNT || counts[g] != 0;
        if (spec->operation == MILENA_AGG_COUNT) {
            if (counts[g] > (size_t)INT64_MAX) status = MILENA_ERR_OVERFLOW;
            else write_signed_to_array(&values, g, (int64_t)counts[g]);
        } else if (counts[g] == 0) {
            continue;
        } else if (spec->operation == MILENA_AGG_MEAN) {
            double sum = (column->values.dtype == MILENA_DTYPE_FLOAT32 ||
                          column->values.dtype == MILENA_DTYPE_FLOAT64) ?
                         float_acc[g] :
                         (dtype_signed_integer(column->values.dtype) ?
                          (double)signed_acc[g] : (double)unsigned_acc[g]);
            if (dtype_signed_integer(column->values.dtype) ||
                dtype_unsigned_integer(column->values.dtype) ||
                column->values.dtype == MILENA_DTYPE_BOOL)
                sum = float_acc[g];
            write_float_to_array(&values, g, sum / (double)counts[g]);
        } else if (dtype_signed_integer(column->values.dtype)) {
            if (!signed_range(out_dtype, signed_acc[g])) status = MILENA_ERR_OVERFLOW;
            else write_signed_to_array(&values, g, signed_acc[g]);
        } else if (column->values.dtype == MILENA_DTYPE_BOOL ||
                   dtype_unsigned_integer(column->values.dtype)) {
            if (!unsigned_range(out_dtype, unsigned_acc[g])) status = MILENA_ERR_OVERFLOW;
            else write_unsigned_to_array(&values, g, unsigned_acc[g]);
        } else write_float_to_array(&values, g, float_acc[g]);
    }
    if (status == MILENA_ERR_OVERFLOW)
        table_error(error, status, "Overflow comprobado en agregación");
    else if (status == MILENA_ERR_MEMORY)
        table_error(error, status, "No se pudo reservar agregación");
    if (status == MILENA_OK)
        status = milena_table_add_column_copy(destination, name, &values,
                                               validity, error);
    milena_array_release(&values);
    free(validity); free(counts); free(signed_acc); free(unsigned_acc);
    free(float_acc); free(compensation);
    return status;
}

MilenaStatus milena_table_group_by(MilenaTable *out,
                                   const MilenaTable *source,
                                   const char *const *key_columns,
                                   size_t key_count,
                                   const MilenaAggregateSpec *aggregates,
                                   size_t aggregate_count,
                                   MilenaError *error) {
    if (out == NULL || source == NULL || out == source ||
        (key_count != 0 && key_columns == NULL) ||
        (aggregate_count != 0 && aggregates == NULL)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Group-by requiere claves y salida sin alias");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    size_t *keys = key_count == 0 ? NULL :
        (size_t *)malloc(key_count * sizeof(size_t));
    if (key_count != 0 && keys == NULL) return MILENA_ERR_MEMORY;
    for (size_t i = 0; i < key_count; ++i) {
        int index = milena_table_column_index(source, key_columns[i]);
        if (index < 0) { status = MILENA_ERR_DATA; break; }
        keys[i] = (size_t)index;
    }
    for (size_t i = 0; status == MILENA_OK && i < aggregate_count; ++i) {
        int index = milena_table_column_index(source, aggregates[i].value_column);
        if (index < 0 || aggregates[i].operation < MILENA_AGG_COUNT ||
            aggregates[i].operation > MILENA_AGG_MAX ||
            !value_supported_for_aggregate(&source->columns[index],
                                           aggregates[i].operation))
            status = index < 0 ? MILENA_ERR_DATA : MILENA_ERR_UNSUPPORTED;
    }
    if (status != MILENA_OK) {
        free(keys);
        table_error(error, status, "Clave/valor u operación de group-by inválida");
        return status;
    }
    size_t capacity = 0;
    status = hash_capacity(source->row_count, &capacity, error);
    TableHashSlot *slots = status == MILENA_OK ?
        (TableHashSlot *)calloc(capacity, sizeof(TableHashSlot)) : NULL;
    size_t *first_rows = source->row_count == 0 ? NULL :
        (size_t *)malloc(source->row_count * sizeof(size_t));
    size_t *group_of = source->row_count == 0 ? NULL :
        (size_t *)malloc(source->row_count * sizeof(size_t));
    if (status == MILENA_OK &&
        (slots == NULL || (source->row_count != 0 &&
                           (first_rows == NULL || group_of == NULL))))
        status = MILENA_ERR_MEMORY;
    size_t group_count = 0;
    for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row) {
        uint64_t hash = key_hash(source, keys, key_count, row);
        size_t slot = (size_t)hash & (capacity - 1);
        while (slots[slot].value_plus_one != 0) {
            size_t group = slots[slot].value_plus_one - 1;
            if (slots[slot].hash == hash &&
                key_equal_same(source, keys, key_count,
                               first_rows[group], row)) break;
            slot = (slot + 1) & (capacity - 1);
        }
        if (slots[slot].value_plus_one == 0) {
            first_rows[group_count] = row;
            slots[slot].hash = hash;
            slots[slot].value_plus_one = group_count + 1;
            group_of[row] = group_count++;
        } else group_of[row] = slots[slot].value_plus_one - 1;
    }
    MilenaTable temporary;
    memset(&temporary, 0, sizeof(temporary));
    milena_table_init(&temporary);
    temporary.row_count = group_count;
    for (size_t i = 0; status == MILENA_OK && i < key_count; ++i)
        status = add_column_rows(&temporary, &source->columns[keys[i]],
                                 source->columns[keys[i]].name,
                                 first_rows, group_count, error);
    for (size_t i = 0; status == MILENA_OK && i < aggregate_count; ++i) {
        char *generated = aggregates[i].output_name == NULL ?
            aggregate_default_name(aggregates[i].value_column,
                                   aggregates[i].operation, error) : NULL;
        const char *name = aggregates[i].output_name != NULL ?
                           aggregates[i].output_name : generated;
        if (name == NULL) status = MILENA_ERR_MEMORY;
        else if (milena_table_column_index(&temporary, name) >= 0) {
            status = MILENA_ERR_ARGUMENT;
            table_error(error, status, "Nombre agregado duplicado");
        } else status = build_one_aggregate(&temporary, source, &aggregates[i],
                                            group_of, group_count, name, error);
        free(generated);
    }
    if (status == MILENA_OK) status = table_commit(out, &temporary, error);
    milena_table_destroy(&temporary);
    free(keys); free(slots); free(first_rows); free(group_of);
    return status;
}

MilenaStatus milena_table_summarize(MilenaTable *out,
                                      const MilenaTable *source,
                                      const MilenaAggregateSpec *aggregates,
                                      size_t aggregate_count,
                                      MilenaError *error) {
    return milena_table_group_by(out, source, NULL, 0, aggregates,
                                 aggregate_count, error);
}

MilenaStatus milena_table_group_by_aggregate(MilenaTable *out,
                                             const MilenaTable *source,
                                             const char *key_column,
                                             const char *value_column,
                                             MilenaAggregateOp operation,
                                             MilenaError *error) {
    const char *keys[] = {key_column};
    MilenaAggregateSpec spec = {value_column, operation, NULL};
    return milena_table_group_by(out, source, keys, 1, &spec, 1, error);
}

typedef struct {
    uint64_t hash;
    size_t head_plus_one;
    size_t tail;
} JoinHashSlot;

static bool row_has_null_key(const MilenaTable *table, const size_t *keys,
                             size_t key_count, size_t row) {
    for (size_t i = 0; i < key_count; ++i)
        if (!table->columns[keys[i]].validity[row]) return true;
    return false;
}

static bool join_key_equal(const MilenaTable *left, const size_t *left_keys,
                           size_t left_row, const MilenaTable *right,
                           const size_t *right_keys, size_t right_row,
                           size_t key_count) {
    for (size_t i = 0; i < key_count; ++i)
        if (!cell_equal_cross(&left->columns[left_keys[i]], left_row,
                              &right->columns[right_keys[i]], right_row,
                              false)) return false;
    return true;
}

static size_t join_find_slot(const JoinHashSlot *slots, size_t capacity,
                             uint64_t hash, const MilenaTable *probe_table,
                             const size_t *probe_keys, size_t probe_row,
                             const MilenaTable *right,
                             const size_t *right_keys, size_t key_count) {
    size_t slot = (size_t)hash & (capacity - 1);
    while (slots[slot].head_plus_one != 0) {
        size_t right_row = slots[slot].head_plus_one - 1;
        if (slots[slot].hash == hash &&
            join_key_equal(probe_table, probe_keys, probe_row,
                           right, right_keys, right_row, key_count)) break;
        slot = (slot + 1) & (capacity - 1);
    }
    return slot;
}

static char *unique_right_name(const MilenaTable *table, const char *base,
                               MilenaError *error) {
    if (milena_table_column_index(table, base) < 0) return milena_strdup(base);
    for (size_t suffix = 1; suffix != SIZE_MAX; ++suffix) {
        const char *middle = suffix == 1 ? "_right" : "_right";
        int digits = suffix == 1 ? 0 : snprintf(NULL, 0, "%zu", suffix);
        if (digits < 0) return NULL;
        size_t length = 0;
        if (!milena_size_add(strlen(base), strlen(middle), &length) ||
            !milena_size_add(length, (size_t)digits + 1, &length)) {
            table_error(error, MILENA_ERR_OVERFLOW, "Nombre de join desborda size_t");
            return NULL;
        }
        char *candidate = (char *)malloc(length);
        if (candidate == NULL) return NULL;
        if (suffix == 1) (void)snprintf(candidate, length, "%s_right", base);
        else (void)snprintf(candidate, length, "%s_right%zu", base, suffix);
        if (milena_table_column_index(table, candidate) < 0) return candidate;
        free(candidate);
    }
    return NULL;
}

MilenaStatus milena_table_join(MilenaTable *out, const MilenaTable *left,
                               const MilenaTable *right,
                               const char *const *left_keys,
                               const char *const *right_keys,
                               size_t key_count, MilenaJoinType join_type,
                               MilenaError *error) {
    if (out == NULL || left == NULL || right == NULL || out == left ||
        out == right || left_keys == NULL || right_keys == NULL ||
        key_count == 0 || join_type < MILENA_JOIN_INNER ||
        join_type > MILENA_JOIN_FULL) {
        table_error(error, MILENA_ERR_ARGUMENT, "Join inválido o con alias de salida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(left, error);
    if (status == MILENA_OK) status = milena_table_validate(right, error);
    if (status != MILENA_OK) return status;
    size_t *lk = (size_t *)malloc(key_count * sizeof(size_t));
    size_t *rk = (size_t *)malloc(key_count * sizeof(size_t));
    if (lk == NULL || rk == NULL) status = MILENA_ERR_MEMORY;
    for (size_t i = 0; status == MILENA_OK && i < key_count; ++i) {
        int li = milena_table_column_index(left, left_keys[i]);
        int ri = milena_table_column_index(right, right_keys[i]);
        if (li < 0 || ri < 0) { status = MILENA_ERR_DATA; break; }
        lk[i] = (size_t)li; rk[i] = (size_t)ri;
        const MilenaTableColumn *a = &left->columns[lk[i]];
        const MilenaTableColumn *b = &right->columns[rk[i]];
        if (a->type != b->type ||
            (a->type == MILENA_COLUMN_ARRAY && a->values.dtype != b->values.dtype))
            status = MILENA_ERR_TYPE;
    }
    if (status != MILENA_OK) {
        free(lk); free(rk);
        table_error(error, status, "Claves de join ausentes o incompatibles");
        return status;
    }
    size_t capacity = 0;
    status = hash_capacity(right->row_count, &capacity, error);
    JoinHashSlot *slots = status == MILENA_OK ?
        (JoinHashSlot *)calloc(capacity, sizeof(JoinHashSlot)) : NULL;
    size_t *next = right->row_count == 0 ? NULL :
        (size_t *)malloc(right->row_count * sizeof(size_t));
    bool *matched = right->row_count == 0 ? NULL :
        (bool *)calloc(right->row_count, sizeof(bool));
    if (status == MILENA_OK &&
        (slots == NULL || (right->row_count != 0 &&
                           (next == NULL || matched == NULL))))
        status = MILENA_ERR_MEMORY;
    for (size_t row = 0; row < right->row_count; ++row) next[row] = SIZE_MAX;
    for (size_t row = 0; status == MILENA_OK && row < right->row_count; ++row) {
        if (row_has_null_key(right, rk, key_count, row)) continue;
        uint64_t hash = key_hash(right, rk, key_count, row);
        size_t slot = join_find_slot(slots, capacity, hash, right, rk, row,
                                     right, rk, key_count);
        if (slots[slot].head_plus_one == 0) {
            slots[slot].hash = hash;
            slots[slot].head_plus_one = row + 1;
            slots[slot].tail = row;
        } else {
            next[slots[slot].tail] = row;
            slots[slot].tail = row;
        }
    }
    size_t pair_count = 0;
    for (size_t row = 0; status == MILENA_OK && row < left->row_count; ++row) {
        size_t matches = 0;
        if (!row_has_null_key(left, lk, key_count, row)) {
            uint64_t hash = key_hash(left, lk, key_count, row);
            size_t slot = join_find_slot(slots, capacity, hash, left, lk, row,
                                         right, rk, key_count);
            if (slots[slot].head_plus_one != 0)
                for (size_t r = slots[slot].head_plus_one - 1;
                     r != SIZE_MAX; r = next[r]) ++matches;
        }
        size_t addition = matches != 0 ? matches :
            ((join_type == MILENA_JOIN_LEFT || join_type == MILENA_JOIN_FULL) ? 1 : 0);
        if (!milena_size_add(pair_count, addition, &pair_count))
            status = MILENA_ERR_OVERFLOW;
    }
    if (status == MILENA_OK &&
        (join_type == MILENA_JOIN_RIGHT || join_type == MILENA_JOIN_FULL)) {
        /* Mark matches in a separate pass before counting unmatched rights. */
        for (size_t row = 0; row < left->row_count; ++row) {
            if (row_has_null_key(left, lk, key_count, row)) continue;
            uint64_t hash = key_hash(left, lk, key_count, row);
            size_t slot = join_find_slot(slots, capacity, hash, left, lk, row,
                                         right, rk, key_count);
            if (slots[slot].head_plus_one != 0)
                for (size_t r = slots[slot].head_plus_one - 1;
                     r != SIZE_MAX; r = next[r]) matched[r] = true;
        }
        for (size_t r = 0; r < right->row_count; ++r)
            if (!matched[r] && !milena_size_add(pair_count, 1, &pair_count))
                status = MILENA_ERR_OVERFLOW;
    }
    size_t *left_rows = NULL; size_t *right_rows = NULL;
    if (status == MILENA_OK && pair_count != 0) {
        if (pair_count > SIZE_MAX / sizeof(size_t)) status = MILENA_ERR_OVERFLOW;
        else {
            left_rows = (size_t *)malloc(pair_count * sizeof(size_t));
            right_rows = (size_t *)malloc(pair_count * sizeof(size_t));
            if (left_rows == NULL || right_rows == NULL) status = MILENA_ERR_MEMORY;
        }
    }
    if (matched != NULL) memset(matched, 0, right->row_count * sizeof(bool));
    size_t at = 0;
    for (size_t row = 0; status == MILENA_OK && row < left->row_count; ++row) {
        bool any = false;
        if (!row_has_null_key(left, lk, key_count, row)) {
            uint64_t hash = key_hash(left, lk, key_count, row);
            size_t slot = join_find_slot(slots, capacity, hash, left, lk, row,
                                         right, rk, key_count);
            if (slots[slot].head_plus_one != 0) {
                for (size_t r = slots[slot].head_plus_one - 1;
                     r != SIZE_MAX; r = next[r]) {
                    left_rows[at] = row; right_rows[at] = r; ++at;
                    matched[r] = true; any = true;
                }
            }
        }
        if (!any && (join_type == MILENA_JOIN_LEFT || join_type == MILENA_JOIN_FULL)) {
            left_rows[at] = row; right_rows[at] = SIZE_MAX; ++at;
        }
    }
    if (status == MILENA_OK &&
        (join_type == MILENA_JOIN_RIGHT || join_type == MILENA_JOIN_FULL))
        for (size_t r = 0; r < right->row_count; ++r)
            if (!matched[r]) {
                left_rows[at] = SIZE_MAX; right_rows[at] = r; ++at;
            }
    MilenaTable temporary;
    memset(&temporary, 0, sizeof(temporary));
    milena_table_init(&temporary);
    temporary.row_count = pair_count;
    for (size_t c = 0; status == MILENA_OK && c < left->column_count; ++c)
        status = add_column_rows(&temporary, &left->columns[c],
                                 left->columns[c].name, left_rows,
                                 pair_count, error);
    for (size_t c = 0; status == MILENA_OK && c < right->column_count; ++c) {
        char *name = unique_right_name(&temporary, right->columns[c].name, error);
        if (name == NULL) status = MILENA_ERR_MEMORY;
        else status = add_column_rows(&temporary, &right->columns[c], name,
                                      right_rows, pair_count, error);
        free(name);
    }
    if (status == MILENA_OK) status = table_commit(out, &temporary, error);
    else if (error != NULL && error->code == MILENA_OK)
        table_error(error, status, "No se pudo construir join");
    milena_table_destroy(&temporary);
    free(lk); free(rk); free(slots); free(next); free(matched);
    free(left_rows); free(right_rows);
    return status;
}

MilenaStatus milena_table_unpivot(MilenaTable *out,
                                  const MilenaTable *source,
                                  const char *const *id_columns,
                                  size_t id_count,
                                  const char *const *value_columns,
                                  size_t value_count,
                                  const char *variable_name,
                                  const char *value_name,
                                  MilenaError *error) {
    if (out == NULL || source == NULL || out == source ||
        (id_count != 0 && id_columns == NULL) || value_count == 0 ||
        value_columns == NULL || variable_name == NULL || value_name == NULL) {
        table_error(error, MILENA_ERR_ARGUMENT, "Unpivot inválido");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    size_t *ids = id_count == 0 ? NULL :
        (size_t *)malloc(id_count * sizeof(size_t));
    size_t *vals = (size_t *)malloc(value_count * sizeof(size_t));
    if (status == MILENA_OK &&
        ((id_count != 0 && ids == NULL) || vals == NULL)) status = MILENA_ERR_MEMORY;
    for (size_t i = 0; status == MILENA_OK && i < id_count; ++i) {
        int index = milena_table_column_index(source, id_columns[i]);
        if (index < 0) status = MILENA_ERR_DATA;
        else ids[i] = (size_t)index;
    }
    for (size_t i = 0; status == MILENA_OK && i < value_count; ++i) {
        int index = milena_table_column_index(source, value_columns[i]);
        if (index < 0) status = MILENA_ERR_DATA;
        else vals[i] = (size_t)index;
    }
    if (status == MILENA_OK) {
        const MilenaTableColumn *first = &source->columns[vals[0]];
        for (size_t i = 1; i < value_count; ++i) {
            const MilenaTableColumn *other = &source->columns[vals[i]];
            if (first->type != other->type ||
                (first->type != MILENA_COLUMN_STRING &&
                 first->values.dtype != other->values.dtype) ||
                (first->type == MILENA_COLUMN_CATEGORICAL &&
                 first->dictionary_size != other->dictionary_size)) {
                status = MILENA_ERR_TYPE;
                break;
            }
            if (first->type == MILENA_COLUMN_CATEGORICAL)
                for (size_t d = 0; d < first->dictionary_size; ++d)
                    if (strcmp(first->dictionary[d], other->dictionary[d]) != 0)
                        status = MILENA_ERR_TYPE;
        }
    }
    size_t output_rows = 0;
    if (status == MILENA_OK &&
        !milena_size_mul(source->row_count, value_count, &output_rows))
        status = MILENA_ERR_OVERFLOW;
    size_t *source_rows = output_rows == 0 ? NULL :
        (size_t *)malloc(output_rows * sizeof(size_t));
    if (status == MILENA_OK && output_rows != 0 && source_rows == NULL)
        status = MILENA_ERR_MEMORY;
    for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row)
        for (size_t v = 0; v < value_count; ++v)
            source_rows[row * value_count + v] = row;
    MilenaTable temporary;
    memset(&temporary, 0, sizeof(temporary));
    milena_table_init(&temporary);
    temporary.row_count = output_rows;
    for (size_t i = 0; status == MILENA_OK && i < id_count; ++i)
        status = add_column_rows(&temporary, &source->columns[ids[i]],
                                 source->columns[ids[i]].name,
                                 source_rows, output_rows, error);
    const char **variables = output_rows == 0 ? NULL :
        (const char **)malloc(output_rows * sizeof(char *));
    if (status == MILENA_OK && output_rows != 0 && variables == NULL)
        status = MILENA_ERR_MEMORY;
    for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row)
        for (size_t v = 0; v < value_count; ++v)
            variables[row * value_count + v] = value_columns[v];
    if (status == MILENA_OK)
        status = milena_table_add_string_column_copy(&temporary, variable_name,
                                                      variables, output_rows,
                                                      NULL, error);
    bool *validity = output_rows == 0 ? NULL :
        (bool *)malloc(output_rows * sizeof(bool));
    if (status == MILENA_OK && output_rows != 0 && validity == NULL)
        status = MILENA_ERR_MEMORY;
    const MilenaTableColumn *first = status == MILENA_OK ?
        &source->columns[vals[0]] : NULL;
    if (status == MILENA_OK && first->type == MILENA_COLUMN_STRING) {
        const char **strings = output_rows == 0 ? NULL :
            (const char **)calloc(output_rows, sizeof(char *));
        if (output_rows != 0 && strings == NULL) status = MILENA_ERR_MEMORY;
        for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row)
            for (size_t v = 0; v < value_count; ++v) {
                size_t at = row * value_count + v;
                const MilenaTableColumn *column = &source->columns[vals[v]];
                validity[at] = column->validity[row];
                strings[at] = validity[at] ? column->strings[row] : NULL;
            }
        if (status == MILENA_OK)
            status = milena_table_add_string_column_copy(&temporary, value_name,
                                                          strings, output_rows,
                                                          validity, error);
        free(strings);
    } else if (status == MILENA_OK) {
        size_t shape[] = {output_rows};
        MilenaArray values = {0};
        status = milena_array_zeros(&values, first->values.dtype, 1, shape, error);
        for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row)
            for (size_t v = 0; v < value_count; ++v) {
                size_t at = row * value_count + v;
                const MilenaTableColumn *column = &source->columns[vals[v]];
                validity[at] = column->validity[row];
                memcpy(array_row_mut(&values, at, error),
                       array_row_const(&column->values, row), values.itemsize);
            }
        if (status == MILENA_OK && first->type == MILENA_COLUMN_CATEGORICAL)
            status = milena_table_add_categorical_column_copy(
                &temporary, value_name, &values,
                (const char *const *)first->dictionary,
                first->dictionary_size, validity, error);
        else if (status == MILENA_OK)
            status = milena_table_add_column_copy(&temporary, value_name,
                                                   &values, validity, error);
        milena_array_release(&values);
    }
    if (status == MILENA_OK) status = table_commit(out, &temporary, error);
    else if (error != NULL && error->code == MILENA_OK)
        table_error(error, status, "No se pudo construir unpivot");
    milena_table_destroy(&temporary);
    free(ids); free(vals); free(source_rows); free(variables); free(validity);
    return status;
}


static const MilenaVariable *table_schema_variable(const MilenaSchema *schema,
                                                    const char *name) {
    if (!schema || !name) return NULL;
    int index = schema_index(schema, name);
    return index >= 0 ? &schema->variables[index] : NULL;
}

static MilenaStatus table_dataset_validity(const Dataset *dataset, size_t column,
                                           bool **out, MilenaError *error) {
    bool *validity = NULL;
    if (dataset->row_count > 0) {
        validity = (bool *)calloc(dataset->row_count, sizeof(*validity));
        if (!validity) {
            table_error(error, MILENA_ERR_MEMORY, "Sin memoria para validez del dataset");
            return MILENA_ERR_MEMORY;
        }
        for (size_t row = 0; row < dataset->row_count; row++) {
            const char *value = dataset->rows[row][column];
            validity[row] = value != NULL && value[0] != '\0';
        }
    }
    *out = validity;
    return MILENA_OK;
}

static MilenaStatus table_dataset_numeric(const Dataset *dataset, size_t column,
                                          MilenaArray *array, bool *validity,
                                          MilenaError *error) {
    double *values = NULL;
    if (dataset->row_count > 0) {
        values = (double *)calloc(dataset->row_count, sizeof(*values));
        if (!values) {
            table_error(error, MILENA_ERR_MEMORY, "Sin memoria para columna numérica");
            return MILENA_ERR_MEMORY;
        }
    }
    for (size_t row = 0; row < dataset->row_count; row++) {
        if (!validity[row]) continue;
        if (milena_parse_double(dataset->rows[row][column], &values[row]) != MILENA_OK) {
            validity[row] = false;
        }
    }
    size_t shape[1] = {dataset->row_count};
    MilenaStatus status = milena_array_from_f64(array, 1, shape, values, error);
    free(values);
    return status;
}

static MilenaStatus table_dataset_categorical(const Dataset *dataset, size_t column,
                                              MilenaArray *array,
                                              bool *validity,
                                              char ***dictionary,
                                              size_t *dictionary_size,
                                              MilenaError *error) {
    int64_t *codes = NULL;
    char **items = NULL;
    size_t count = 0, capacity = 0;
    if (dataset->row_count > 0) {
        codes = (int64_t *)calloc(dataset->row_count, sizeof(*codes));
        if (!codes) {
            table_error(error, MILENA_ERR_MEMORY, "Sin memoria para códigos categóricos");
            return MILENA_ERR_MEMORY;
        }
    }
    for (size_t row = 0; row < dataset->row_count; row++) {
        if (!validity[row]) continue;
        const char *value = dataset->rows[row][column];
        size_t index = 0;
        while (index < count && strcmp(items[index], value) != 0) index++;
        if (index == count) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 8;
                char **grown = (char **)realloc(items, next * sizeof(*grown));
                if (!grown) {
                    free(codes);
                    free(items);
                    table_error(error, MILENA_ERR_MEMORY, "Sin memoria para diccionario categórico");
                    return MILENA_ERR_MEMORY;
                }
                items = grown;
                capacity = next;
            }
            items[count] = (char *)value;
            count++;
        }
        codes[row] = (int64_t)index;
    }
    size_t shape[1] = {dataset->row_count};
    MilenaArray wide;
    milena_array_init(&wide);
    MilenaStatus status = milena_array_from_i64(&wide, 1, shape, codes, error);
    free(codes);
    if (status != MILENA_OK) {
        free(items);
        return status;
    }
    status = milena_array_cast(array, &wide, MILENA_DTYPE_UINT32, error);
    milena_array_release(&wide);
    if (status != MILENA_OK) {
        free(items);
        return status;
    }
    *dictionary = items;
    *dictionary_size = count;
    return MILENA_OK;
}

MilenaStatus milena_table_from_dataset(MilenaTable *out,
                                        const Dataset *dataset,
                                        const MilenaSchema *schema,
                                        MilenaError *error) {
    if (!out || !dataset || !schema) {
        table_error(error, MILENA_ERR_ARGUMENT, "Dataset o esquema inválido");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaTable temporary;
    milena_table_init(&temporary);
    MilenaStatus status = MILENA_OK;
    for (size_t column = 0; column < dataset->column_count; column++) {
        const char *name = dataset->headers[column];
        const MilenaVariable *variable = table_schema_variable(schema, name);
        MilenaVariableType type = variable ? variable->type : MILENA_VAR_TEXT;
        bool *validity = NULL;
        status = table_dataset_validity(dataset, column, &validity, error);
        if (status != MILENA_OK) break;

        if (type == MILENA_VAR_NUMERIC) {
            MilenaArray values;
            milena_array_init(&values);
            status = table_dataset_numeric(dataset, column, &values, validity, error);
            if (status == MILENA_OK) {
                status = milena_table_add_column_copy(&temporary, name, &values,
                                                      validity, error);
            }
            milena_array_release(&values);
        } else if (type == MILENA_VAR_CATEGORICAL || type == MILENA_VAR_BINARY) {
            MilenaArray codes;
            char **dictionary = NULL;
            size_t dictionary_size = 0;
            milena_array_init(&codes);
            status = table_dataset_categorical(dataset, column, &codes, validity,
                                               &dictionary, &dictionary_size, error);
            if (status == MILENA_OK) {
                const char *const *labels = (const char *const *)dictionary;
                status = milena_table_add_categorical_column_copy(
                    &temporary, name, &codes, labels, dictionary_size,
                    validity, error);
            }
            milena_array_release(&codes);
            free(dictionary);
        } else {
            const char *const *values = (const char *const *)dataset->rows[0];
            if (dataset->row_count == 0) values = NULL;
            else {
                /* rows are column-major only through this temporary view. */
                const char **column_values = (const char **)calloc(
                    dataset->row_count, sizeof(*column_values));
                if (!column_values) {
                    free(validity);
                    table_error(error, MILENA_ERR_MEMORY, "Sin memoria para columna textual");
                    status = MILENA_ERR_MEMORY;
                    break;
                }
                for (size_t row = 0; row < dataset->row_count; row++)
                    column_values[row] = dataset->rows[row][column];
                status = milena_table_add_string_column_copy(
                    &temporary, name, column_values, dataset->row_count,
                    validity, error);
                free(column_values);
            }
        }
        free(validity);
        if (status != MILENA_OK) break;
        if (variable) {
            status = milena_table_column_set_metadata(
                &temporary, name, "rol", schema_role_name(variable->role), error);
            if (status != MILENA_OK) break;
        }
    }
    if (status == MILENA_OK) status = milena_table_validate(&temporary, error);
    if (status != MILENA_OK) {
        milena_table_destroy(&temporary);
        return status;
    }
    milena_table_destroy(out);
    *out = temporary;
    memset(&temporary, 0, sizeof(temporary));
    return MILENA_OK;
}


static MilenaStatus table_dataset_cell_text(const MilenaTable *table,
                                            size_t column, size_t row,
                                            char *buffer, size_t buffer_size,
                                            const char **text,
                                            MilenaError *error) {
    if (milena_table_is_null(table, column, row)) {
        *text = "";
        return MILENA_OK;
    }
    const MilenaTableColumn *column_data = milena_table_column(table, column);
    if (!column_data) {
        table_error(error, MILENA_ERR_ARGUMENT, "Columna de tabla inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (column_data->type == MILENA_COLUMN_STRING) {
        return milena_table_get_string(table, column, row, text, error);
    }
    if (column_data->type == MILENA_COLUMN_CATEGORICAL) {
        const char *label = NULL;
        uint32_t code = 0;
        MilenaStatus status = milena_table_get_category(table, column, row, &code,
                                                        &label, error);
        if (status == MILENA_OK) *text = label ? label : "";
        return status;
    }
    const void *value = NULL;
    MilenaStatus status = milena_table_get_array_value(table, column, row,
                                                       &value, error);
    if (status != MILENA_OK) return status;
    int written = 0;
    switch (column_data->values.dtype) {
        case MILENA_DTYPE_BOOL:
            written = snprintf(buffer, buffer_size, "%s",
                               *(const bool *)value ? "verdadero" : "falso");
            break;
        case MILENA_DTYPE_INT8: written = snprintf(buffer, buffer_size, "%d", (int)*(const int8_t *)value); break;
        case MILENA_DTYPE_INT16: written = snprintf(buffer, buffer_size, "%d", (int)*(const int16_t *)value); break;
        case MILENA_DTYPE_INT32: written = snprintf(buffer, buffer_size, "%d", *(const int32_t *)value); break;
        case MILENA_DTYPE_INT64: written = snprintf(buffer, buffer_size, "%lld", (long long)*(const int64_t *)value); break;
        case MILENA_DTYPE_UINT8: written = snprintf(buffer, buffer_size, "%u", (unsigned)*(const uint8_t *)value); break;
        case MILENA_DTYPE_UINT16: written = snprintf(buffer, buffer_size, "%u", (unsigned)*(const uint16_t *)value); break;
        case MILENA_DTYPE_UINT32: written = snprintf(buffer, buffer_size, "%u", *(const uint32_t *)value); break;
        case MILENA_DTYPE_UINT64: written = snprintf(buffer, buffer_size, "%llu", (unsigned long long)*(const uint64_t *)value); break;
        case MILENA_DTYPE_FLOAT32: written = snprintf(buffer, buffer_size, "%.9g", (double)*(const float *)value); break;
        case MILENA_DTYPE_FLOAT64: written = snprintf(buffer, buffer_size, "%.17g", *(const double *)value); break;
        default:
            table_error(error, MILENA_ERR_UNSUPPORTED, "Tipo numérico no convertible a dataset");
            return MILENA_ERR_UNSUPPORTED;
    }
    if (written < 0 || (size_t)written >= buffer_size) {
        table_error(error, MILENA_ERR_OVERFLOW, "Valor de tabla demasiado largo");
        return MILENA_ERR_OVERFLOW;
    }
    *text = buffer;
    return MILENA_OK;
}

MilenaStatus milena_dataset_from_table(Dataset *out,
                                        const MilenaTable *table,
                                        MilenaError *error) {
    if (!out || !table) {
        table_error(error, MILENA_ERR_ARGUMENT, "Tabla de conversión inválida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;
    dataset_init(out);
    out->column_count = table->column_count;
    out->row_count = table->row_count;
    out->row_capacity = table->row_count;
    if (out->column_count > 0) {
        out->headers = (char **)calloc(out->column_count, sizeof(*out->headers));
        if (!out->headers) status = MILENA_ERR_MEMORY;
    }
    if (status == MILENA_OK && out->row_count > 0) {
        out->rows = (char ***)calloc(out->row_count, sizeof(*out->rows));
        if (!out->rows) status = MILENA_ERR_MEMORY;
    }
    for (size_t column = 0; status == MILENA_OK && column < out->column_count; column++) {
        const MilenaTableColumn *column_data = milena_table_column(table, column);
        out->headers[column] = milena_strdup(column_data->name);
        if (!out->headers[column]) status = MILENA_ERR_MEMORY;
    }
    for (size_t row = 0; status == MILENA_OK && row < out->row_count; row++) {
        out->rows[row] = (char **)calloc(out->column_count, sizeof(**out->rows));
        if (!out->rows[row]) {
            status = MILENA_ERR_MEMORY;
            break;
        }
        for (size_t column = 0; column < out->column_count; column++) {
            char buffer[128];
            const char *text = NULL;
            status = table_dataset_cell_text(table, column, row, buffer,
                                             sizeof(buffer), &text, error);
            if (status != MILENA_OK) break;
            out->rows[row][column] = milena_strdup(text ? text : "");
            if (!out->rows[row][column]) {
                status = MILENA_ERR_MEMORY;
                break;
            }
        }
    }
    if (status != MILENA_OK) {
        dataset_destroy(out);
        table_error(error, status, "No se pudo reconstruir el dataset desde la tabla");
        return status;
    }
    return MILENA_OK;
}


static MilenaStatus table_rows_equal(const MilenaTable *table,
                                     size_t left, size_t right,
                                     bool *equal, MilenaError *error) {
    if (!table || !equal || left >= table->row_count || right >= table->row_count) {
        table_error(error, MILENA_ERR_ARGUMENT, "Fila inválida para duplicados");
        return MILENA_ERR_ARGUMENT;
    }
    *equal = true;
    for (size_t column = 0; column < table->column_count; column++) {
        bool left_null = milena_table_is_null(table, column, left);
        bool right_null = milena_table_is_null(table, column, right);
        if (left_null != right_null) {
            *equal = false;
            return MILENA_OK;
        }
        if (left_null) continue;
        const MilenaTableColumn *column_data = milena_table_column(table, column);
        if (!column_data) {
            table_error(error, MILENA_ERR_DATA, "Columna inválida al comparar duplicados");
            return MILENA_ERR_DATA;
        }
        if (column_data->type == MILENA_COLUMN_STRING) {
            const char *left_text = NULL, *right_text = NULL;
            MilenaStatus status = milena_table_get_string(table, column, left,
                                                          &left_text, error);
            if (status != MILENA_OK) return status;
            status = milena_table_get_string(table, column, right,
                                             &right_text, error);
            if (status != MILENA_OK) return status;
            if (strcmp(left_text ? left_text : "", right_text ? right_text : "") != 0) {
                *equal = false;
                return MILENA_OK;
            }
        } else if (column_data->type == MILENA_COLUMN_CATEGORICAL) {
            uint32_t left_code = 0, right_code = 0;
            const char *left_label = NULL, *right_label = NULL;
            MilenaStatus status = milena_table_get_category(table, column, left,
                                                             &left_code, &left_label, error);
            if (status != MILENA_OK) return status;
            status = milena_table_get_category(table, column, right,
                                               &right_code, &right_label, error);
            if (status != MILENA_OK) return status;
            if (left_code != right_code) {
                *equal = false;
                return MILENA_OK;
            }
        } else {
            const void *left_value = NULL, *right_value = NULL;
            MilenaStatus status = milena_table_get_array_value(table, column, left,
                                                               &left_value, error);
            if (status != MILENA_OK) return status;
            status = milena_table_get_array_value(table, column, right,
                                                  &right_value, error);
            if (status != MILENA_OK) return status;
            if (memcmp(left_value, right_value, column_data->values.itemsize) != 0) {
                *equal = false;
                return MILENA_OK;
            }
        }
    }
    return MILENA_OK;
}

MilenaStatus milena_table_drop_duplicates(MilenaTable *out,
                                         const MilenaTable *source,
                                         MilenaError *error) {
    if (!out || !source) {
        table_error(error, MILENA_ERR_ARGUMENT, "Tabla inválida para eliminar duplicados");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    size_t shape[1] = {source->row_count};
    MilenaArray mask;
    milena_array_init(&mask);
    status = milena_array_zeros(&mask, MILENA_DTYPE_BOOL, 1, shape, error);
    if (status != MILENA_OK) return status;
    bool *keep = (bool *)milena_array_data(&mask);
    if (source->row_count > 0 && !keep) {
        milena_array_release(&mask);
        table_error(error, MILENA_ERR_DATA, "Máscara de duplicados inválida");
        return MILENA_ERR_DATA;
    }
    size_t kept_count = 0;
    for (size_t row = 0; row < source->row_count; row++) {
        bool duplicate = false;
        for (size_t previous = 0; previous < row; previous++) {
            if (!keep[previous]) continue;
            status = table_rows_equal(source, row, previous, &duplicate, error);
            if (status != MILENA_OK) break;
            if (duplicate) break;
        }
        if (status != MILENA_OK) break;
        keep[row] = !duplicate;
        if (!duplicate) kept_count++;
    }
    if (status == MILENA_OK) {
        status = milena_table_filter(out, source, &mask, error);
    }
    milena_array_release(&mask);
    (void)kept_count;
    return status;
}


static MilenaStatus table_numeric_cell(const MilenaTable *table, size_t column,
                                       size_t row, double *value,
                                       MilenaError *error) {
    const MilenaTableColumn *data = milena_table_column(table, column);
    if (!data || data->type != MILENA_COLUMN_ARRAY) {
        table_error(error, MILENA_ERR_TYPE, "La transformación requiere columnas numéricas");
        return MILENA_ERR_TYPE;
    }
    const void *raw = NULL;
    MilenaStatus status = milena_table_get_array_value(table, column, row, &raw, error);
    if (status != MILENA_OK) return status;
    switch (data->values.dtype) {
        case MILENA_DTYPE_BOOL: *value = *(const bool *)raw ? 1.0 : 0.0; break;
        case MILENA_DTYPE_INT8: *value = (double)*(const int8_t *)raw; break;
        case MILENA_DTYPE_INT16: *value = (double)*(const int16_t *)raw; break;
        case MILENA_DTYPE_INT32: *value = (double)*(const int32_t *)raw; break;
        case MILENA_DTYPE_INT64: *value = (double)*(const int64_t *)raw; break;
        case MILENA_DTYPE_UINT8: *value = (double)*(const uint8_t *)raw; break;
        case MILENA_DTYPE_UINT16: *value = (double)*(const uint16_t *)raw; break;
        case MILENA_DTYPE_UINT32: *value = (double)*(const uint32_t *)raw; break;
        case MILENA_DTYPE_UINT64: *value = (double)*(const uint64_t *)raw; break;
        case MILENA_DTYPE_FLOAT32: *value = (double)*(const float *)raw; break;
        case MILENA_DTYPE_FLOAT64: *value = *(const double *)raw; break;
        default:
            table_error(error, MILENA_ERR_TYPE, "Tipo numérico no soportado en producto");
            return MILENA_ERR_TYPE;
    }
    return isfinite(*value) ? MILENA_OK : MILENA_ERR_DATA;
}

MilenaStatus milena_table_add_product(MilenaTable *table,
                                     const char *left_column,
                                     const char *right_column,
                                     const char *output_column,
                                     MilenaError *error) {
    if (!table || !left_column || !right_column || !output_column) {
        table_error(error, MILENA_ERR_ARGUMENT, "Columnas inválidas para producto");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;
    int left = milena_table_column_index(table, left_column);
    int right = milena_table_column_index(table, right_column);
    if (left < 0 || right < 0) {
        table_error(error, MILENA_ERR_DATA, "Columna de producto inexistente");
        return MILENA_ERR_DATA;
    }
    if (milena_table_column_index(table, output_column) >= 0) {
        table_error(error, MILENA_ERR_DATA, "La columna de salida ya existe");
        return MILENA_ERR_DATA;
    }
    double *values = NULL;
    bool *validity = NULL;
    if (table->row_count > 0) {
        values = (double *)calloc(table->row_count, sizeof(*values));
        validity = (bool *)calloc(table->row_count, sizeof(*validity));
        if (!values || !validity) {
            free(values); free(validity);
            table_error(error, MILENA_ERR_MEMORY, "Sin memoria para producto de tabla");
            return MILENA_ERR_MEMORY;
        }
    }
    for (size_t row = 0; row < table->row_count; row++) {
        if (milena_table_is_null(table, (size_t)left, row) ||
            milena_table_is_null(table, (size_t)right, row)) continue;
        double left_value = 0.0, right_value = 0.0;
        status = table_numeric_cell(table, (size_t)left, row, &left_value, error);
        if (status == MILENA_OK)
            status = table_numeric_cell(table, (size_t)right, row, &right_value, error);
        if (status != MILENA_OK) break;
        values[row] = left_value * right_value;
        if (!isfinite(values[row])) {
            table_error(error, MILENA_ERR_OVERFLOW, "Producto fuera de rango");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        validity[row] = true;
    }
    if (status == MILENA_OK) {
        MilenaArray result;
        milena_array_init(&result);
        size_t shape[1] = {table->row_count};
        status = milena_array_from_f64(&result, 1, shape, values, error);
        if (status == MILENA_OK) {
            status = milena_table_add_column_copy(table, output_column, &result,
                                                  validity, error);
        }
        milena_array_release(&result);
    }
    free(values);
    free(validity);
    return status;
}


MilenaStatus milena_table_add_month(MilenaTable *table,
                                   const char *date_column,
                                   const char *output_column,
                                   MilenaError *error) {
    if (!table || !date_column || !output_column) {
        table_error(error, MILENA_ERR_ARGUMENT, "Columnas inválidas para periodo");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;
    int date_index = milena_table_column_index(table, date_column);
    if (date_index < 0) {
        table_error(error, MILENA_ERR_DATA, "Columna de fecha inexistente");
        return MILENA_ERR_DATA;
    }
    const MilenaTableColumn *date_data = milena_table_column(table, (size_t)date_index);
    if (!date_data || date_data->type != MILENA_COLUMN_STRING) {
        table_error(error, MILENA_ERR_TYPE, "La extracción de periodo requiere texto de fecha");
        return MILENA_ERR_TYPE;
    }
    if (milena_table_column_index(table, output_column) >= 0) {
        table_error(error, MILENA_ERR_DATA, "La columna de periodo ya existe");
        return MILENA_ERR_DATA;
    }
    char **values = NULL;
    bool *validity = NULL;
    if (table->row_count > 0) {
        values = (char **)calloc(table->row_count, sizeof(*values));
        validity = (bool *)calloc(table->row_count, sizeof(*validity));
        if (!values || !validity) {
            free(values); free(validity);
            table_error(error, MILENA_ERR_MEMORY, "Sin memoria para periodo");
            return MILENA_ERR_MEMORY;
        }
    }
    for (size_t row = 0; row < table->row_count; row++) {
        if (milena_table_is_null(table, (size_t)date_index, row)) continue;
        const char *date = NULL;
        status = milena_table_get_string(table, (size_t)date_index, row, &date, error);
        if (status != MILENA_OK) break;
        int year = 0, month = 0, day = 0;
        if (!date || sscanf(date, "%d-%d-%d", &year, &month, &day) != 3 ||
            year < 1 || month < 1 || month > 12 || day < 1 || day > 31) {
            table_error(error, MILENA_ERR_TYPE, "Fecha inválida para extraer periodo");
            status = MILENA_ERR_TYPE;
            break;
        }
        char buffer[32];
        int written = snprintf(buffer, sizeof(buffer), "%04d-%02d", year, month);
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            table_error(error, MILENA_ERR_OVERFLOW, "Periodo demasiado largo");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        values[row] = milena_strdup(buffer);
        if (!values[row]) {
            status = MILENA_ERR_MEMORY;
            break;
        }
        validity[row] = true;
    }
    if (status == MILENA_OK) {
        const char *const *column_values = (const char *const *)values;
        status = milena_table_add_string_column_copy(table, output_column,
                                                     column_values, table->row_count,
                                                     validity, error);
    }
    for (size_t row = 0; row < table->row_count; row++) free(values ? values[row] : NULL);
    free(values);
    free(validity);
    return status;
}


MilenaStatus milena_table_filter_numeric(MilenaTable *out,
                                         const MilenaTable *source,
                                         const char *column_name,
                                         const char *operator_text,
                                         double threshold,
                                         MilenaError *error) {
    if (!out || !source || !column_name || !operator_text) {
        table_error(error, MILENA_ERR_ARGUMENT, "Condición de filtro inválida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    int column = milena_table_column_index(source, column_name);
    if (column < 0) {
        table_error(error, MILENA_ERR_DATA, "Columna inexistente en condición");
        return MILENA_ERR_DATA;
    }
    if (strcmp(operator_text, ">") != 0 && strcmp(operator_text, ">=") != 0 &&
        strcmp(operator_text, "<") != 0 && strcmp(operator_text, "<=") != 0 &&
        strcmp(operator_text, "==") != 0 && strcmp(operator_text, "!=") != 0) {
        table_error(error, MILENA_ERR_UNSUPPORTED, "Operador de filtro no soportado");
        return MILENA_ERR_UNSUPPORTED;
    }
    size_t shape[1] = {source->row_count};
    MilenaArray mask;
    milena_array_init(&mask);
    status = milena_array_zeros(&mask, MILENA_DTYPE_BOOL, 1, shape, error);
    if (status != MILENA_OK) return status;
    bool *keep = (bool *)milena_array_data(&mask);
    for (size_t row = 0; status == MILENA_OK && row < source->row_count; row++) {
        if (milena_table_is_null(source, (size_t)column, row)) continue;
        double value = 0.0;
        status = table_numeric_cell(source, (size_t)column, row, &value, error);
        if (status != MILENA_OK) break;
        if (strcmp(operator_text, ">") == 0) keep[row] = value > threshold;
        else if (strcmp(operator_text, ">=") == 0) keep[row] = value >= threshold;
        else if (strcmp(operator_text, "<") == 0) keep[row] = value < threshold;
        else if (strcmp(operator_text, "<=") == 0) keep[row] = value <= threshold;
        else if (strcmp(operator_text, "==") == 0) keep[row] = value == threshold;
        else keep[row] = value != threshold;
    }
    if (status == MILENA_OK) status = milena_table_filter(out, source, &mask, error);
    milena_array_release(&mask);
    return status;
}


static int compare_table_doubles(const void *left, const void *right) {
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

MilenaStatus milena_table_add_statistic(MilenaTable *out,
                                        const MilenaTable *source,
                                        const char *value_column,
                                        const char *output_column,
                                        MilenaTableStatistic statistic,
                                        MilenaError *error) {
    if (!out || !source || !value_column || !output_column || out == source ||
        statistic < MILENA_STAT_VARIANCE || statistic > MILENA_STAT_MEDIAN) {
        table_error(error, MILENA_ERR_ARGUMENT, "Estadística de tabla inválida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status != MILENA_OK) return status;
    status = milena_table_validate(out, error);
    if (status != MILENA_OK) return status;
    if (out->row_count != 1) {
        table_error(error, MILENA_ERR_ARGUMENT,
                    "La estadística global requiere una tabla de una fila");
        return MILENA_ERR_ARGUMENT;
    }
    int column = milena_table_column_index(source, value_column);
    if (column < 0 || milena_table_column(source, (size_t)column)->type != MILENA_COLUMN_ARRAY) {
        table_error(error, MILENA_ERR_TYPE, "La estadística requiere una columna numérica");
        return MILENA_ERR_TYPE;
    }
    double *values = source->row_count == 0 ? NULL :
        (double *)malloc(source->row_count * sizeof(*values));
    if (source->row_count != 0 && !values) {
        table_error(error, MILENA_ERR_MEMORY, "Sin memoria para estadística");
        return MILENA_ERR_MEMORY;
    }
    size_t count = 0;
    double mean = 0.0, m2 = 0.0;
    for (size_t row = 0; row < source->row_count; row++) {
        if (milena_table_is_null(source, (size_t)column, row)) continue;
        double value = 0.0;
        status = table_numeric_cell(source, (size_t)column, row, &value, error);
        if (status != MILENA_OK) break;
        values[count++] = value;
        double delta = value - mean;
        mean += delta / (double)count;
        m2 += delta * (value - mean);
    }
    double result = 0.0;
    if (status == MILENA_OK && count > 0) {
        if (statistic == MILENA_STAT_VARIANCE) result = m2 / (double)count;
        else if (statistic == MILENA_STAT_STDDEV) result = sqrt(m2 / (double)count);
        else {
            qsort(values, count, sizeof(*values), compare_table_doubles);
            size_t middle = count / 2;
            result = count % 2 ? values[middle] :
                (values[middle - 1] + values[middle]) / 2.0;
        }
    }
    bool valid = status == MILENA_OK && count > 0;
    MilenaArray array;
    milena_array_init(&array);
    size_t shape[1] = {1};
    if (status == MILENA_OK) {
        status = milena_array_from_f64(&array, 1, shape, &result, error);
        if (status == MILENA_OK) status = milena_table_add_column_copy(
            out, output_column, &array, &valid, error);
    }
    milena_array_release(&array);
    free(values);
    return status;
}


MilenaStatus milena_table_add_percentile(MilenaTable *out,
                                         const MilenaTable *source,
                                         const char *value_column,
                                         const char *output_column,
                                         double percentile,
                                         MilenaError *error) {
    if (percentile < 0.0 || percentile > 100.0 || !isfinite(percentile)) {
        table_error(error, MILENA_ERR_ARGUMENT, "Percentil fuera del rango 0..100");
        return MILENA_ERR_ARGUMENT;
    }
    if (!out || !source || out == source || !value_column || !output_column) {
        table_error(error, MILENA_ERR_ARGUMENT, "Percentil de tabla inválido");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(source, error);
    if (status == MILENA_OK) status = milena_table_validate(out, error);
    if (status != MILENA_OK) return status;
    if (out->row_count != 1) {
        table_error(error, MILENA_ERR_ARGUMENT,
                    "El percentil global requiere una tabla de una fila");
        return MILENA_ERR_ARGUMENT;
    }
    int column = milena_table_column_index(source, value_column);
    const MilenaTableColumn *data = column < 0 ? NULL :
        milena_table_column(source, (size_t)column);
    if (!data || data->type != MILENA_COLUMN_ARRAY) {
        table_error(error, MILENA_ERR_TYPE, "El percentil requiere una columna numérica");
        return MILENA_ERR_TYPE;
    }
    double *values = source->row_count == 0 ? NULL :
        (double *)malloc(source->row_count * sizeof(*values));
    if (source->row_count != 0 && !values) {
        table_error(error, MILENA_ERR_MEMORY, "Sin memoria para percentil");
        return MILENA_ERR_MEMORY;
    }
    size_t count = 0;
    for (size_t row = 0; row < source->row_count; row++) {
        if (milena_table_is_null(source, (size_t)column, row)) continue;
        double value = 0.0;
        status = table_numeric_cell(source, (size_t)column, row, &value, error);
        if (status != MILENA_OK) break;
        values[count++] = value;
    }
    double result = 0.0;
    if (status == MILENA_OK && count > 0) {
        qsort(values, count, sizeof(*values), compare_table_doubles);
        double position = percentile * (double)(count - 1) / 100.0;
        size_t lower = (size_t)position;
        size_t upper = lower < count - 1 ? lower + 1 : lower;
        double fraction = position - (double)lower;
        result = values[lower] + fraction * (values[upper] - values[lower]);
    }
    bool valid = status == MILENA_OK && count > 0;
    MilenaArray array;
    milena_array_init(&array);
    size_t shape[1] = {1};
    if (status == MILENA_OK) {
        status = milena_array_from_f64(&array, 1, shape, &result, error);
        if (status == MILENA_OK) status = milena_table_add_column_copy(
            out, output_column, &array, &valid, error);
    }
    milena_array_release(&array);
    free(values);
    return status;
}
