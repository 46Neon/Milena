#include "data_source.h"
#include <assert.h>
#include <string.h>
int main(void) {
    const char *local="datos.csv"; const char *remote[]={"s3://bucket/a.csv","gs://bucket/a.csv","az://container/a.csv","hdfs://host/a.csv","https://example.invalid/a.csv"};
    MilenaDataSource s; MilenaError e; const char *p;
    assert(milena_data_source_parse(local,&s,&e)==MILENA_OK && s.kind==MILENA_DATA_SOURCE_LOCAL_FILE);
    assert(milena_data_source_require_local(&s,&p,&e)==MILENA_OK && strcmp(p,local)==0);
    for(size_t i=0;i<sizeof(remote)/sizeof(remote[0]);i++){assert(milena_data_source_parse(remote[i],&s,&e)==MILENA_OK);assert(s.kind!=MILENA_DATA_SOURCE_LOCAL_FILE);assert(milena_data_source_require_local(&s,&p,&e)==MILENA_ERR_UNSUPPORTED);assert(strstr(e.message,"solo admite rutas")!=NULL);}
    assert(milena_data_source_parse("",&s,&e)==MILENA_ERR_ARGUMENT);
    return 0;
}
