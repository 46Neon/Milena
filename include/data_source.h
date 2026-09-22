#ifndef MILENA_DATA_SOURCE_H
#define MILENA_DATA_SOURCE_H
#include "common.h"
typedef enum { MILENA_DATA_SOURCE_LOCAL_FILE = 0, MILENA_DATA_SOURCE_S3, MILENA_DATA_SOURCE_GS, MILENA_DATA_SOURCE_AZ, MILENA_DATA_SOURCE_HDFS, MILENA_DATA_SOURCE_UNKNOWN } MilenaDataSourceKind;
typedef struct { MilenaDataSourceKind kind; const char *uri; } MilenaDataSource;
MilenaStatus milena_data_source_parse(const char *uri, MilenaDataSource *source, MilenaError *error);
MilenaStatus milena_data_source_require_local(const MilenaDataSource *source, const char **path, MilenaError *error);
const char *milena_data_source_kind_name(MilenaDataSourceKind kind);
#endif
