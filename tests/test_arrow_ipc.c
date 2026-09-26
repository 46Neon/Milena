#include "arrow_ipc.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
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

struct TestBodyAllocationBudget {
    size_t byte_limit;
    size_t bytes_live;
    size_t peak_bytes_live;
    size_t max_request;
    size_t allocation_calls;
    size_t rejected_requests;
};

static uint8_t *test_body_budget_reallocate(
    struct ArrowBufferAllocator *allocator, uint8_t *ptr,
    int64_t old_size, int64_t new_size) {
    struct TestBodyAllocationBudget *budget =
        (struct TestBodyAllocationBudget *)allocator->private_data;
    if (!budget || old_size < 0 || new_size < 0 ||
        (uint64_t)old_size > budget->bytes_live ||
        (uint64_t)new_size > budget->byte_limit ||
        (size_t)new_size > budget->byte_limit -
                               (budget->bytes_live - (size_t)old_size)) {
        if (budget) budget->rejected_requests++;
        return NULL;
    }

    uint8_t *result = NULL;
    if (new_size > 0) {
        result = (uint8_t *)malloc((size_t)new_size);
        if (!result) return NULL;
        if (ptr && old_size > 0) {
            size_t copy_bytes = (size_t)old_size;
            if (copy_bytes > (size_t)new_size) copy_bytes = (size_t)new_size;
            memcpy(result, ptr, copy_bytes);
        }
    }
    free(ptr);
    budget->bytes_live -= (size_t)old_size;
    budget->bytes_live += (size_t)new_size;
    if ((size_t)new_size > budget->max_request)
        budget->max_request = (size_t)new_size;
    if (budget->bytes_live > budget->peak_bytes_live)
        budget->peak_bytes_live = budget->bytes_live;
    if (new_size > 0) budget->allocation_calls++;
    return result;
}

static void test_body_budget_free(struct ArrowBufferAllocator *allocator,
                                  uint8_t *ptr, int64_t size) {
    struct TestBodyAllocationBudget *budget =
        (struct TestBodyAllocationBudget *)allocator->private_data;
    if (!ptr) return;
    free(ptr);
    if (budget && size >= 0 && (uint64_t)size <= budget->bytes_live)
        budget->bytes_live -= (size_t)size;
    else if (budget)
        budget->bytes_live = 0;
}

static bool write_bytes(const char *path, const void *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(bytes, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static uint16_t test_read_u16_le(const unsigned char *data) {
    return (uint16_t)((uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8));
}

static uint32_t test_read_u32_le(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t test_read_u64_le(const unsigned char *data) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8u; ++i)
        value |= (uint64_t)data[i] << (i * 8u);
    return value;
}

static void test_write_u64_le(unsigned char *data, uint64_t value) {
    for (unsigned int i = 0; i < 8u; ++i)
        data[i] = (unsigned char)(value >> (i * 8u));
}

/* Copy a valid fixture but change only the first RecordBatch Message.bodyLength.
 * Its physical body remains unchanged, making it a hostile declaration that
 * must be rejected before body storage is allocated or any body bytes are read. */
static bool make_oversized_declared_batch(const char *source,
                                          const char *destination,
                                          uint64_t declared_body_size,
                                          size_t *body_offset_out) {
    FILE *file = fopen(source, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long file_size_long = ftell(file);
    if (file_size_long <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t file_size = (size_t)file_size_long;
    unsigned char *bytes = (unsigned char *)malloc(file_size);
    if (!bytes) { fclose(file); return false; }
    bool ok = fread(bytes, 1u, file_size, file) == file_size;
    if (fclose(file) != 0) ok = false;

    size_t offset = 0;
    bool mutated = false;
    while (ok && offset <= file_size && file_size - offset >= 8u) {
        uint32_t continuation = test_read_u32_le(bytes + offset);
        uint32_t metadata_size = test_read_u32_le(bytes + offset + 4u);
        offset += 8u;
        if (continuation != UINT32_MAX || metadata_size == 0u ||
            (size_t)metadata_size > file_size - offset) break;
        unsigned char *metadata = bytes + offset;
        size_t metadata_length = (size_t)metadata_size;
        uint32_t root = test_read_u32_le(metadata);
        if ((size_t)root > metadata_length || metadata_length - (size_t)root < 4u)
            break;
        size_t table = (size_t)root;
        uint32_t raw_delta = test_read_u32_le(metadata + table);
        int64_t vtable_delta = raw_delta <= INT32_MAX ? (int64_t)raw_delta :
            (int64_t)raw_delta - INT64_C(4294967296);
        size_t vtable;
        if (vtable_delta > 0) {
            if ((uint64_t)vtable_delta > table) break;
            vtable = table - (size_t)vtable_delta;
        } else if (vtable_delta < 0) {
            uint64_t forward_delta = (uint64_t)(-vtable_delta);
            if (forward_delta > metadata_length - table) break;
            vtable = table + (size_t)forward_delta;
        } else {
            break;
        }
        if (metadata_length - vtable < 8u) break;
        uint16_t vtable_size = test_read_u16_le(metadata + vtable);
        if (vtable_size < 8u || vtable_size > metadata_length - vtable) break;
        uint16_t type_offset = test_read_u16_le(metadata + vtable + 6u);
        uint16_t body_offset = vtable_size >= 12u ?
            test_read_u16_le(metadata + vtable + 10u) : 0u;
        if (type_offset != 0u && body_offset != 0u &&
            (size_t)type_offset < metadata_length - table &&
            (size_t)body_offset <= metadata_length - table &&
            metadata_length - table - (size_t)body_offset >= 8u &&
            metadata[table + type_offset] ==
                (unsigned char)NANOARROW_IPC_MESSAGE_TYPE_RECORD_BATCH) {
            test_write_u64_le(metadata + table + body_offset, declared_body_size);
            if (body_offset_out) *body_offset_out = offset + metadata_length;
            mutated = true;
            break;
        }
        if (body_offset != 0u && (size_t)body_offset <= metadata_length - table &&
            metadata_length - table - (size_t)body_offset >= 8u) {
            uint64_t body_size = test_read_u64_le(metadata + table + body_offset);
            if (body_size > (uint64_t)(file_size - offset - metadata_length)) break;
            offset += (size_t)body_size;
        }
        offset += metadata_length;
    }

    if (ok && mutated) ok = write_bytes(destination, bytes, file_size);
    else ok = false;
    free(bytes);
    return ok;
}

static bool write_sentinel(const char *path) {
    static const char sentinel[] = "existing-destination-must-survive";
    return write_bytes(path, sentinel, sizeof(sentinel) - 1u);
}

static bool no_staging_files_for(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    size_t directory_length = slash ? (size_t)(slash - path) : 1u;
    const char *directory_name = slash ? path : ".";
    const char *base_name = slash ? slash + 1 : path;
    char *directory = (char *)malloc(directory_length + 2u);
    size_t prefix_length = strlen(base_name) + sizeof(".part.");
    char *prefix = (char *)malloc(prefix_length);
    if (!directory || !prefix) { free(directory); free(prefix); return false; }
    if (slash) {
        if (directory_length == 0u) {
            directory[0] = '/';
            directory[1] = '\0';
        } else {
            memcpy(directory, directory_name, directory_length);
            directory[directory_length] = '\0';
        }
    } else {
        memcpy(directory, directory_name, 2u);
    }
    (void)snprintf(prefix, prefix_length, "%s.part.", base_name);
    DIR *dir = opendir(directory);
    if (!dir) { free(directory); free(prefix); return false; }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            found = true;
            break;
        }
    }
    if (closedir(dir) != 0) found = true;
    free(directory);
    free(prefix);
    return !found;
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

static bool write_shared_text_fixture(const char *path) {
    static const char malformed[] = {(char)0xc0, (char)0xaf};
    FILE *file = fopen(path, "wb");
    struct ArrowIpcOutputStream output = {0};
    struct ArrowIpcWriter writer = {0};
    struct ArrowSchema schema = {0};
    struct ArrowArray array = {0};
    struct ArrowArrayView view = {0};
    struct ArrowError arrow_error;
    bool output_live = false, writer_live = false;
    bool schema_live = false, array_live = false, view_live = false;
    bool ok = false;
    if (!file) return false;
    ArrowSchemaInit(&schema);
    schema_live = true;
    if (ArrowSchemaSetTypeStruct(&schema, 3) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[0], "tag") != NANOARROW_OK ||
        ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[1], "selected") != NANOARROW_OK ||
        ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[2], "unused") != NANOARROW_OK ||
        ArrowSchemaSetType(schema.children[2], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        goto cleanup;
    if (ArrowArrayInitFromSchema(&array, &schema, &arrow_error) != NANOARROW_OK)
        goto cleanup;
    array_live = true;
    if (ArrowArrayStartAppending(&array) != NANOARROW_OK) goto cleanup;
    const struct ArrowStringView malformed_view = {
        malformed, (int64_t)sizeof(malformed)};
    if (ArrowArrayAppendString(array.children[0], ArrowCharView("keep")) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[1], ArrowCharView("first")) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[2], malformed_view) != NANOARROW_OK ||
        ArrowArrayFinishElement(&array) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[0], malformed_view) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[1], malformed_view) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[2], malformed_view) != NANOARROW_OK ||
        ArrowArrayFinishElement(&array) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[0], ArrowCharView("keep")) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[1], ArrowCharView("last")) != NANOARROW_OK ||
        ArrowArrayAppendString(array.children[2], malformed_view) != NANOARROW_OK ||
        ArrowArrayFinishElement(&array) != NANOARROW_OK ||
        ArrowArrayFinishBuildingDefault(&array, &arrow_error) != NANOARROW_OK)
        goto cleanup;
    if (ArrowArrayViewInitFromSchema(&view, &schema, &arrow_error) != NANOARROW_OK)
        goto cleanup;
    view_live = true;
    if (ArrowArrayViewSetArray(&view, &array, &arrow_error) != NANOARROW_OK ||
        ArrowIpcOutputStreamInitFile(&output, file, 1) != NANOARROW_OK)
        goto cleanup;
    output_live = true;
    file = NULL;
    if (ArrowIpcWriterInit(&writer, &output) != NANOARROW_OK) goto cleanup;
    writer_live = true;
    output_live = false; /* writer owns the output stream */
    if (ArrowIpcWriterWriteSchema(&writer, &schema, &arrow_error) != NANOARROW_OK ||
        ArrowIpcWriterWriteArrayView(&writer, &view, &arrow_error) != NANOARROW_OK ||
        ArrowIpcWriterWriteArrayView(&writer, NULL, &arrow_error) != NANOARROW_OK)
        goto cleanup;
    ok = true;
cleanup:
    if (view_live) ArrowArrayViewReset(&view);
    if (array_live && array.release) array.release(&array);
    if (schema_live && schema.release) schema.release(&schema);
    if (writer_live) ArrowIpcWriterReset(&writer);
    else if (output_live && output.release) output.release(&output);
    if (file && fclose(file) != 0) ok = false;
    return ok;
}

static bool verify_shared_text_output(const char *path) {
    FILE *file = fopen(path, "rb");
    struct ArrowIpcInputStream input = {0};
    struct ArrowArrayStream reader = {0};
    struct ArrowSchema schema = {0};
    struct ArrowArrayView view = {0};
    struct ArrowError arrow_error;
    struct ArrowIpcArrayStreamReaderOptions reader_options = {0};
    bool reader_live = false, schema_live = false, view_live = false;
    bool ok = false;
    if (!file) return false;
    reader_options.field_index = -1;
    input.read = test_read;
    input.release = test_input_release;
    input.private_data = file;
    if (ArrowIpcArrayStreamReaderInit(&reader, &input, &reader_options) != NANOARROW_OK)
        goto cleanup;
    reader_live = true;
    if (!reader.get_schema || reader.get_schema(&reader, &schema) != NANOARROW_OK)
        goto cleanup;
    schema_live = schema.release != NULL;
    if (schema.n_children != 2 ||
        strcmp(schema.children[0]->name, "selected") != 0 ||
        strcmp(schema.children[1]->name, "tag") != 0 ||
        ArrowArrayViewInitFromSchema(&view, &schema, &arrow_error) != NANOARROW_OK)
        goto cleanup;
    view_live = true;
    struct ArrowArray batch = {0};
    if (reader.get_next(&reader, &batch) != NANOARROW_OK || !batch.release ||
        batch.length != 2 ||
        ArrowArrayViewSetArray(&view, &batch, &arrow_error) != NANOARROW_OK)
        goto cleanup_batch;
    {
        struct ArrowStringView first = ArrowArrayViewGetStringUnsafe(view.children[0], 0);
        struct ArrowStringView second = ArrowArrayViewGetStringUnsafe(view.children[0], 1);
        struct ArrowStringView first_tag = ArrowArrayViewGetStringUnsafe(view.children[1], 0);
        struct ArrowStringView second_tag = ArrowArrayViewGetStringUnsafe(view.children[1], 1);
        if (first.size_bytes != 5 || memcmp(first.data, "first", 5u) != 0 ||
            second.size_bytes != 4 || memcmp(second.data, "last", 4u) != 0 ||
            first_tag.size_bytes != 4 || memcmp(first_tag.data, "keep", 4u) != 0 ||
            second_tag.size_bytes != 4 || memcmp(second_tag.data, "keep", 4u) != 0)
            goto cleanup_batch;
    }
    batch.release(&batch);
    if (reader.get_next(&reader, &batch) != NANOARROW_OK || batch.release)
        goto cleanup_batch;
    ok = true;
cleanup_batch:
    if (batch.release) batch.release(&batch);
cleanup:
    if (view_live) ArrowArrayViewReset(&view);
    if (schema_live && schema.release) schema.release(&schema);
    if (reader_live && reader.release) reader.release(&reader);
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
    struct ArrowIpcArrayStreamReaderOptions reader_options = {0};
    reader_options.field_index = -1;
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
    CHECK(no_staging_files_for(options.output_path),
          "failed transform left an Arrow staging file behind");
    return true;
}

static bool test_shared_text_utf8_semantics(void) {
    static const char *const selected_projection[] = {"selected", "tag"};
    static const char *const unused_projection[] = {"unused"};
    static const MilenaArrowValueType text_types[] = {
        MILENA_ARROW_VALUE_TEXTO, MILENA_ARROW_VALUE_TEXTO
    };
    const char *input = "tests/arrow-shared-text-input.stream";
    const char *output = "tests/arrow-shared-text-output.stream";
    const char *failure = "tests/arrow-shared-text-failure.stream";
    CHECK(write_shared_text_fixture(input), "could not construct invalid-UTF8 test STREAM");

    MilenaArrowIpcOptions options = default_options(input, output,
        selected_projection, text_types, 2u);
    options.filter_kind = MILENA_ARROW_FILTER_TEXT_EQUAL;
    options.filter_column = "tag";
    options.filter_column_type = MILENA_ARROW_VALUE_TEXTO;
    options.filter_text = "keep";
    options.utf8_validation_policy = MILENA_ARROW_UTF8_VALIDATE_PROJECTED_RESULTS;
    CHECK(run_transform(&options),
          "shared text filter rejected invalid UTF-8 in filtered/unprojected source rows");
    CHECK(verify_shared_text_output(output),
          "shared Arrow result lost projection order or source row order");
    (void)remove(output);

    options.utf8_validation_policy = MILENA_ARROW_UTF8_VALIDATE_ALL_INPUT;
    options.output_path = failure;
    CHECK(expect_failure_preserves(options, MILENA_ERR_DATA,
          "standalone strict Arrow UTF-8 mode accepted invalid source text"),
          "standalone strict UTF-8 regression failed");

    options.projection = unused_projection;
    options.projection_count = 1u;
    options.utf8_validation_policy = MILENA_ARROW_UTF8_VALIDATE_PROJECTED_RESULTS;
    CHECK(expect_failure_preserves(options, MILENA_ERR_DATA,
          "shared Arrow mode returned invalid projected text"),
          "returned-projection UTF-8 regression failed");
    (void)remove(input);
    (void)remove(output);
    (void)remove(failure);
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

static bool test_oversized_reader_preallocation(const char *path,
                                               size_t byte_limit,
                                               size_t expected_body_offset) {
    FILE *file = fopen(path, "rb");
    struct ArrowIpcInputStream input = {0};
    struct ArrowArrayStream reader = {0};
    struct ArrowSchema schema = {0};
    struct ArrowArray batch = {0};
    struct ArrowIpcArrayStreamReaderOptions reader_options = {-1, 1};
    struct TestBodyAllocationBudget budget = {0};
    bool reader_live = false;
    bool schema_live = false;
    bool ok = false;

    if (!file) return false;
    budget.byte_limit = byte_limit;
    input.read = test_read;
    input.release = test_input_release;
    input.private_data = file;
    if (ArrowIpcArrayStreamReaderInit(&reader, &input, &reader_options) != NANOARROW_OK) {
        fprintf(stderr, "FALLO Arrow IPC: hostile reader initialization failed\n");
        goto cleanup;
    }
    reader_live = true;
    struct ArrowBufferAllocator body_allocator = {
        &test_body_budget_reallocate, &test_body_budget_free, &budget
    };
    if (ArrowIpcArrayStreamReaderSetBodyAllocationLimit(
            &reader, (int64_t)byte_limit, body_allocator) != NANOARROW_OK) {
        fprintf(stderr, "FALLO Arrow IPC: hostile allocator installation failed\n");
        goto cleanup;
    }
    if (!reader.get_schema || reader.get_schema(&reader, &schema) != NANOARROW_OK) {
        fprintf(stderr, "FALLO Arrow IPC: hostile fixture schema was not readable\n");
        goto cleanup;
    }
    schema_live = schema.release != NULL;
    int next_status = reader.get_next(&reader, &batch);
    const char *message = reader.get_last_error ? reader.get_last_error(&reader) : NULL;
    if (next_status == NANOARROW_OK || batch.release || !message ||
        strstr(message, "record batch body exceeds configured byte limit") == NULL) {
        fprintf(stderr, "FALLO Arrow IPC: oversized declared body was not rejected before decode\n");
        goto cleanup;
    }
    if (budget.allocation_calls != 0u || budget.rejected_requests != 0u ||
        budget.max_request > byte_limit || budget.peak_bytes_live > byte_limit ||
        budget.bytes_live != 0u) {
        fprintf(stderr, "FALLO Arrow IPC: body allocator exceeded or attempted to exceed cap\n");
        goto cleanup;
    }
    long position_after_header = ftell(file);
    if (position_after_header < 0 ||
        (uint64_t)position_after_header != (uint64_t)expected_body_offset) {
        fprintf(stderr, "FALLO Arrow IPC: reader consumed hostile batch-body bytes\n");
        goto cleanup;
    }
    ok = true;

cleanup:
    if (batch.release) batch.release(&batch);
    if (schema_live && schema.release) schema.release(&schema);
    if (reader_live && reader.release) reader.release(&reader);
    if (fclose(file) != 0) ok = false;
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

    const char *hostile = "tests/arrow-hostile-declared-size.stream";
    const size_t body_limit = 2048u;
    size_t hostile_body_offset = 0u;
    CHECK(make_oversized_declared_batch(PRIMITIVE_FIXTURE, hostile,
                                        (uint64_t)body_limit + 1u,
                                        &hostile_body_offset),
          "could not create hostile oversized declared-batch fixture");
    CHECK(test_oversized_reader_preallocation(hostile, body_limit,
                                               hostile_body_offset),
          "nanoarrow allocated before enforcing the declared-batch cap");
    options.input_path = hostile;
    options.max_batch_bytes = body_limit;
    CHECK(expect_failure_preserves(options, MILENA_ERR_OVERFLOW,
                                   "oversized declared Arrow body was accepted"),
          "pre-allocation batch cap/staging cleanup test failed");
    remove(hostile);
    options.input_path = PRIMITIVE_FIXTURE;
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
    if (argc == 3 && strcmp(argv[1], "--write-shared-fixture") == 0)
        return write_shared_text_fixture(argv[2]) ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--verify-shared-text") == 0)
        return verify_shared_text_output(argv[2]) ? 0 : 1;
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--verify-primitive|--verify-text|--verify-wide|--verify-shared-text FILE|--write-shared-fixture FILE]\n", argv[0]);
        return 2;
    }
    remove("tests/arrow-primitive-output.stream");
    remove("tests/arrow-text-output.stream");
    remove("tests/arrow-wide-output.stream");
    if (!test_primitive_backend("tests/arrow-primitive-output.stream")) return 1;
    if (!test_text_backend("tests/arrow-text-output.stream")) return 1;
    if (!test_exact_int64_backend("tests/arrow-wide-output.stream")) return 1;
    if (!test_shared_text_utf8_semantics()) return 1;
    if (!test_failures()) return 1;
    remove("tests/arrow-primitive-output.stream");
    remove("tests/arrow-text-output.stream");
    remove("tests/arrow-wide-output.stream");
    puts("OK: Arrow IPC backend tests");
    return 0;
}
