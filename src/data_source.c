#include "data_source.h"
#include <string.h>
static int prefix(const char *s, const char *p) { return s && strncmp(s, p, strlen(p)) == 0; }
const char *milena_data_source_kind_name(MilenaDataSourceKind k) { switch (k) { case MILENA_DATA_SOURCE_LOCAL_FILE:return "local"; case MILENA_DATA_SOURCE_S3:return "s3"; case MILENA_DATA_SOURCE_GS:return "gs"; case MILENA_DATA_SOURCE_AZ:return "az"; case MILENA_DATA_SOURCE_HDFS:return "hdfs"; default:return "desconocido"; } }
MilenaStatus milena_data_source_parse(const char *uri, MilenaDataSource *source, MilenaError *e) {
 if (!uri || !*uri || !source) { milena_error_set(e, MILENA_ERR_ARGUMENT,0,0,0,"URI de fuente de datos vacía"); return MILENA_ERR_ARGUMENT; }
 source->uri=uri; if(prefix(uri,"s3://")) source->kind=MILENA_DATA_SOURCE_S3; else if(prefix(uri,"gs://")) source->kind=MILENA_DATA_SOURCE_GS; else if(prefix(uri,"az://")) source->kind=MILENA_DATA_SOURCE_AZ; else if(prefix(uri,"hdfs://")) source->kind=MILENA_DATA_SOURCE_HDFS; else source->kind=MILENA_DATA_SOURCE_LOCAL_FILE; return MILENA_OK;
}
MilenaStatus milena_data_source_require_local(const MilenaDataSource *source,const char **path,MilenaError *e) { if(!source||!path||source->kind!=MILENA_DATA_SOURCE_LOCAL_FILE){milena_error_set(e,MILENA_ERR_UNSUPPORTED,0,0,0,"Fuente remota clasificada pero aún no soportada; solo se admite archivo local");return MILENA_ERR_UNSUPPORTED;} *path=source->uri; return MILENA_OK; }
