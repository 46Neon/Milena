#include "execution_contract.h"
#include <assert.h>
#include <string.h>
int main(void) {
    MilenaOperatorCapabilities c=milena_operator_capabilities(MILENA_OPERATOR_AGGREGATE); assert(c.streamable&&c.associative&&c.parallelizable&&!c.requires_shuffle);
    c=milena_operator_capabilities(MILENA_OPERATOR_SORT); assert(c.requires_materialization&&c.order_sensitive&&c.requires_shuffle);
    MilenaExecutionPlan p=milena_execution_plan_default(); assert(p.operator_kind==MILENA_OPERATOR_AGGREGATE); assert(milena_execution_validate(&p,0)==MILENA_ERR_ARGUMENT);
    p.operator_kind=MILENA_OPERATOR_PROJECT; p.projection_column="valor"; p.capabilities=milena_operator_capabilities(p.operator_kind); assert(milena_execution_validate(&p,0)==MILENA_OK);
    p.operator_kind=MILENA_OPERATOR_FILTER; p.projection_column=NULL; p.filter_expression="valor > 2"; p.capabilities=milena_operator_capabilities(p.operator_kind); assert(milena_execution_validate(&p,0)==MILENA_OK);
    p.source=MILENA_EXEC_SOURCE_CSV_STREAM; p.sink=MILENA_EXEC_SINK_JSON_REPORT; MilenaError e; assert(milena_stream_execute_plan(&p,"in.csv","out.json",NULL,&e)==MILENA_ERR_UNSUPPORTED); assert(strstr(e.message,"no materializa")!=NULL);
    return 0;
}
