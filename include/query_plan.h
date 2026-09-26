#ifndef MILENA_QUERY_PLAN_H
#define MILENA_QUERY_PLAN_H

#include "ast.h"
#include "common.h"
#include "table.h"

/* Canonical logical contract for the current, intentionally narrow overlap
 * between materialized table HIR and CSV record-streaming execution. Strings
 * are borrowed from the validated source plans. This is not an engine-neutral
 * physical plan, and it does not imply Arrow/SQLite support. */
#define MILENA_DATA_PLAN_MAX_METRICS 64u
#define MILENA_DATA_PLAN_MAX_OPERATORS 4u
#define MILENA_DATA_PLAN_MAX_SCHEMA_REFS (MILENA_DATA_PLAN_MAX_METRICS + 2u)

typedef enum {
    MILENA_DATA_OPERATOR_CSV_SCAN = 1,
    MILENA_DATA_OPERATOR_NUMERIC_GREATER_FILTER,
    MILENA_DATA_OPERATOR_GROUP_AGGREGATE,
    MILENA_DATA_OPERATOR_JSON_SINK
} MilenaDataOperatorKind;

typedef enum {
    MILENA_DATA_AGGREGATE_SUM = 1,
    MILENA_DATA_AGGREGATE_MEAN,
    MILENA_DATA_AGGREGATE_COUNT
} MilenaDataAggregateKind;

typedef enum {
    MILENA_DATA_EXECUTION_MATERIALIZED_TABLE = 1,
    MILENA_DATA_EXECUTION_CSV_RECORD_STREAM
} MilenaDataExecutionMode;

typedef struct {
    const char *input_column;
    MilenaDataAggregateKind operation;
} MilenaDataPlanMetric;

typedef struct {
    const char *name;
    size_t declaration_index;
    unsigned declared_type;
} MilenaDataPlanSchemaRef;

typedef struct {
    const char *source_path;
    const char *sink_path;
    MilenaDataExecutionMode execution_mode;
    MilenaDataOperatorKind operators[MILENA_DATA_PLAN_MAX_OPERATORS];
    size_t operator_count;
    bool has_numeric_greater_filter;
    const char *filter_column;
    double filter_threshold;
    const char *group_key;
    MilenaDataPlanMetric metrics[MILENA_DATA_PLAN_MAX_METRICS];
    size_t metric_count;
    /* Materialized preflight identity, populated only by the typed-HIR
     * preflight builder. Column types are MilenaHIRColumnType identities. */
    bool has_preflight_identity;
    size_t source_dataset_id;
    size_t source_max_rows;
    size_t source_max_columns;
    size_t source_max_record_bytes;
    size_t source_max_memory_bytes;
    double source_max_elapsed_milliseconds;
    size_t group_limit_input_rows;
    size_t group_limit_output_rows;
    size_t group_limit_columns;
    MilenaDataPlanSchemaRef schema_refs[MILENA_DATA_PLAN_MAX_SCHEMA_REFS];
    size_t schema_ref_count;
} MilenaDataOperatorPlan;

struct MilenaDataHIR;
struct MilenaStreamExecutionPlan;
MilenaStatus milena_data_operator_plan_from_hir(
    const struct MilenaDataHIR *hir, MilenaDataOperatorPlan *plan,
    MilenaError *error);
/* Builds the common materialized subset from the already parsed typed HIR and
 * explicit source declarations, without binding or opening the input file. */
MilenaStatus milena_data_operator_plan_preflight_from_hir(
    const struct MilenaDataHIR *hir, MilenaDataOperatorPlan *plan,
    MilenaError *error);
/* Checks typed numeric transforms already represented in the source HIR before
 * a materialized input is opened; it does not extend the common operator set. */
MilenaStatus milena_data_operator_hir_transform_preflight(
    const struct MilenaDataHIR *hir, MilenaError *error);
/* Rebuilds the post-bind plan and verifies that it retains the preflighted
 * source, schema identities, operation graph, and declared limits. */
MilenaStatus milena_data_operator_plan_check_bound_hir(
    const MilenaDataOperatorPlan *preflight,
    const struct MilenaDataHIR *bound_hir, MilenaError *error);
MilenaStatus milena_data_operator_plan_from_stream(
    const ASTNode *analysis, const struct MilenaStreamExecutionPlan *stream_plan,
    MilenaDataOperatorPlan *plan, MilenaError *error);
MilenaStatus milena_data_operator_plan_validate(
    const MilenaDataOperatorPlan *plan, MilenaError *error);
/* Executes only the validated materialized overlap by following the common
 * operator sequence. Input is borrowed; output is replaced transactionally. */
MilenaStatus milena_data_operator_plan_execute_materialized(
    const MilenaDataOperatorPlan *plan, const MilenaTable *input,
    size_t max_input_rows, size_t max_output_rows, size_t max_columns,
    MilenaTable *output, MilenaError *error);
bool milena_data_operator_plans_same_logic(
    const MilenaDataOperatorPlan *left,
    const MilenaDataOperatorPlan *right);

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

typedef struct MilenaStreamExecutionPlan {
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

/* Separate native columnar vertical: this type and planner do not alter the
 * existing CSV plan, scanner, or physical operator chain. */
typedef enum {
    MILENA_ARROW_LOGICAL_SCAN_STREAM = 1,
    MILENA_ARROW_LOGICAL_FILTER,
    MILENA_ARROW_LOGICAL_PROJECT,
    MILENA_ARROW_LOGICAL_STREAM_SINK
} MilenaArrowLogicalOperator;

typedef enum {
    MILENA_ARROW_PHYSICAL_IPC_STREAM_SCAN = 1,
    MILENA_ARROW_PHYSICAL_FILTER,
    MILENA_ARROW_PHYSICAL_PROJECT,
    MILENA_ARROW_PHYSICAL_IPC_STREAM_WRITE
} MilenaArrowPhysicalOperator;

#define MILENA_ARROW_PLAN_MAX_OPERATORS 4u
#define MILENA_ARROW_PLAN_MAX_COLUMNS 128u

typedef struct MilenaArrowIpcExecutionPlan {
    const ASTNode *source;
    const ASTNode *projection;
    const ASTNode *filter;
    const ASTNode *sink;
    const ASTNode *projection_declarations[MILENA_ARROW_PLAN_MAX_COLUMNS];
    const ASTNode *filter_declaration;
    MilenaArrowLogicalOperator logical_operators[MILENA_ARROW_PLAN_MAX_OPERATORS];
    size_t logical_operator_count;
    MilenaArrowPhysicalOperator physical_operators[MILENA_ARROW_PLAN_MAX_OPERATORS];
    size_t physical_operator_count;
    const char *physical_plan_reason;
} MilenaArrowIpcExecutionPlan;

MilenaStatus milena_arrow_ipc_execution_plan_build(
    const ASTNode *analysis, MilenaArrowIpcExecutionPlan *plan,
    MilenaError *error);
MilenaStatus milena_arrow_ipc_execution_plan_validate(
    const MilenaArrowIpcExecutionPlan *plan, MilenaError *error);

/* Existing native SQL is a bounded raw-SQL compatibility baseline, not a typed
 * ORM plan. Only parameter literals are represented with typed AST nodes; table,
 * projection, filter and result schemas are not represented here. The backend
 * accepts exactly one SQLite statement and binds values without interpolation.
 * This plan must not be described as a completed SQL/ORM vertical. */
typedef enum {
    MILENA_SQL_PLAN_QUERY = 1,
    MILENA_SQL_PLAN_EXECUTE,
    MILENA_SQL_PLAN_BEGIN,
    MILENA_SQL_PLAN_COMMIT,
    MILENA_SQL_PLAN_ROLLBACK,
    MILENA_SQL_PLAN_SCHEMA,
    MILENA_SQL_PLAN_TYPED_SELECT,
    MILENA_SQL_PLAN_TYPED_INSERT,
    MILENA_SQL_PLAN_TYPED_UPDATE
} MilenaSqlPlanOperationKind;
typedef enum {
    MILENA_SQL_PLAN_NULL = 0,
    MILENA_SQL_PLAN_INT64,
    MILENA_SQL_PLAN_FLOAT64,
    MILENA_SQL_PLAN_TEXT
} MilenaSqlPlanParameterKind;
typedef struct { const char *data; size_t length; } MilenaSqlPlanText;
typedef struct {
    MilenaSqlPlanParameterKind kind;
    union { int64_t i64; double f64; MilenaSqlPlanText text; } value;
} MilenaSqlPlanParameter;
typedef struct {
    const ASTNode *source;
    const ASTNode *schema_column;
    const char *name;
    ASTSqlType type;
} MilenaSqlTypedProjection;
typedef struct {
    const ASTNode *source;
    const ASTNode *schema_column;
    const char *name;
    ASTSqlType type;
} MilenaSqlTypedInsertColumn;
typedef struct {
    const ASTNode *source;
    const ASTNode *schema_column;
    ASTSqlType type;
} MilenaSqlTypedInsertValue;
typedef struct {
    const ASTNode *source; /* AST_SQL_UPDATE_ASSIGNMENT */
    const ASTNode *column_source;
    const ASTNode *value_source;
    const ASTNode *schema_column;
    const char *name;
    ASTSqlType type;
} MilenaSqlTypedUpdateAssignment;
typedef struct {
    MilenaSqlPlanOperationKind kind;
    const ASTNode *source;
    const char *statement;
    char *owned_statement; /* generated only for a validated typed SQL operation */
    MilenaSqlPlanParameter *parameters;
    size_t parameter_count;
    /* Typed SELECT plan nodes; borrowed from the validated source AST. */
    const ASTNode *typed_table;
    const ASTNode *typed_schema;
    MilenaSqlTypedProjection *projections;
    size_t projection_count;
    const ASTNode *filter_column;
    const ASTNode *filter_operator;
    const ASTNode *typed_parameter;
    /* Typed INSERT columns and values resolved against the preceding schema. */
    MilenaSqlTypedInsertColumn *insert_columns;
    size_t insert_column_count;
    MilenaSqlTypedInsertValue *insert_values;
    size_t insert_value_count;
    /* Typed UPDATE metadata, separate from raw SQL and typed INSERT. */
    MilenaSqlTypedUpdateAssignment *update_assignments;
    size_t update_assignment_count;
} MilenaSqlPlanOperation;
typedef struct MilenaSqlExecutionPlan {
    const ASTNode *source;
    const char *connection_path;
    MilenaSqlPlanOperation *operations;
    size_t operation_count;
    size_t max_rows;
    size_t max_bytes;
    unsigned timeout_ms;
    bool explicit_limits;
} MilenaSqlExecutionPlan;
/* Validates typed SQL declarations/selects/updates without opening a database. */
MilenaStatus milena_sql_semantic_validate(const ASTNode *program,
    MilenaError *error);
MilenaStatus milena_sql_execution_plan_build(const ASTNode *program,
    MilenaSqlExecutionPlan *plan, MilenaError *error);
MilenaStatus milena_sql_execution_plan_validate(const MilenaSqlExecutionPlan *plan,
    MilenaError *error);
void milena_sql_execution_plan_destroy(MilenaSqlExecutionPlan *plan);

#endif
