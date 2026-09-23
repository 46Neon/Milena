#ifndef MILENA_STREAM_H
#define MILENA_STREAM_H

#include "common.h"


/*
 * Bounded-memory execution for large CSV inputs. The stream path keeps only
 * the current record, the header and one accumulator per requested metric.
 */
typedef enum {
    MILENA_STREAM_SUM,
    MILENA_STREAM_MEAN,
    MILENA_STREAM_MIN,
    MILENA_STREAM_MAX,
    MILENA_STREAM_COUNT,
    MILENA_STREAM_VARIANCE,
    MILENA_STREAM_STDDEV
} MilenaStreamOperation;

typedef struct {
    size_t chunk_rows;
    size_t max_record_bytes;
    size_t max_columns;
    /* 0 means unlimited; production callers should set both budgets. */
    size_t max_rows;
    double max_elapsed_milliseconds;
    /* Hard-capped per-operation group budget; 0 selects the default. */
    size_t max_groups;
} MilenaStreamOptions;

typedef struct {
    const char *column;
    const char *name;
    MilenaStreamOperation operation;
} MilenaStreamMetric;

typedef struct {
    const char *scratch_path;
    size_t memory_budget_bytes;
    size_t spill_quota_bytes;
    size_t max_key_bytes;
    size_t max_output_groups;
    /* Hard upper bound for the staged JSON result; zero selects 1 GiB. */
    size_t max_output_bytes;
    size_t max_runs;
} MilenaStreamSpillPolicy;

typedef struct {
    size_t rows_read;
    size_t rows_with_valid_values;
    size_t malformed_rows;
    size_t input_bytes;
    size_t chunk_rows;
    double elapsed_milliseconds;
    /* Maximum bytes observed in a complete record (payload, excluding NUL). */
    size_t observed_record_bytes;
    /* Allocated buffer capacity; may exceed observed_record_bytes. */
    size_t peak_record_bytes;
    size_t header_columns;
    size_t max_record_bytes;
    size_t max_columns;
    size_t max_rows;
    double max_elapsed_milliseconds;
    bool resource_limit_reached;
    size_t bytes_read;
    size_t groups;
    size_t max_groups;
} MilenaStreamReport;

typedef enum {
    MILENA_STREAM_PLAN_SCAN_CSV_RECORDS = 0,
    MILENA_STREAM_PLAN_SUMMARY_AGGREGATE,
    MILENA_STREAM_PLAN_GROUPED_AGGREGATE,
    MILENA_STREAM_PLAN_GROUPED_SPILL,
    MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES,
    MILENA_STREAM_PLAN_ORDER_BY_KEY,
    MILENA_STREAM_PLAN_JSON_SINK
} MilenaStreamPlanOperator;

typedef enum {
    MILENA_STREAM_PLAN_SUMMARY = 0,
    MILENA_STREAM_PLAN_GROUPED,
    MILENA_STREAM_PLAN_GROUPED_SPILL_MODE
} MilenaStreamPlanKind;

/* A spillable grouped CSV plan names scan, local aggregate/run creation,
 * partial-state reduction, deterministic key ordering, and its sink. */
#define MILENA_STREAM_PLAN_MAX_OPERATORS 5u

typedef struct {
    MilenaStreamPlanKind kind;
    MilenaStreamPlanOperator operators[MILENA_STREAM_PLAN_MAX_OPERATORS];
    size_t operator_count;
    size_t partition_count;
    size_t worker_count;
    bool csv_record_safe;
    bool parallel_enabled;
    const char *reason;
} MilenaStreamExecutionPlan;

/* CSV physical plans begin with a single record-aware scan. Grouped spill
 * plans push aggregation, partial-state reduction, and final bytewise key
 * ordering before the JSON sink. The generic byte-range planner is deliberately
 * not used for quoted/multiline CSV; CSV plans remain sequential until a
 * record-boundary-aware partitioner and global reducer are implemented. */
MilenaStatus milena_stream_plan_build_csv(bool grouped, bool spill,
                                          MilenaStreamExecutionPlan *plan,
                                          MilenaError *error);
MilenaStatus milena_stream_plan_validate_csv(
    const MilenaStreamExecutionPlan *plan, MilenaError *error);
MilenaStatus milena_stream_execute_csv_plan(
    const MilenaStreamExecutionPlan *plan, const char *input_path,
    const char *output_path, const char *group_column,
    const MilenaStreamMetric *metrics, size_t metric_count,
    const MilenaStreamOptions *options,
    const MilenaStreamSpillPolicy *spill_policy,
    MilenaStreamReport *report, MilenaError *error);

/*
 * Summarizes a CSV without materializing it as Dataset or MilenaTable.
 * The output is a JSON report. Memory is O(columns + metrics + max_record).
 */
MilenaStreamOptions milena_stream_options_default(void);

MilenaStatus milena_stream_csv_summary_with_options(const char *input_path,
                                       const char *output_path,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       const MilenaStreamOptions *options,
                                       MilenaStreamReport *report,
                                       MilenaError *error);

MilenaStatus milena_stream_csv_summary(const char *input_path,
                                       const char *output_path,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       size_t chunk_rows,
                                       MilenaStreamReport *report,
                                       MilenaError *error);

const char *milena_stream_operation_name(MilenaStreamOperation operation);

/* Canonical one-pass CSV grouping. Group keys are emitted in bytewise order. */
MilenaStatus milena_stream_csv_grouped_with_options(const char *input_path,
                                       const char *output_path,
                                       const char *group_column,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       const MilenaStreamOptions *options,
                                       MilenaStreamReport *report,
                                       MilenaError *error);

/* CSV -> bounded parser -> spillable reducer -> callback-written JSON report.
 * Reducer memory is a separate budget, additional to record/header/column
 * buffers and one callback key; this does not claim a process-wide RSS cap. */
MilenaStatus milena_stream_csv_grouped_spill_with_options(
                                       const char *input_path,
                                       const char *output_path,
                                       const char *group_column,
                                       const MilenaStreamMetric *metric,
                                       const MilenaStreamOptions *options,
                                       const MilenaStreamSpillPolicy *policy,
                                       MilenaStreamReport *report,
                                       MilenaError *error);

#endif

