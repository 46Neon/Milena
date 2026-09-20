#ifndef MILENA_COMMON_H
#define MILENA_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <float.h>
#include <math.h>

#define MILENA_VERSION "0.2.0-identity"
#define MILENA_ERROR_TEXT 512

typedef enum {
    MILENA_OK = 0,
    MILENA_ERR_ARGUMENT,
    MILENA_ERR_MEMORY,
    MILENA_ERR_IO,
    MILENA_ERR_PARSE,
    MILENA_ERR_DATA,
    MILENA_ERR_TYPE,
    MILENA_ERR_OVERFLOW,
    MILENA_ERR_UNSUPPORTED,
    MILENA_ERR_INTERNAL
} MilenaStatus;

typedef struct {
    MilenaStatus code;
    size_t line;
    size_t column;
    size_t row;
    char message[MILENA_ERROR_TEXT];
} MilenaError;

void milena_error_clear(MilenaError *error);
void milena_error_set(MilenaError *error, MilenaStatus code, size_t line,
                    size_t column, size_t row, const char *message);
const char *milena_status_name(MilenaStatus status);
char *milena_strdup(const char *text);
bool milena_size_add(size_t a, size_t b, size_t *out);
bool milena_size_mul(size_t a, size_t b, size_t *out);
MilenaStatus milena_parse_double(const char *text, double *value);
void milena_error_print(const MilenaError *error, FILE *stream);

#endif
