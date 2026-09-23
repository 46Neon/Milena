#ifndef MILENA_QUERY_PLAN_H
#define MILENA_QUERY_PLAN_H

#include "ast.h"
#include "common.h"

/* Typed logical and physical plan annotations for the canonical .analisis path.
 * AST references are borrowed and remain valid while the runtime owns the AST. */
typedef enum {
    MILENA_LOGICAL_CSV_SCAN = 1,
    MILENA_LOGICAL_GLOBAL_AGGREGATE,
    MILENA_LOGICAL_GROUP_AGGREGATE,
    MILENA_LOGICAL_FILTER,
    MILENA_LOGICAL_JSON_REPORT
} MilenaLogicalOperator;

typedef enum {
    MILENA_PHYSICAL_CSV_STREAM_SUMMARY = 1,
    MILENA_PHYSICAL_CSV_STREAM_GROUPED,
    MILENA_PHYSICAL_CSV_STREAM_GROUPED_SPILL
} MilenaPhysicalOperator;

typedef enum {
    MILENA_STREAM_PLAN_SCAN_CSV_RECORDS = 0,
    MILENA_STREAM_PLAN_FILTER,
    MILENA_STREAM_PLAN_SUMMARY_AGGREGATE,
    MILENA_STREAM_PLAN_GROUPED_AGGREGATE,
    MILENA_STREAM_PLAN_GROUPED_SPILL,
    MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES,
    MILENA_STREAM_PLAN_ORDER_BY_KEY,
    MILENA_STREAM_PLAN_JSON_SINK
} MilenaStreamPlanOperator;

#define MILENA_STREAM_PLAN_MAX_OPERATORS 6u

typedef struct {
    const ASTNode *source;
    const ASTNode *filter;
    const ASTNode *sink;
    const ASTNode *summary;
    const ASTNode *group;
    const ASTNode *group_key; /* compatibility alias for group_keys[0] */
    const ASTNode *group_keys[2];
    size_t group_key_count;
    const ASTNode *group_summary;
    const ASTNode *spill_policy;
    MilenaLogicalOperator logical_operators[4];
    size_t logical_operator_count;
    MilenaPhysicalOperator physical_operator;
    /* Physical CSV pipeline derived from the typed AST; never byte-splits records. */
    MilenaStreamPlanOperator physical_operators[MILENA_STREAM_PLAN_MAX_OPERATORS];
    size_t physical_operator_count;
    size_t partition_count;
    size_t worker_count;
    bool csv_record_safe;
    bool parallel_enabled;
    const char *physical_plan_reason;
} MilenaStreamExecutionPlan;

/* Build a plan only from a semantically validated canonical AST. The function
 * still defensively rejects ambiguous/unsupported stream shapes. It does not
 * open files, infer schema, or execute any data operation. */
MilenaStatus milena_stream_execution_plan_build(
    const ASTNode *analysis, MilenaStreamExecutionPlan *plan,
    MilenaError *error);
MilenaStatus milena_stream_execution_plan_validate(
    const MilenaStreamExecutionPlan *plan, MilenaError *error);

#endif
