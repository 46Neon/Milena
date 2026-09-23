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

#endif

