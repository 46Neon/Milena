#include "execution_contract.h"
#include <assert.h>
#include <string.h>
int main(void) { MilenaOperatorCapabilities c=milena_operator_capabilities(MILENA_OPERATOR_AGGREGATE); assert(c.streamable&&c.associative&&c.parallelizable&&!c.requires_shuffle); c=milena_operator_capabilities(MILENA_OPERATOR_SORT); assert(c.requires_materialization&&c.order_sensitive&&c.requires_shuffle); MilenaExecutionPlan p=milena_execution_plan_default(); assert(p.operator_kind==MILENA_OPERATOR_AGGREGATE); assert(milena_execution_validate(&p,0)==MILENA_ERR_ARGUMENT); return 0; }
