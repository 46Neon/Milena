#include "arrow_ipc.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FALLO Arrow IPC: %s\n", (message)); \
            return false; \
        } \
    } while (0)

#define PRIMITIVE_FIXTURE "tests/fixtures/arrow_ipc/generated_primitive_cpp21.stream"
#define BINARY_FIXTURE "tests/fixtures/arrow_ipc/generated_binary_cpp21.stream"
#define WIDE_FIXTURE "tests/fixtures/arrow_ipc/int64_above_2_53_pyarrow23.stream"

static ArrowErrorCode test_read(struct ArrowIpcInputStream *stream,
                                uint8_t *buffer, int64_t requested,
                                int64_t *read_out, struct ArrowError *error) {
    FILE *file = (FILE *)stream->private_data;
    *read_out = 0;
    if (!file || requested < 0 || (uint64_t)requested > SIZE_MAX) {
        (void)ArrowErrorSet(error, "Invalid test reader request");
        return EINVAL;
    }
    size_t count = fread(buffer, 1u, (size_t)requested, file);
    *read_out = (int64_t)count;
    if (count != (size_t)requested && ferror(file)) {
        (void)ArrowErrorSet(error, "Test fixture read failed");
        return EIO;
    }
    return NANOARROW_OK;
}

static void test_input_release(struct ArrowIpcInputStream *stream) {
    if (!stream) return;
    stream->release = NULL;
    stream->private_data = NULL;
}

static bool write_bytes(const char *path, const void *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(bytes, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool write_sentinel(const char *path) {
    static const char sentinel[] = "existing-destination-must-survive";
    return write_bytes(path, sentinel, sizeof(sentinel) - 1u);
}

static bool is_sentinel(const char *path) {
    static const char sentinel[] = "existing-destination-must-survive";
    char data[sizeof(sentinel)];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(data, 1u, sizeof(data), file);
    bool ok = !ferror(file) && count == sizeof(sentinel) - 1u &&
              memcmp(data, sentinel, sizeof(sentinel) - 1u) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static MilenaArrowIpcOptions default_options(
    const char *input, const char *output,
    const char *const *projection, const MilenaArrowValueType *types,
    size_t projection_count) {
    MilenaArrowIpcOptions options;
    memset(&options, 0, sizeof(options));
    options.input_path = input;
    options.output_path = output;
    options.projection = projection;
    options.projection_types = types;
    options.projection_count = projection_count;
    options.max_input_bytes = 64u * 1024u * 1024u;
    options.max_output_bytes = 64u * 1024u * 1024u;
    options.max_rows = 10000000u;
    options.max_batch_rows = 65536u;
    options.max_batch_bytes = 64u * 1024u * 1024u;
    options.max_columns = 128u;
    options.max_elapsed_milliseconds = 30000.0;
    return options;
}

typedef enum {
    VERIFY_PRIMITIVE,
    VERIFY_BINARY_TEXT,
    VERIFY_WIDE_INT64
} VerifyKind;

static bool verify_output(const char *path, VerifyKind kind) {
    FILE *file = NULL;
    struct ArrowIpcInputStream input = {0};
    struct ArrowArrayStream reader = {0};
    struct ArrowSchema schema = {0};
    struct ArrowArrayView view = {0};
    struct ArrowError arrow_error;
    struct ArrowIpcArrayStreamReaderOptions reader_options = {-1, 0};
    size_t batches = 0, rows = 0;
    bool ok = false, reader_live = false, schema_live = false, view_live = false;

    file = fopen(path, "rb");
    CHECK(file != NULL, "could not open output IPC STREAM");
    input.read = test_read;
    input.release = test_input_release;
    input.private_data = file;
    CHECK(ArrowIpcArrayStreamReaderInit(&reader, &input, &reader_options) == NANOARROW_OK,
          "independent IPC stream reader initialization failed");
    reader_live = true;
    CHECK(reader.get_schema && reader.get_schema(&reader, &schema) == NANOARROW_OK,
          "output schema is not readable");
    schema_live = schema.release != NULL;

    if (kind == VERIFY_PRIMITIVE) {
        CHECK(schema.n_children == 2, "primitive output must contain two projected columns");
        CHECK(strcmp(schema.children[0]->name, "int64_nullable") == 0 &&
              strcmp(schema.children[1]->name, "float64_nullable") == 0,
              "primitive projection order or names changed");
    } else if (kind == VERIFY_BINARY_TEXT) {
        CHECK(schema.n_children == 2, "text output must contain two projected columns");
        CHECK(strcmp(schema.children[0]->name, "utf8_nullable") == 0 &&
              strcmp(schema.children[1]->name, "utf8_nonnullable") == 0,
              "text projection order or names changed");
    } else {
        CHECK(schema.n_children == 1 && strcmp(schema.children[0]->name, "wide") == 0,
              "int64 output schema is incorrect");
    }

    CHECK(ArrowArrayViewInitFromSchema(&view, &schema, &arrow_error) == NANOARROW_OK,
          "could not create output array view");
    view_live = true;
    for (;;) {
        struct ArrowArray batch = {0};
        int next_status = reader.get_next(&reader, &batch);
        CHECK(next_status == 0, "output record batch is malformed or truncated");
        if (!batch.release) break;
        CHECK(ArrowArrayViewSetArray(&view, &batch, &arrow_error) == NANOARROW_OK,
              "could not view output record batch");
        CHECK(ArrowArrayViewValidate(&view, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &arrow_error) == NANOARROW_OK,
              "output record batch failed full validation");
        batches++;
        if (kind == VERIFY_PRIMITIVE) {
            CHECK(schema.children[0]->format[0] == 'l' &&
                  schema.children[1]->format[0] == 'g',
                  "numeric output must retain int64 and float64 types");
            if (batches == 1) {
                CHECK(batch.length == 12, "first filtered batch should contain 12 rows");
                CHECK(ArrowArrayViewGetIntUnsafe(view.children[0], 0) == 2147483647,
                      "first int64 projected value changed");
                CHECK(!ArrowArrayViewIsNull(view.children[0], 0),
                      "first int64 value unexpectedly became NULL");
                CHECK(ArrowArrayViewIsNull(view.children[0], 1),
                      "projected int64 NULL validity was not preserved");
                CHECK(fabs(ArrowArrayViewGetDoubleUnsafe(view.children[1], 0) + 1746.99) < 1e-9,
                      "first float64 projected value changed");
            } else if (batches == 2) {
                CHECK(batch.length == 9, "second filtered batch should contain 9 rows");
                CHECK(ArrowArrayViewIsNull(view.children[0], 0) &&
                      ArrowArrayViewIsNull(view.children[1], 0),
                      "second-batch projected NULL validity was not preserved");
            } else CHECK(false, "unexpected number of primitive output batches");
        } else if (kind == VERIFY_BINARY_TEXT) {
            CHECK(schema.children[0]->format[0] == 'u' &&
                  schema.children[1]->format[0] == 'u',
                  "text output must retain UTF-8 string types");
            if (batches == 1) {
                CHECK(batch.length == 17, "first UTF-8 batch should contain 17 rows");
                CHECK(ArrowArrayViewIsNull(view.children[0], 0) &&
                      ArrowArrayViewIsNull(view.children[0], 1),
                      "nullable UTF-8 validity was not retained");
                CHECK(!ArrowArrayViewIsNull(view.children[0], 2),
                      "valid UTF-8 value became NULL");
                struct ArrowStringView value = ArrowArrayViewGetStringUnsafe(view.children[0], 2);
                static const char expected[] = "r\xC2\xB0rir\xE7\x9F\xA2\xE7\x9F\xA2";
                CHECK(value.size_bytes == (int64_t)(sizeof(expected) - 1u) &&
                      memcmp(value.data, expected, sizeof(expected) - 1u) == 0,
                      "UTF-8 projected bytes changed");
            } else if (batches == 2) {
                CHECK(batch.length == 20, "second UTF-8 batch should contain 20 rows");
            } else CHECK(false, "unexpected number of UTF-8 output batches");
        } else {
            CHECK(schema.children[0]->format[0] == 'l',
                  "wide signed integer output type changed");
            CHECK(batch.length == 2, "only two int64 values should pass the filter");
            CHECK(ArrowArrayViewGetIntUnsafe(view.children[0], 0) == INT64_C(9007199254740993),
                  "int64 value above 2^53 was rounded");
            CHECK(ArrowArrayViewGetIntUnsafe(view.children[0], 1) == INT64_MAX,
                  "INT64_MAX was rounded");
            CHECK(!ArrowArrayViewIsNull(view.children[0], 0) &&
                  !ArrowArrayViewIsNull(view.children[0], 1),
                  "filtered int64 values unexpectedly became NULL");
        }
        rows += (size_t)batch.length;
        batch.release(&batch);
    }
    if (kind == VERIFY_PRIMITIVE) CHECK(rows == 21 && batches == 2,
                                        "primitive output row/batch counts changed");
    else if (kind == VERIFY_BINARY_TEXT) CHECK(rows == 37 && batches == 2,
                                               "UTF-8 output row/batch counts changed");
    else CHECK(rows == 2 && batches == 1,
               "exact int64 output row/batch counts changed");
    ok = true;

    if (view_live) ArrowArrayViewReset(&view);
    if (schema_live && schema.release) schema.release(&schema);
    if (reader_live && reader.release) reader.release(&reader);
    if (file && fclose(file) != 0) ok = false;
    return ok;
}

static bool run_transform(const MilenaArrowIpcOptions *options) {
    MilenaArrowIpcReport report = {0};
    MilenaError error;
    milena_error_clear(&error);
    MilenaStatus status = milena_arrow_ipc_stream_transform(options, &report, &error);
    if (status != MILENA_OK) {
        fprintf(stderr, "Arrow backend failed: %s (status %d)\n",
                error.message, (int)status);
        return false;
    }
    return true;
}

static bool test_primitive_backend(const char *output_path) {
    static const char *const projection[] = {"int64_nullable", "float64_nullable"};
    static const MilenaArrowValueType types[] = {
        MILENA_ARROW_VALUE_NUMERICA, MILENA_ARROW_VALUE_NUMERICA
    };
    MilenaArrowIpcOptions options = default_options(PRIMITIVE_FIXTURE, output_path,
                                                     projection, types, 2);
    options.filter_kind = MILENA_ARROW_FILTER_NUMERIC_GREATER;
    options.filter_column = "int64_nonnullable";
    options.filter_column_type = MILENA_ARROW_VALUE_NUMERICA;
    options.filter_number = 0.0;
    CHECK(run_transform(&options), "primitive backend transform failed");
    CHECK(verify_output(output_path, VERIFY_PRIMITIVE), "primitive output validation failed");
    return true;
}

static bool test_text_backend(const char *output_path) {
    static const char *const projection[] = {"utf8_nullable", "utf8_nonnullable"};
    static const MilenaArrowValueType types[] = {
        MILENA_ARROW_VALUE_TEXTO, MILENA_ARROW_VALUE_TEXTO
    };
    MilenaArrowIpcOptions options = default_options(BINARY_FIXTURE, output_path,
                                                     projection, types, 2);
    CHECK(run_transform(&options), "UTF-8 backend transform failed");
    CHECK(verify_output(output_path, VERIFY_BINARY_TEXT), "UTF-8 output validation failed");
    return true;
}

static bool test_exact_int64_backend(const char *output_path) {
    static const char *const projection[] = {"wide"};
    static const MilenaArrowValueType types[] = {MILENA_ARROW_VALUE_NUMERICA};
    MilenaArrowIpcOptions options = default_options(WIDE_FIXTURE, output_path,
                                                     projection, types, 1);
    options.filter_kind = MILENA_ARROW_FILTER_NUMERIC_GREATER;
    options.filter_column = "wide";
    options.filter_column_type = MILENA_ARROW_VALUE_NUMERICA;
    options.filter_number = 9007199254740992.0;
    CHECK(run_transform(&options), "exact int64 backend transform failed");
    CHECK(verify_output(output_path, VERIFY_WIDE_INT64), "exact int64 output validation failed");
    return true;
}

static bool expect_failure_preserves(MilenaArrowIpcOptions options,
                                    MilenaStatus expected_status,
                                    const char *message) {
    MilenaError error;
    CHECK(write_sentinel(options.output_path), "could not initialize existing destination");
    milena_error_clear(&error);
    MilenaStatus status = milena_arrow_ipc_stream_transform(&options, NULL, &error);
    CHECK(status != MILENA_OK, message);
    if (expected_status != MILENA_OK)
        CHECK(status == expected_status, "failure returned an unexpected status code");
    CHECK(is_sentinel(options.output_path), "failed transform replaced the existing destination");
    return true;
}

static bool cancellation_after_rows(void *context) {
    unsigned int *calls = (unsigned int *)context;
    (*calls)++;
    return *calls >= 3u;
}

static bool slow_callback_for_time_limit(void *context) {
    unsigned int *calls = (unsigned int *)context;
    (*calls)++;
    if (*calls == 2u) {
        clock_t start = clock();
        while (start != (clock_t)-1 &&
               (double)(clock() - start) / (double)CLOCKS_PER_SEC < 0.03) {
            /* Burn a bounded 30 ms in a deterministic test callback. */
        }
    }
    return false;
}

static bool make_truncated_copy(const char *source, const char *destination) {
    FILE *in = fopen(source, "rb");
    if (!in) return false;
    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return false; }
    long length = ftell(in);
    if (length < 64 || fseek(in, 0, SEEK_SET) != 0) { fclose(in); return false; }
    size_t keep = (size_t)length - 32u;
    unsigned char *bytes = (unsigned char *)malloc(keep);
    if (!bytes) { fclose(in); return false; }
    bool ok = fread(bytes, 1u, keep, in) == keep;
    if (fclose(in) != 0) ok = false;
    if (ok) ok = write_bytes(destination, bytes, keep);
    free(bytes);
    return ok;
}

static bool test_failures(void) {
    static const char *const numeric_projection[] = {"int64_nullable"};
    static const MilenaArrowValueType numeric_type[] = {MILENA_ARROW_VALUE_NUMERICA};
    static const char *const bool_projection[] = {"bool_nullable"};
    static const MilenaArrowValueType text_type[] = {MILENA_ARROW_VALUE_TEXTO};
    const char *destination = "tests/arrow-failure-destination.stream";
    MilenaArrowIpcOptions options = default_options(PRIMITIVE_FIXTURE, destination,
                                                     numeric_projection, numeric_type, 1);

    options.max_input_bytes = 8u;
    CHECK(expect_failure_preserves(options, MILENA_ERR_OVERFLOW,
                                   "oversized input file was accepted"), "input cap test failed");
    options.max_input_bytes = 64u * 1024u * 1024u;
    options.max_output_bytes = 1u;
    CHECK(expect_failure_preserves(options, MILENA_OK,
                                   "output byte cap was ignored"), "output cap test failed");
    options.max_output_bytes = 64u * 1024u * 1024u;
    options.max_rows = 2u;
    CHECK(expect_failure_preserves(options, MILENA_ERR_OVERFLOW,
                                   "row limit was ignored"), "row cap test failed");
    options.max_rows = 10000000u;
    options.max_batch_rows = 1u;
    CHECK(expect_failure_preserves(options, MILENA_ERR_OVERFLOW,
                                   "record batch row limit was ignored"), "batch-row cap test failed");
    options.max_batch_rows = 65536u;
    options.max_batch_bytes = 1u;
    CHECK(expect_failure_preserves(options, MILENA_ERR_OVERFLOW,
                                   "decoded batch byte limit was ignored"), "batch-byte cap test failed");
    options.max_batch_bytes = 64u * 1024u * 1024u;
    options.max_columns = 1u;
    CHECK(expect_failure_preserves(options, MILENA_ERR_UNSUPPORTED,
                                   "source column limit was ignored"), "column cap test failed");
    options.max_columns = 128u;

    options.projection = bool_projection;
    options.projection_types = text_type;
    CHECK(expect_failure_preserves(options, MILENA_ERR_TYPE,
                                   "bool was accepted as a Milena texto column"),
          "unsupported selected bool test failed");
    options.projection = numeric_projection;
    options.projection_types = text_type;
    CHECK(expect_failure_preserves(options, MILENA_ERR_TYPE,
                                   "int64 was accepted as a Milena texto column"),
          "schema/declaration mismatch test failed");
    options.projection_types = numeric_type;

    const char *truncated = "tests/arrow-truncated.stream";
    CHECK(make_truncated_copy(PRIMITIVE_FIXTURE, truncated), "could not create truncated IPC input");
    options.input_path = truncated;
    CHECK(expect_failure_preserves(options, MILENA_OK,
                                   "truncated Arrow IPC input was accepted"), "truncation test failed");
    remove(truncated);
    options.input_path = PRIMITIVE_FIXTURE;

    unsigned int cancel_calls = 0;
    options.is_cancelled = cancellation_after_rows;
    options.cancel_context = &cancel_calls;
    CHECK(expect_failure_preserves(options, MILENA_ERR_IO,
                                   "cooperative cancellation was ignored"), "cancellation test failed");
    options.is_cancelled = NULL;
    options.cancel_context = NULL;

    unsigned int slow_calls = 0;
    options.max_elapsed_milliseconds = 1.0;
    options.is_cancelled = slow_callback_for_time_limit;
    options.cancel_context = &slow_calls;
    CHECK(expect_failure_preserves(options, MILENA_ERR_OVERFLOW,
                                   "elapsed-time limit was ignored"), "time cap test failed");
    options.is_cancelled = NULL;
    options.cancel_context = NULL;
    remove(destination);
    return true;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--verify-primitive") == 0)
        return verify_output(argv[2], VERIFY_PRIMITIVE) ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--verify-text") == 0)
        return verify_output(argv[2], VERIFY_BINARY_TEXT) ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--verify-wide") == 0)
        return verify_output(argv[2], VERIFY_WIDE_INT64) ? 0 : 1;
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--verify-primitive|--verify-text|--verify-wide FILE]\n", argv[0]);
        return 2;
    }
    remove("tests/arrow-primitive-output.stream");
    remove("tests/arrow-text-output.stream");
    remove("tests/arrow-wide-output.stream");
    if (!test_primitive_backend("tests/arrow-primitive-output.stream")) return 1;
    if (!test_text_backend("tests/arrow-text-output.stream")) return 1;
    if (!test_exact_int64_backend("tests/arrow-wide-output.stream")) return 1;
    if (!test_failures()) return 1;
    remove("tests/arrow-primitive-output.stream");
    remove("tests/arrow-text-output.stream");
    remove("tests/arrow-wide-output.stream");
    puts("OK: Arrow IPC backend tests");
    return 0;
}
