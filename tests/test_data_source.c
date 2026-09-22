#include "data_source.h"
#include <assert.h>
#include <string.h>
int main(void) { const char *v[]={"datos.csv","s3://bucket/a.csv","gs://bucket/a.csv","az://container/a.csv","hdfs://host/a.csv"}; MilenaDataSource s; MilenaError e; const char *p; assert(milena_data_source_parse(v[0],&s,&e)==MILENA_OK&&s.kind==MILENA_DATA_SOURCE_LOCAL_FILE); assert(milena_data_source_require_local(&s,&p,&e)==MILENA_OK&&strcmp(p,v[0])==0); for(int i=1;i<5;i++){assert(milena_data_source_parse(v[i],&s,&e)==MILENA_OK);assert(s.kind!=MILENA_DATA_SOURCE_LOCAL_FILE);assert(milena_data_source_require_local(&s,&p,&e)==MILENA_ERR_UNSUPPORTED);} assert(milena_data_source_parse("",&s,&e)==MILENA_ERR_ARGUMENT); return 0; }
