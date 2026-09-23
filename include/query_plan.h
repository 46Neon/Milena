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
    MILENA_LOGICAL_JSON_REPORT
} MilenaLogicalOperator;

typedef enum {
    MILENA_PHYSICAL_CSV_STREAM_SUMMARY = 1,
    MILENA_PHYSICAL_CSV_STREAM_GROUPED
} MilenaPhysicalOperator;

typedef struct {
    const ASTNode *source;
    const ASTNode *sink;
    const ASTNode *summary;
    const ASTNode *group;
    const ASTNode *group_key;
    const ASTNode *group_summary;
    MilenaLogicalOperator logical_operators[3];
    size_t logical_operator_count;
    MilenaPhysicalOperator physical_operator;
} MilenaStreamExecutionPlan;

/* Build a plan only from a semantically validated canonical AST. The function
 * still defensively rejects ambiguous/unsupported stream shapes. It does not
 * open files, infer schema, or execute any data operation. */
MilenaStatus milena_stream_execution_plan_build(
    const ASTNode *analysis, MilenaStreamExecutionPlan *plan,
    MilenaError *error);

#endif
