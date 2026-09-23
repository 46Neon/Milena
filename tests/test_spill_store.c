#include "spill_store.h"

#include <assert.h>

typedef struct {
    size_t count;
    size_t bytes;
    const char *expected[3];
    size_t lengths[3];
} VisitState;

static MilenaStatus verify_record(const void *data, size_t length,
                                  size_t record_index, void *context,
                                  MilenaError *error) {
    (void)error;
    VisitState *state = (VisitState *)context;
    assert(record_index == state->count);
    assert(record_index < 3);
    assert(length == state->lengths[record_index]);
    if (length == 0) {
        assert(data == NULL);
    } else {
        assert(data != NULL);
        assert(memcmp(data, state->expected[record_index], length) == 0);
    }
    state->count++;
    state->bytes += length;
    return MILENA_OK;
}

static MilenaStatus fail_visit(const void *data, size_t length,
                               size_t record_index, void *context,
                               MilenaError *error) {
    (void)data;
    (void)length;
    (void)record_index;
    (void)context;
    (void)error;
    return MILENA_ERR_DATA;
}

static size_t file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    assert(fclose(file) == 0);
    return (size_t)size;
}

int main(void) {
    const char *path = "milena-test-spill.bin";
    (void)remove(path);
    MilenaError error;
    MilenaSpillStore store = {0};
    milena_error_clear(&error);
    assert(milena_spill_store_open(path, 1024, 128, &store, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "uno", 3, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, NULL, 0, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "dos", 3, &error) == MILENA_OK);
    assert(milena_spill_store_close(&store, &error) == MILENA_OK);

    MilenaSpillRecovery recovery;
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 3);
    assert(!recovery.truncated_tail);

    /* A lower quota must reject the valid existing store without trimming it. */
    size_t original_size = file_size(path);
    assert(milena_spill_store_recover(path, 23, 128, &recovery, &error) == MILENA_ERR_OVERFLOW);
    assert(file_size(path) == original_size);
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 3 && !recovery.truncated_tail);

    VisitState visited = {0, 0, {"uno", "", "dos"}, {3, 0, 3}};
    assert(milena_spill_store_visit(path, 1024, 128, verify_record,
                                    &visited, &error) == MILENA_OK);
    assert(visited.count == 3 && visited.bytes == 6);
    assert(milena_spill_store_visit(path, 1024, 128, fail_visit,
                                    NULL, &error) == MILENA_ERR_DATA);

    FILE *file = fopen(path, "ab");
    assert(file != NULL);
    assert(fputc('x', file) != EOF);
    assert(fclose(file) == 0);
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 3);
    assert(recovery.truncated_tail);
    assert(recovery.bytes_discarded == 1);

    file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, (long)(MILENA_SPILL_HEADER_SIZE + 1), SEEK_SET) == 0);
    assert(fputc('X', file) != EOF);
    assert(fclose(file) == 0);
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 0);
    assert(recovery.truncated_tail);

    assert(milena_spill_store_open(path, 60, 128, &store, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "cuatro", 6, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "12345678901234567890", 20, &error) == MILENA_ERR_OVERFLOW);
    assert(milena_spill_store_close(&store, &error) == MILENA_OK);
    (void)remove(path);
    puts("spill store tests passed");
    return 0;
}
