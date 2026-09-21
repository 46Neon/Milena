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
    const char *column;
    const char *name;
    MilenaStreamOperation operation;
} MilenaStreamMetric;

typedef struct {
    size_t rows_read;
    size_t rows_with_valid_values;
    size_t malformed_rows;
    size_t chunk_rows;
    double elapsed_milliseconds;
} MilenaStreamReport;

/*
 * Summarizes a CSV without materializing it as Dataset or MilenaTable.
 * The output is a JSON report. Memory is O(columns + metrics + max_record).
 */
MilenaStatus milena_stream_csv_summary(const char *input_path,
                                       const char *output_path,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       size_t chunk_rows,
                                       MilenaStreamReport *report,
                                       MilenaError *error);

const char *milena_stream_operation_name(MilenaStreamOperation operation);

#endif
