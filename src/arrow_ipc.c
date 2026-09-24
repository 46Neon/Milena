#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "arrow_ipc.h"
#include "file_position.h"

#include <errno.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#define ARROW_STREAM_HARD_INPUT_BYTES (64u * 1024u * 1024u)
#define ARROW_STREAM_HARD_OUTPUT_BYTES (64u * 1024u * 1024u)
#define ARROW_STREAM_HARD_ROWS 10000000u
#define ARROW_STREAM_HARD_BATCH_ROWS 65536u
#define ARROW_STREAM_HARD_BATCH_BYTES (64u * 1024u * 1024u)
#define ARROW_STREAM_HARD_COLUMNS 128u

struct ArrowBoundedOutput {
    FILE *file;
    size_t bytes_written;
    size_t byte_limit;
};

struct ArrowBoundedInput {
    FILE *file;
    size_t bytes_read;
    size_t byte_limit;
};

struct ArrowBatchAllocationBudget {
    size_t byte_limit;
    size_t bytes_live;
    size_t peak_bytes_live;
};

/* Nanoarrow scopes this allocator to the IPC message-body buffer and any
 * endian-conversion scratch buffers. Releasing old storage before replacement
 * avoids an allocator-side old+new overlap; shared-buffer ownership ensures
 * the previous batch's body is no longer referenced before the next batch. */
static uint8_t *arrow_batch_budget_reallocate(
    struct ArrowBufferAllocator *allocator, uint8_t *ptr,
    int64_t old_size, int64_t new_size) {
    struct ArrowBatchAllocationBudget *budget =
        (struct ArrowBatchAllocationBudget *)allocator->private_data;
    if (!budget || old_size < 0 || new_size < 0) {
        free(ptr);
        return NULL;
    }

    if (ptr) {
        free(ptr);
        if ((uint64_t)old_size > budget->bytes_live) {
            budget->bytes_live = 0;
            return NULL;
        }
        budget->bytes_live -= (size_t)old_size;
    }
    if ((uint64_t)new_size > budget->byte_limit ||
        (size_t)new_size > budget->byte_limit - budget->bytes_live)
        return NULL;
    if (new_size == 0) return NULL;

    uint8_t *result = (uint8_t *)malloc((size_t)new_size);
    if (!result) return NULL;
    budget->bytes_live += (size_t)new_size;
    if (budget->bytes_live > budget->peak_bytes_live)
        budget->peak_bytes_live = budget->bytes_live;
    return result;
}

static void arrow_batch_budget_free(struct ArrowBufferAllocator *allocator,
                                    uint8_t *ptr, int64_t size) {
    struct ArrowBatchAllocationBudget *budget =
        (struct ArrowBatchAllocationBudget *)allocator->private_data;
    if (ptr) {
        free(ptr);
        if (budget && size >= 0 && (uint64_t)size <= budget->bytes_live)
            budget->bytes_live -= (size_t)size;
        else if (budget)
            budget->bytes_live = 0;
    }
}

static ArrowErrorCode arrow_bounded_read(struct ArrowIpcInputStream *stream,
                                         uint8_t *buffer, int64_t requested,
                                         int64_t *read_out,
                                         struct ArrowError *error) {
    struct ArrowBoundedInput *input =
        (struct ArrowBoundedInput *)stream->private_data;
    *read_out = 0;
    if (!input || !input->file || requested < 0 ||
        (uint64_t)requested > SIZE_MAX) {
        (void)ArrowErrorSet(error, "Invalid bounded Arrow input request");
        return EINVAL;
    }
    if (requested == 0) return NANOARROW_OK;
    if (input->bytes_read == input->byte_limit) {
        int extra = fgetc(input->file);
        if (extra != EOF) {
            (void)ArrowErrorSet(error, "Arrow input byte limit exceeded while reading");
            return EFBIG;
        }
        if (ferror(input->file)) {
            (void)ArrowErrorSet(error, "Could not read Arrow input file");
            return EIO;
        }
        return NANOARROW_OK;
    }
    size_t remaining = input->byte_limit - input->bytes_read;
    size_t amount = (size_t)requested;
    if (amount > remaining) amount = remaining;
    if (amount == 0u) return NANOARROW_OK;
    size_t count = fread(buffer, 1u, amount, input->file);
    input->bytes_read += count;
    *read_out = (int64_t)count;
    if (count != amount && ferror(input->file)) {
        (void)ArrowErrorSet(error, "Could not read Arrow input file");
        return EIO;
    }
    return NANOARROW_OK;
}

static void arrow_bounded_input_release(struct ArrowIpcInputStream *stream) {
    if (!stream) return;
    stream->release = NULL;
    stream->private_data = NULL;
}

static void arrow_runtime_error(MilenaError *error, MilenaStatus code,
                                const char *message) {
    if (error) milena_error_set(error, code, 0, 0, 0, message);
}

static double arrow_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (QueryPerformanceFrequency(&frequency) &&
        QueryPerformanceCounter(&counter) && frequency.QuadPart > 0)
        return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
    struct timespec value;
    if (timespec_get(&value, TIME_UTC) != TIME_UTC) return 0.0;
#else
    struct timespec monotonic;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0)
        return (double)monotonic.tv_sec * 1000.0 +
               (double)monotonic.tv_nsec / 1000000.0;
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0) return 0.0;
#endif
    return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
}

static bool arrow_cancelled(const MilenaArrowIpcOptions *options) {
    return options->is_cancelled &&
           options->is_cancelled(options->cancel_context);
}

static bool arrow_utf8_valid(const char *text, size_t length) {
    if (length != 0u && !text) return false;
    size_t i = 0;
    while (i < length) {
        const unsigned char a = (unsigned char)text[i++];
        if (a < 0x80u) continue;
        if (a >= 0xc2u && a <= 0xdfu) {
            if (i >= length || ((unsigned char)text[i++] & 0xc0u) != 0x80u) return false;
        } else if (a >= 0xe0u && a <= 0xefu) {
            if (i + 1u >= length) return false;
            const unsigned char b = (unsigned char)text[i++];
            const unsigned char c = (unsigned char)text[i++];
            if ((b & 0xc0u) != 0x80u || (c & 0xc0u) != 0x80u) return false;
            if ((a == 0xe0u && b < 0xa0u) || (a == 0xedu && b >= 0xa0u)) return false;
        } else if (a >= 0xf0u && a <= 0xf4u) {
            if (i + 2u >= length) return false;
            const unsigned char b = (unsigned char)text[i++];
            const unsigned char c = (unsigned char)text[i++];
            const unsigned char d = (unsigned char)text[i++];
            if ((b & 0xc0u) != 0x80u || (c & 0xc0u) != 0x80u ||
                (d & 0xc0u) != 0x80u) return false;
            if ((a == 0xf0u && b < 0x90u) || (a == 0xf4u && b >= 0x90u)) return false;
        } else {
            return false;
        }
    }
    return true;
}

static ArrowErrorCode arrow_bounded_write(struct ArrowIpcOutputStream *stream,
                                          const void *data, int64_t size,
                                          int64_t *written,
                                          struct ArrowError *error) {
    struct ArrowBoundedOutput *output =
        (struct ArrowBoundedOutput *)stream->private_data;
    *written = 0;
    if (!output || !output->file || size < 0 || (uint64_t)size > SIZE_MAX) {
        (void)ArrowErrorSet(error, "Invalid bounded Arrow output request");
        return EINVAL;
    }
    size_t requested = (size_t)size;
    if (requested > output->byte_limit - output->bytes_written) {
        (void)ArrowErrorSet(error, "Arrow output byte limit exceeded");
        return EFBIG;
    }
    if (requested == 0) return NANOARROW_OK;
    size_t count = fwrite(data, 1u, requested, output->file);
    output->bytes_written += count;
    *written = (int64_t)count;
    if (count != requested) {
        (void)ArrowErrorSet(error, "Could not write complete staged Arrow output");
        return errno ? errno : EIO;
    }
    return NANOARROW_OK;
}

static void arrow_bounded_output_release(struct ArrowIpcOutputStream *stream) {
    if (!stream) return;
    stream->release = NULL;
    stream->private_data = NULL;
}

static bool arrow_publish_staged(const char *staging_path,
                                 const char *output_path) {
#ifdef _WIN32
    return MoveFileExA(staging_path, output_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(staging_path, output_path) == 0;
#endif
}

static FILE *arrow_open_staging(const char *output_path, char **staging_path,
                                MilenaError *error) {
    const size_t length = strlen(output_path);
    if (length > SIZE_MAX - 64u) {
        arrow_runtime_error(error, MILENA_ERR_OVERFLOW, "Arrow output path is too long");
        return NULL;
    }
    char *candidate = (char *)malloc(length + 64u);
    if (!candidate) {
        arrow_runtime_error(error, MILENA_ERR_MEMORY, "No memory for Arrow staging path");
        return NULL;
    }
    const unsigned long long stamp = (unsigned long long)(arrow_now_ms() * 1000.0);
    for (unsigned int attempt = 0; attempt < 128u; ++attempt) {
        (void)snprintf(candidate, length + 64u, "%s.part.%llu.%u",
                       output_path, stamp, attempt);
        FILE *file = fopen(candidate, "wbx");
        if (file) {
            *staging_path = candidate;
            return file;
        }
        if (errno != EEXIST) break;
    }
    free(candidate);
    arrow_runtime_error(error, MILENA_ERR_IO,
                        "Could not create exclusive Arrow staging file");
    return NULL;
}

static bool arrow_declared_type_matches(MilenaArrowValueType declared,
                                        enum ArrowType logical_type) {
    if (declared == MILENA_ARROW_VALUE_NUMERICA)
        return logical_type == NANOARROW_TYPE_INT64 ||
               logical_type == NANOARROW_TYPE_DOUBLE;
    if (declared == MILENA_ARROW_VALUE_TEXTO)
        return logical_type == NANOARROW_TYPE_STRING;
    return false;
}

static int arrow_find_column(const struct ArrowSchema *schema, const char *name) {
    if (!schema || !name) return -1;
    for (int64_t i = 0; i < schema->n_children; ++i) {
        if (schema->children[i]->name && strcmp(schema->children[i]->name, name) == 0)
            return (int)i;
    }
    return -1;
}

static bool arrow_numeric_greater(const struct ArrowArrayView *view,
                                  int64_t row, double threshold) {
    if (view->storage_type == NANOARROW_TYPE_DOUBLE)
        return ArrowArrayViewGetDoubleUnsafe(view, row) > threshold;
    if (view->storage_type == NANOARROW_TYPE_INT64) {
        const double int64_min = -9223372036854775808.0;
        const double int64_upper = 9223372036854775808.0;
        if (threshold < int64_min) return true;
        if (threshold >= int64_upper) return false;
        /* For an integer x, x > d is equivalent to x > floor(d). This avoids
         * converting int64 data to binary64 and losing values above 2^53. */
        return ArrowArrayViewGetIntUnsafe(view, row) > (int64_t)floor(threshold);
    }
    return false;
}

static bool arrow_filter_matches(const MilenaArrowIpcOptions *options,
                                 const struct ArrowArrayView *batch,
                                 int filter_index, int64_t row) {
    if (options->filter_kind == MILENA_ARROW_FILTER_NONE) return true;
    const struct ArrowArrayView *filter = batch->children[filter_index];
    if (ArrowArrayViewIsNull(filter, row)) return false;
    if (options->filter_kind == MILENA_ARROW_FILTER_NUMERIC_GREATER)
        return arrow_numeric_greater(filter, row, options->filter_number);
    struct ArrowStringView value = ArrowArrayViewGetStringUnsafe(filter, row);
    const size_t expected_length = strlen(options->filter_text);
    return value.size_bytes >= 0 && (size_t)value.size_bytes == expected_length &&
           (expected_length == 0 || memcmp(value.data, options->filter_text,
                                            expected_length) == 0);
}

static ArrowErrorCode arrow_append_value(struct ArrowArray *out,
                                         const struct ArrowArrayView *in,
                                         int64_t row) {
    if (ArrowArrayViewIsNull(in, row)) return ArrowArrayAppendNull(out, 1);
    switch (in->storage_type) {
        case NANOARROW_TYPE_INT64:
            return ArrowArrayAppendInt(out, ArrowArrayViewGetIntUnsafe(in, row));
        case NANOARROW_TYPE_DOUBLE:
            return ArrowArrayAppendDouble(out, ArrowArrayViewGetDoubleUnsafe(in, row));
        case NANOARROW_TYPE_STRING:
            return ArrowArrayAppendString(out, ArrowArrayViewGetStringUnsafe(in, row));
        default:
            return EINVAL;
    }
}

static bool arrow_view_accumulate_buffer_bytes(struct ArrowArrayView *view,
                                               size_t byte_limit,
                                               size_t *total_bytes) {
    if (!view || !total_bytes || *total_bytes > byte_limit) return false;
    int64_t num_buffers = ArrowArrayViewGetNumBuffers(view);
    if (num_buffers < 0) return false;
    for (int64_t i = 0; i < num_buffers; ++i) {
        struct ArrowBufferView buffer = ArrowArrayViewGetBufferView(view, i);
        if (buffer.size_bytes < 0 ||
            (uint64_t)buffer.size_bytes > byte_limit - *total_bytes)
            return false;
        *total_bytes += (size_t)buffer.size_bytes;
    }
    for (int64_t i = 0; i < view->n_children; ++i) {
        if (!arrow_view_accumulate_buffer_bytes(view->children[i], byte_limit,
                                                total_bytes)) return false;
    }
    return true;
}

static MilenaStatus arrow_validate_options(const MilenaArrowIpcOptions *options,
                                            size_t *max_input,
                                            size_t *max_output,
                                            size_t *max_rows,
                                            size_t *max_batch,
                                            size_t *max_batch_bytes,
                                            size_t *max_columns,
                                            MilenaError *error) {
    if (!options || !options->input_path || !options->input_path[0] ||
        !options->output_path || !options->output_path[0] ||
        strcmp(options->input_path, options->output_path) == 0 ||
        !options->projection || !options->projection_types ||
        options->projection_count == 0 ||
        options->projection_count > ARROW_STREAM_HARD_COLUMNS ||
        options->filter_kind < MILENA_ARROW_FILTER_NONE ||
        options->filter_kind > MILENA_ARROW_FILTER_NUMERIC_GREATER ||
        (options->filter_kind != MILENA_ARROW_FILTER_NONE &&
         (!options->filter_column || !options->filter_column[0])) ||
        (options->filter_kind == MILENA_ARROW_FILTER_TEXT_EQUAL &&
         (!options->filter_text || options->filter_column_type != MILENA_ARROW_VALUE_TEXTO)) ||
        (options->filter_kind == MILENA_ARROW_FILTER_NUMERIC_GREATER &&
         options->filter_column_type != MILENA_ARROW_VALUE_NUMERICA) ||
        (options->filter_kind == MILENA_ARROW_FILTER_NUMERIC_GREATER &&
         !isfinite(options->filter_number)) ||
        (options->max_elapsed_milliseconds != 0.0 &&
         (!isfinite(options->max_elapsed_milliseconds) ||
          options->max_elapsed_milliseconds < 1.0 ||
          options->max_elapsed_milliseconds > 3600000.0))) {
        arrow_runtime_error(error, MILENA_ERR_ARGUMENT,
                            "Invalid Arrow IPC STREAM execution options");
        return MILENA_ERR_ARGUMENT;
    }
    *max_input = options->max_input_bytes ? options->max_input_bytes : ARROW_STREAM_HARD_INPUT_BYTES;
    *max_output = options->max_output_bytes ? options->max_output_bytes : ARROW_STREAM_HARD_OUTPUT_BYTES;
    *max_rows = options->max_rows ? options->max_rows : ARROW_STREAM_HARD_ROWS;
    *max_batch = options->max_batch_rows ? options->max_batch_rows : ARROW_STREAM_HARD_BATCH_ROWS;
    *max_batch_bytes = options->max_batch_bytes ? options->max_batch_bytes :
                       ARROW_STREAM_HARD_BATCH_BYTES;
    *max_columns = options->max_columns ? options->max_columns : ARROW_STREAM_HARD_COLUMNS;
    if (*max_input > ARROW_STREAM_HARD_INPUT_BYTES || *max_input == 0 ||
        *max_output > ARROW_STREAM_HARD_OUTPUT_BYTES || *max_output == 0 ||
        *max_rows > ARROW_STREAM_HARD_ROWS || *max_rows == 0 ||
        *max_batch > ARROW_STREAM_HARD_BATCH_ROWS || *max_batch == 0 ||
        *max_batch_bytes > ARROW_STREAM_HARD_BATCH_BYTES || *max_batch_bytes == 0 ||
        *max_columns > ARROW_STREAM_HARD_COLUMNS || *max_columns == 0 ||
        options->projection_count > *max_columns) {
        arrow_runtime_error(error, MILENA_ERR_OVERFLOW,
                            "Arrow IPC resource policy exceeds a hard limit");
        return MILENA_ERR_OVERFLOW;
    }
    return MILENA_OK;
}

MilenaStatus milena_arrow_ipc_stream_transform(
    const MilenaArrowIpcOptions *options, MilenaArrowIpcReport *report,
    MilenaError *error) {
    FILE *input_file = NULL;
    FILE *output_file = NULL;
    char *staging_path = NULL;
    struct ArrowIpcInputStream ipc_input = {0};
    struct ArrowBoundedInput bounded_input = {0};
    struct ArrowArrayStream reader = {0};
    struct ArrowSchema input_schema = {0};
    struct ArrowSchema output_schema = {0};
    struct ArrowArrayView input_view = {0};
    struct ArrowIpcOutputStream ipc_output = {0};
    struct ArrowIpcWriter writer = {0};
    struct ArrowBoundedOutput bounded_output = {0};
    struct ArrowError arrow_error;
    bool reader_live = false, input_schema_live = false;
    bool output_schema_live = false, input_view_live = false;
    bool writer_live = false, input_file_closed = false, output_file_closed = false;
    size_t max_input = 0, max_output = 0, max_rows = 0;
    size_t max_batch = 0, max_batch_bytes = 0, max_columns = 0;
    struct ArrowBatchAllocationBudget batch_allocation_budget = {0};
    int projection_indices[ARROW_STREAM_HARD_COLUMNS];
    int filter_index = -1;
    size_t input_size = 0, total_input_rows = 0, total_output_rows = 0;
    size_t input_batches = 0, output_batches = 0;
    double start_ms = arrow_now_ms();
    MilenaStatus status;
    if (report) memset(report, 0, sizeof(*report));
    if (error) milena_error_clear(error);
    status = arrow_validate_options(options, &max_input, &max_output, &max_rows,
                                    &max_batch, &max_batch_bytes, &max_columns, error);
    if (status != MILENA_OK) return status;
    batch_allocation_budget.byte_limit = max_batch_bytes;
    if (arrow_cancelled(options)) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Arrow IPC operation cancelled");
        return MILENA_ERR_IO;
    }

    input_file = fopen(options->input_path, "rb");
    if (!input_file) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Could not open local Arrow input");
        status = MILENA_ERR_IO;
        goto cleanup;
    }
    if (milena_file_seek64(input_file, 0, SEEK_END) != 0) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Could not determine Arrow input size");
        status = MILENA_ERR_IO;
        goto cleanup;
    }
    int64_t input_size_64 = milena_file_tell64(input_file);
    if (input_size_64 < 0 || (uint64_t)input_size_64 > max_input ||
        (uint64_t)input_size_64 > SIZE_MAX) {
        arrow_runtime_error(error, MILENA_ERR_OVERFLOW,
                            "Arrow input exceeds the configured byte limit");
        status = MILENA_ERR_OVERFLOW;
        goto cleanup;
    }
    input_size = (size_t)input_size_64;
    if (milena_file_seek64(input_file, 0, SEEK_SET) != 0) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Could not rewind Arrow input");
        status = MILENA_ERR_IO;
        goto cleanup;
    }
    bounded_input.file = input_file;
    bounded_input.byte_limit = max_input;
    ipc_input.read = arrow_bounded_read;
    ipc_input.release = arrow_bounded_input_release;
    ipc_input.private_data = &bounded_input;
    struct ArrowIpcArrayStreamReaderOptions reader_options = {-1, 1};
    if (ArrowIpcArrayStreamReaderInit(&reader, &ipc_input, &reader_options) != NANOARROW_OK) {
        if (ipc_input.release) ipc_input.release(&ipc_input);
        arrow_runtime_error(error, MILENA_ERR_DATA,
                            "Input is not a readable Arrow IPC STREAM");
        status = MILENA_ERR_DATA;
        goto cleanup;
    }
    reader_live = true;
    struct ArrowBufferAllocator body_allocator = {
        &arrow_batch_budget_reallocate, &arrow_batch_budget_free,
        &batch_allocation_budget
    };
    if (ArrowIpcArrayStreamReaderSetBodyAllocationLimit(
            &reader, (int64_t)max_batch_bytes, body_allocator) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_ARGUMENT,
                            "Could not install Arrow batch allocation budget");
        status = MILENA_ERR_ARGUMENT;
        goto cleanup;
    }
    if (!reader.get_schema || reader.get_schema(&reader, &input_schema) != 0) {
        const char *message = reader.get_last_error ? reader.get_last_error(&reader) : NULL;
        arrow_runtime_error(error, MILENA_ERR_DATA,
                            message ? message : "Invalid or truncated Arrow IPC schema");
        status = MILENA_ERR_DATA;
        goto cleanup;
    }
    input_schema_live = input_schema.release != NULL;
    struct ArrowSchemaView root_view;
    if (input_schema.n_children < 1 ||
        ArrowSchemaViewInit(&root_view, &input_schema, &arrow_error) != NANOARROW_OK ||
        root_view.type != NANOARROW_TYPE_STRUCT || root_view.extension_name.data != NULL ||
        (uint64_t)input_schema.n_children > max_columns) {
        arrow_runtime_error(error, MILENA_ERR_UNSUPPORTED,
                            "Arrow IPC profile requires a bounded scalar struct schema");
        status = MILENA_ERR_UNSUPPORTED;
        goto cleanup;
    }
    for (int64_t i = 0; i < input_schema.n_children; ++i) {
        const struct ArrowSchema *field = input_schema.children[i];
        struct ArrowSchemaView field_view;
        if (!field || !field->name || !field->name[0] || field->n_children != 0 ||
            field->dictionary != NULL ||
            ArrowSchemaViewInit(&field_view, field, &arrow_error) != NANOARROW_OK ||
            field_view.extension_name.data != NULL) {
            arrow_runtime_error(error, MILENA_ERR_UNSUPPORTED,
                "Arrow IPC profile requires non-nested, non-dictionary scalar fields without extensions");
            status = MILENA_ERR_UNSUPPORTED;
            goto cleanup;
        }
        for (int64_t j = 0; j < i; ++j) {
            if (strcmp(input_schema.children[j]->name, field->name) == 0) {
                arrow_runtime_error(error, MILENA_ERR_DATA,
                                    "Arrow schema contains duplicate field names");
                status = MILENA_ERR_DATA;
                goto cleanup;
            }
        }
    }
    for (size_t i = 0; i < options->projection_count; ++i) {
        if (!options->projection[i] || !options->projection[i][0]) {
            arrow_runtime_error(error, MILENA_ERR_ARGUMENT,
                                "Projection contains an empty field name");
            status = MILENA_ERR_ARGUMENT;
            goto cleanup;
        }
        int index = arrow_find_column(&input_schema, options->projection[i]);
        if (index < 0) {
            arrow_runtime_error(error, MILENA_ERR_DATA,
                                "Projection references a missing Arrow field");
            status = MILENA_ERR_DATA;
            goto cleanup;
        }
        for (size_t j = 0; j < i; ++j) {
            if (projection_indices[j] == index) {
                arrow_runtime_error(error, MILENA_ERR_DATA,
                                    "Projection field names must be unique");
                status = MILENA_ERR_DATA;
                goto cleanup;
            }
        }
        struct ArrowSchemaView projected_view;
        if (ArrowSchemaViewInit(&projected_view,
                input_schema.children[index], &arrow_error) != NANOARROW_OK ||
            !arrow_declared_type_matches(options->projection_types[i],
                                         projected_view.type)) {
            arrow_runtime_error(error, MILENA_ERR_TYPE,
                "Arrow projection field does not match its Milena numeric/text declaration");
            status = MILENA_ERR_TYPE;
            goto cleanup;
        }
        projection_indices[i] = index;
    }
    if (options->filter_kind != MILENA_ARROW_FILTER_NONE) {
        filter_index = arrow_find_column(&input_schema, options->filter_column);
        if (filter_index < 0) {
            arrow_runtime_error(error, MILENA_ERR_DATA,
                                "Filter references a missing Arrow field");
            status = MILENA_ERR_DATA;
            goto cleanup;
        }
        struct ArrowSchemaView filter_view;
        if (ArrowSchemaViewInit(&filter_view,
                input_schema.children[filter_index], &arrow_error) != NANOARROW_OK ||
            !arrow_declared_type_matches(options->filter_column_type,
                                         filter_view.type) ||
            (options->filter_kind == MILENA_ARROW_FILTER_TEXT_EQUAL &&
             filter_view.type != NANOARROW_TYPE_STRING) ||
            (options->filter_kind == MILENA_ARROW_FILTER_NUMERIC_GREATER &&
             filter_view.type != NANOARROW_TYPE_INT64 &&
             filter_view.type != NANOARROW_TYPE_DOUBLE)) {
            arrow_runtime_error(error, MILENA_ERR_TYPE,
                                "Arrow filter operator does not match the field type");
            status = MILENA_ERR_TYPE;
            goto cleanup;
        }
    }
    if (ArrowArrayViewInitFromSchema(&input_view, &input_schema, &arrow_error) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_DATA, "Could not create Arrow batch view");
        status = MILENA_ERR_DATA;
        goto cleanup;
    }
    input_view_live = true;
    ArrowSchemaInit(&output_schema);
    output_schema_live = true;
    if (ArrowSchemaSetTypeStruct(&output_schema,
             (int64_t)options->projection_count) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_MEMORY, "Could not create projected Arrow schema");
        status = MILENA_ERR_MEMORY;
        goto cleanup;
    }
    if (input_schema.metadata &&
        ArrowSchemaSetMetadata(&output_schema, input_schema.metadata) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_MEMORY,
                            "Could not preserve Arrow stream schema metadata");
        status = MILENA_ERR_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0; i < options->projection_count; ++i) {
        struct ArrowSchema *child = output_schema.children[i];
        if (child->release) child->release(child);
        if (ArrowSchemaDeepCopy(input_schema.children[projection_indices[i]], child) != NANOARROW_OK) {
            arrow_runtime_error(error, MILENA_ERR_MEMORY, "Could not copy projected Arrow field");
            status = MILENA_ERR_MEMORY;
            goto cleanup;
        }
    }

    output_file = arrow_open_staging(options->output_path, &staging_path, error);
    if (!output_file) { status = error ? error->code : MILENA_ERR_IO; goto cleanup; }
    bounded_output.file = output_file;
    bounded_output.byte_limit = max_output;
    ipc_output.write = arrow_bounded_write;
    ipc_output.release = arrow_bounded_output_release;
    ipc_output.private_data = &bounded_output;
    if (ArrowIpcWriterInit(&writer, &ipc_output) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_MEMORY, "Could not initialize Arrow IPC writer");
        status = MILENA_ERR_MEMORY;
        goto cleanup;
    }
    writer_live = true;
    if (ArrowIpcWriterWriteSchema(&writer, &output_schema, &arrow_error) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Could not stage Arrow IPC schema");
        status = MILENA_ERR_IO;
        goto cleanup;
    }

    for (;;) {
        struct ArrowArray batch = {0};
        int next_status = reader.get_next(&reader, &batch);
        if (next_status != 0) {
            const char *message = reader.get_last_error ? reader.get_last_error(&reader) : NULL;
            bool body_limit_exceeded = message &&
                strstr(message, "record batch body exceeds configured byte limit") != NULL;
            status = body_limit_exceeded ? MILENA_ERR_OVERFLOW : MILENA_ERR_DATA;
            arrow_runtime_error(error, status,
                                message ? message : "Malformed or truncated Arrow IPC batch");
            if (batch.release) batch.release(&batch);
            goto cleanup;
        }
        if (!batch.release) break;
        if (batch.length < 0 || (uint64_t)batch.length > max_batch ||
            !milena_size_add(total_input_rows, (size_t)batch.length, &total_input_rows) ||
            total_input_rows > max_rows) {
            if (batch.release) batch.release(&batch);
            arrow_runtime_error(error, MILENA_ERR_OVERFLOW,
                                "Arrow record batch or total row limit exceeded");
            status = MILENA_ERR_OVERFLOW;
            goto cleanup;
        }
        input_batches++;
        if (ArrowArrayViewSetArray(&input_view, &batch, &arrow_error) != NANOARROW_OK ||
            ArrowArrayViewValidate(&input_view, NANOARROW_VALIDATION_LEVEL_FULL,
                                   &arrow_error) != NANOARROW_OK) {
            if (batch.release) batch.release(&batch);
            arrow_runtime_error(error, MILENA_ERR_DATA,
                                "Arrow IPC batch is malformed or truncated");
            status = MILENA_ERR_DATA;
            goto cleanup;
        }
        size_t decoded_batch_buffer_bytes = 0;
        if (!arrow_view_accumulate_buffer_bytes(&input_view, max_batch_bytes,
                                                &decoded_batch_buffer_bytes)) {
            if (batch.release) batch.release(&batch);
            arrow_runtime_error(error, MILENA_ERR_OVERFLOW,
                "Arrow decoded record-batch buffers exceed the configured byte limit");
            status = MILENA_ERR_OVERFLOW;
            goto cleanup;
        }
        struct ArrowArray output_batch = {0};
        if (ArrowArrayInitFromSchema(&output_batch, &output_schema, &arrow_error) != NANOARROW_OK ||
            ArrowArrayStartAppending(&output_batch) != NANOARROW_OK) {
            if (output_batch.release) output_batch.release(&output_batch);
            if (batch.release) batch.release(&batch);
            arrow_runtime_error(error, MILENA_ERR_MEMORY,
                                "Could not allocate bounded Arrow output batch");
            status = MILENA_ERR_MEMORY;
            goto cleanup;
        }
        for (int64_t row = 0; row < batch.length; ++row) {
            if (arrow_cancelled(options)) {
                output_batch.release(&output_batch);
                batch.release(&batch);
                arrow_runtime_error(error, MILENA_ERR_IO, "Arrow IPC operation cancelled");
                status = MILENA_ERR_IO;
                goto cleanup;
            }
            if (options->max_elapsed_milliseconds > 0.0 &&
                arrow_now_ms() - start_ms > options->max_elapsed_milliseconds) {
                output_batch.release(&output_batch);
                batch.release(&batch);
                arrow_runtime_error(error, MILENA_ERR_OVERFLOW,
                                    "Arrow IPC time budget exceeded");
                status = MILENA_ERR_OVERFLOW;
                goto cleanup;
            }
            /* Validate UTF-8 even in non-projected scalar string columns; null
             * rows are guarded by Arrow's validity bitmap. */
            for (int64_t col = 0; col < input_schema.n_children; ++col) {
                const struct ArrowArrayView *field = input_view.children[col];
                if (field->storage_type == NANOARROW_TYPE_STRING &&
                    !ArrowArrayViewIsNull(field, row)) {
                    struct ArrowStringView value = ArrowArrayViewGetStringUnsafe(field, row);
                    if (value.size_bytes < 0 ||
                        !arrow_utf8_valid(value.data, (size_t)value.size_bytes)) {
                        output_batch.release(&output_batch);
                        batch.release(&batch);
                        arrow_runtime_error(error, MILENA_ERR_DATA,
                                            "Arrow UTF-8 field contains invalid text");
                        status = MILENA_ERR_DATA;
                        goto cleanup;
                    }
                }
            }
            if (!arrow_filter_matches(options, &input_view, filter_index, row)) continue;
            for (size_t col = 0; col < options->projection_count; ++col) {
                ArrowErrorCode append_status = arrow_append_value(
                    output_batch.children[col], input_view.children[projection_indices[col]], row);
                if (append_status != NANOARROW_OK) {
                    output_batch.release(&output_batch);
                    batch.release(&batch);
                    arrow_runtime_error(error, MILENA_ERR_MEMORY,
                                        "Could not append projected Arrow value");
                    status = MILENA_ERR_MEMORY;
                    goto cleanup;
                }
            }
            if (ArrowArrayFinishElement(&output_batch) != NANOARROW_OK) {
                output_batch.release(&output_batch);
                batch.release(&batch);
                arrow_runtime_error(error, MILENA_ERR_DATA,
                                    "Could not finish projected Arrow row");
                status = MILENA_ERR_DATA;
                goto cleanup;
            }
            total_output_rows++;
        }
        if (output_batch.length > 0) {
            struct ArrowArrayView output_view = {0};
            bool output_view_live = false;
            if (ArrowArrayFinishBuildingDefault(&output_batch, &arrow_error) != NANOARROW_OK ||
                ArrowArrayViewInitFromSchema(&output_view, &output_schema, &arrow_error) != NANOARROW_OK) {
                if (output_view.children) ArrowArrayViewReset(&output_view);
                output_batch.release(&output_batch);
                batch.release(&batch);
                arrow_runtime_error(error, MILENA_ERR_DATA,
                                    "Could not finalize projected Arrow batch");
                status = MILENA_ERR_DATA;
                goto cleanup;
            }
            output_view_live = true;
            if (ArrowArrayViewSetArray(&output_view, &output_batch, &arrow_error) != NANOARROW_OK ||
                ArrowIpcWriterWriteArrayView(&writer, &output_view, &arrow_error) != NANOARROW_OK) {
                if (output_view_live) ArrowArrayViewReset(&output_view);
                output_batch.release(&output_batch);
                batch.release(&batch);
                arrow_runtime_error(error, MILENA_ERR_IO,
                                    "Could not write projected Arrow record batch");
                status = MILENA_ERR_IO;
                goto cleanup;
            }
            output_batches++;
            ArrowArrayViewReset(&output_view);
        }
        output_batch.release(&output_batch);
        batch.release(&batch);
    }
    if (arrow_cancelled(options)) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Arrow IPC operation cancelled");
        status = MILENA_ERR_IO;
        goto cleanup;
    }
    if (options->max_elapsed_milliseconds > 0.0 &&
        arrow_now_ms() - start_ms > options->max_elapsed_milliseconds) {
        arrow_runtime_error(error, MILENA_ERR_OVERFLOW,
                            "Arrow IPC time budget exceeded");
        status = MILENA_ERR_OVERFLOW;
        goto cleanup;
    }
    if (fgetc(input_file) != EOF || ferror(input_file)) {
        arrow_runtime_error(error, MILENA_ERR_DATA,
                            "Arrow IPC STREAM has trailing bytes or an input I/O error");
        status = MILENA_ERR_DATA;
        goto cleanup;
    }
    if (ArrowIpcWriterWriteArrayView(&writer, NULL, &arrow_error) != NANOARROW_OK) {
        arrow_runtime_error(error, MILENA_ERR_IO, "Could not finish Arrow IPC STREAM");
        status = MILENA_ERR_IO;
        goto cleanup;
    }
    status = MILENA_OK;

cleanup:
    if (writer_live) {
        ArrowIpcWriterReset(&writer);
        writer_live = false;
    }
    if (reader_live) {
        if (reader.release) reader.release(&reader);
        reader_live = false;
    }
    if (input_schema_live && input_schema.release) input_schema.release(&input_schema);
    if (output_schema_live && output_schema.release) output_schema.release(&output_schema);
    if (input_view_live) ArrowArrayViewReset(&input_view);
    if (input_file && !input_file_closed) {
        if (fclose(input_file) != 0 && status == MILENA_OK) {
            arrow_runtime_error(error, MILENA_ERR_IO, "Could not close Arrow input file");
            status = MILENA_ERR_IO;
        }
        input_file_closed = true;
    }
    if (output_file && !output_file_closed) {
        if (status == MILENA_OK && fflush(output_file) != 0) {
            arrow_runtime_error(error, MILENA_ERR_IO, "Could not flush staged Arrow output");
            status = MILENA_ERR_IO;
        }
        if (fclose(output_file) != 0 && status == MILENA_OK) {
            arrow_runtime_error(error, MILENA_ERR_IO, "Could not close staged Arrow output");
            status = MILENA_ERR_IO;
        }
        output_file_closed = true;
    }
    if (status == MILENA_OK) {
        if (!staging_path || !arrow_publish_staged(staging_path, options->output_path)) {
            arrow_runtime_error(error, MILENA_ERR_IO,
                                "Could not atomically publish staged Arrow output");
            status = MILENA_ERR_IO;
        }
    }
    if (status != MILENA_OK && staging_path) (void)remove(staging_path);
    if (staging_path) free(staging_path);
    if (report && status == MILENA_OK) {
        report->input_rows = total_input_rows;
        report->output_rows = total_output_rows;
        report->input_batches = input_batches;
        report->output_batches = output_batches;
        report->input_bytes = input_size;
        report->output_bytes = bounded_output.bytes_written;
    }
    return status;
}
