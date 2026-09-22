#include "data_source.h"
#include <string.h>

static int prefix(const char *s, const char *p) { return s && strncmp(s, p, strlen(p)) == 0; }
static int has_scheme(const char *s) { return s && strstr(s, "://") != NULL; }

const char *milena_data_source_kind_name(MilenaDataSourceKind k) {
    switch (k) {
    case MILENA_DATA_SOURCE_LOCAL_FILE: return "local";
    case MILENA_DATA_SOURCE_S3: return "s3";
    case MILENA_DATA_SOURCE_GS: return "gs";
    case MILENA_DATA_SOURCE_AZ: return "az";
    case MILENA_DATA_SOURCE_HDFS: return "hdfs";
    default: return "desconocido";
    }
}

MilenaStatus milena_data_source_parse(const char *uri, MilenaDataSource *source, MilenaError *e) {
    if (!uri || !*uri || !source) {
        milena_error_set(e, MILENA_ERR_ARGUMENT, 0, 0, 0, "URI de fuente de datos vacía");
        return MILENA_ERR_ARGUMENT;
    }
    source->uri = uri;
    if (prefix(uri, "s3://")) source->kind = MILENA_DATA_SOURCE_S3;
    else if (prefix(uri, "gs://")) source->kind = MILENA_DATA_SOURCE_GS;
    else if (prefix(uri, "az://")) source->kind = MILENA_DATA_SOURCE_AZ;
    else if (prefix(uri, "hdfs://")) source->kind = MILENA_DATA_SOURCE_HDFS;
    else if (has_scheme(uri)) source->kind = MILENA_DATA_SOURCE_UNKNOWN;
    else source->kind = MILENA_DATA_SOURCE_LOCAL_FILE;
    return MILENA_OK;
}

MilenaStatus milena_data_source_require_local(const MilenaDataSource *source,
                                               const char **path, MilenaError *e) {
    if (!source || !path) {
        milena_error_set(e, MILENA_ERR_ARGUMENT, 0, 0, 0, "Fuente de datos inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (source->kind != MILENA_DATA_SOURCE_LOCAL_FILE) {
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
            "Fuente '%s' (%s) no ejecutable: el backend canónico solo admite rutas de archivo local; no se realiza acceso remoto",
            source->uri ? source->uri : "(nula)", milena_data_source_kind_name(source->kind));
        milena_error_set(e, MILENA_ERR_UNSUPPORTED, 0, 0, 0, message);
        return MILENA_ERR_UNSUPPORTED;
    }
    *path = source->uri;
    return MILENA_OK;
}
