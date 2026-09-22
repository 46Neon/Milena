#ifndef MILENA_EXECUTION_CONTRACT_H
#define MILENA_EXECUTION_CONTRACT_H

#include "common.h"
#include "table.h"
#include "stream.h"

typedef enum { MILENA_EXEC_SOURCE_TABLE = 0, MILENA_EXEC_SOURCE_CSV_STREAM } MilenaExecutionSource;
typedef enum { MILENA_OPERATOR_AGGREGATE = 0, MILENA_OPERATOR_FILTER, MILENA_OPERATOR_PROJECT, MILENA_OPERATOR_SORT, MILENA_OPERATOR_JOIN, MILENA_OPERATOR_WINDOW } MilenaLogicalOperatorKind;
typedef struct { unsigned streamable:1; unsigned associative:1; unsigned order_sensitive:1; unsigned requires_materialization:1; unsigned parallelizable:1; unsigned requires_shuffle:1; } MilenaOperatorCapabilities;
typedef enum { MILENA_EXEC_SINK_TABLE = 0, MILENA_EXEC_SINK_JSON_REPORT } MilenaExecutionSink;

typedef struct { const char *column; const char *output_name; MilenaAggregateOp operation; } MilenaExecutionAggregate;
typedef struct {
    MilenaExecutionSource source;
    const char *group_column;
    const char *projection_column;
    const char *filter_expression;
    const MilenaExecutionAggregate *aggregates;
    size_t aggregate_count;
    MilenaStreamOptions options;
    MilenaExecutionSink sink;
    /* Additive logical-plan metadata; existing initializers remain valid. */
    MilenaLogicalOperatorKind operator_kind;
    MilenaOperatorCapabilities capabilities;
} MilenaExecutionPlan;
typedef struct {
    MilenaStatus status;
    size_t rows_read, rows_emitted, malformed_rows, groups;
    size_t peak_record_bytes, input_bytes;
    size_t spilled_bytes, spill_partitions, spill_temp_files, spill_rows;
    double elapsed_milliseconds;
    const char *backend;
} MilenaExecutionReport;
MilenaExecutionPlan milena_execution_plan_default(void);
MilenaOperatorCapabilities milena_operator_capabilities(MilenaLogicalOperatorKind kind);
const char *milena_operator_kind_name(MilenaLogicalOperatorKind kind);
MilenaStatus milena_execution_validate(const MilenaExecutionPlan *plan, MilenaError *error);
MilenaStatus milena_stream_execute_plan(const MilenaExecutionPlan *plan, const char *input_path, const char *output_path, MilenaExecutionReport *report, MilenaError *error);
MilenaStatus milena_table_execute_plan(MilenaTable *out, const MilenaTable *source, const MilenaExecutionPlan *plan, MilenaExecutionReport *report, MilenaError *error);
#endif

