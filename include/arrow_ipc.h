#ifndef MILENA_ARROW_IPC_H
#define MILENA_ARROW_IPC_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Narrow native backend contract: Arrow IPC STREAM, local file only. */
typedef enum {
    MILENA_ARROW_FILTER_NONE = 0,
    MILENA_ARROW_FILTER_TEXT_EQUAL,
    MILENA_ARROW_FILTER_NUMERIC_GREATER
} MilenaArrowFilterKind;

typedef bool (*MilenaArrowCancellationRequested)(void *context);

typedef enum {
    MILENA_ARROW_VALUE_NUMERICA = 1, /* Arrow signed int64 or float64 */
    MILENA_ARROW_VALUE_TEXTO = 2    /* Arrow UTF-8 string */
} MilenaArrowValueType;

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *const *projection;
    const MilenaArrowValueType *projection_types;
    size_t projection_count;
    MilenaArrowFilterKind filter_kind;
    const char *filter_column;
    MilenaArrowValueType filter_column_type;
    const char *filter_text;
    double filter_number;
    size_t max_input_bytes;
    size_t max_output_bytes;
    size_t max_rows;
    size_t max_batch_rows;
    /* Maximum declared source record-batch body bytes, enforced before body
     * allocation/read. The scoped payload allocator also caps simultaneous
     * endian-conversion copies; this is not a whole-process RSS limit. */
    size_t max_batch_bytes;
    size_t max_columns;
    double max_elapsed_milliseconds;
    /* Optional cooperative cancellation callback, polled by the backend. */
    MilenaArrowCancellationRequested is_cancelled;
    void *cancel_context;
} MilenaArrowIpcOptions;

typedef struct {
    size_t input_rows;
    size_t output_rows;
    size_t input_batches;
    size_t output_batches;
    size_t input_bytes;
    size_t output_bytes;
} MilenaArrowIpcReport;

/* Reads and validates a STREAM, projects columns, optionally filters, and writes
 * an IPC STREAM to exclusive same-directory staging followed by atomic publish.
 * Existing output is untouched unless the complete stream has been written. */
MilenaStatus milena_arrow_ipc_stream_transform(
    const MilenaArrowIpcOptions *options,
    MilenaArrowIpcReport *report,
    MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
