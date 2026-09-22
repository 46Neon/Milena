#ifndef MILENA_STREAM_H
#define MILENA_STREAM_H
#include "common.h"
typedef enum { MILENA_STREAM_SUM, MILENA_STREAM_MEAN, MILENA_STREAM_MIN, MILENA_STREAM_MAX, MILENA_STREAM_COUNT, MILENA_STREAM_VARIANCE, MILENA_STREAM_STDDEV } MilenaStreamOperation;
typedef struct {
    size_t chunk_rows, max_record_bytes, max_columns, max_groups;
    /* Optional local out-of-core grouped aggregation. Zero disables spill. */
    const char *spill_directory;
    size_t spill_partitions, spill_max_bytes, spill_max_records;
} MilenaStreamOptions;
typedef struct { const char *column; const char *name; MilenaStreamOperation operation; } MilenaStreamMetric;
typedef struct {
    size_t rows_read, rows_with_valid_values, malformed_rows, input_bytes, chunk_rows;
    double elapsed_milliseconds;
    size_t observed_record_bytes, peak_record_bytes, header_columns, max_record_bytes, max_columns;
    size_t spilled_bytes, spill_partitions, spill_temp_files, spill_rows;
} MilenaStreamReport;
MilenaStreamOptions milena_stream_options_default(void);
MilenaStatus milena_stream_csv_summary_with_options(const char *,const char *,const MilenaStreamMetric *,size_t,const MilenaStreamOptions *,MilenaStreamReport *,MilenaError *);
MilenaStatus milena_stream_csv_summary(const char *,const char *,const MilenaStreamMetric *,size_t,size_t,MilenaStreamReport *,MilenaError *);
const char *milena_stream_operation_name(MilenaStreamOperation);
MilenaStatus milena_stream_csv_grouped_with_options(const char *,const char *,const char *,const MilenaStreamMetric *,size_t,const MilenaStreamOptions *,MilenaStreamReport *,MilenaError *);
#endif
