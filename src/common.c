#include "common.h"

void milena_error_clear(MilenaError *error) {
    if (!error) return;
    error->code = MILENA_OK;
    error->line = error->column = error->row = 0;
    error->message[0] = '\0';
}

void milena_error_set(MilenaError *error, MilenaStatus code, size_t line,
                    size_t column, size_t row, const char *message) {
    if (!error) return;
    error->code = code;
    error->line = line;
    error->column = column;
    error->row = row;
    if (!message) message = "Error desconocido";
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

const char *milena_status_name(MilenaStatus status) {
    switch (status) {
        case MILENA_OK: return "OK";
        case MILENA_ERR_ARGUMENT: return "ARGUMENT";
        case MILENA_ERR_MEMORY: return "MEMORY";
        case MILENA_ERR_IO: return "IO";
        case MILENA_ERR_PARSE: return "PARSE";
        case MILENA_ERR_DATA: return "DATA";
        case MILENA_ERR_TYPE: return "TYPE";
        case MILENA_ERR_OVERFLOW: return "OVERFLOW";
        case MILENA_ERR_UNSUPPORTED: return "UNSUPPORTED";
        case MILENA_ERR_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

char *milena_strdup(const char *text) {
    if (!text) return NULL;
    size_t n = strlen(text);
    if (n == SIZE_MAX) return NULL;
    char *copy = (char *)malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, text, n + 1);
    return copy;
}

bool milena_size_add(size_t a, size_t b, size_t *out) {
    if (!out || b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

bool milena_size_mul(size_t a, size_t b, size_t *out) {
    if (!out || (a != 0 && b > SIZE_MAX / a)) return false;
    *out = a * b;
    return true;
}

MilenaStatus milena_parse_double(const char *text, double *value) {
    if (!text || !value) return MILENA_ERR_ARGUMENT;
    while (*text == ' ' || *text == '\t') text++;
    if (*text == '\0') return MILENA_ERR_TYPE;
    errno = 0;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(parsed)) return MILENA_ERR_TYPE;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return MILENA_ERR_TYPE;
    *value = parsed;
    return MILENA_OK;
}

void milena_error_print(const MilenaError *error, FILE *stream) {
    if (!error || error->code == MILENA_OK) return;
    if (!stream) stream = stderr;
    fprintf(stream, "Milena [%s]", milena_status_name(error->code));
    if (error->line) fprintf(stream, " línea %zu", error->line);
    if (error->row) fprintf(stream, " fila %zu", error->row);
    if (error->column) fprintf(stream, " columna %zu", error->column);
    fprintf(stream, ": %s\n", error->message);
}
