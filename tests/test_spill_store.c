#include "spill_store.h"

#include <assert.h>

typedef struct { size_t count; size_t bytes; } VisitState;

static MilenaStatus count_record(const void *data, size_t length,
                                 size_t record_index, void *context,
                                 MilenaError *error) {
    (void)error;
    VisitState *state = (VisitState *)context;
    assert(data != NULL);
    assert(record_index == state->count);
    state->count++;
    state->bytes += length;
    return MILENA_OK;
}

int main(void) {
    const char *path = "milena-test-spill.bin";
    (void)remove(path);
    MilenaError error;
    MilenaSpillStore store = {0};
    milena_error_clear(&error);
    assert(milena_spill_store_open(path, 1024, 128, &store, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "uno", 3, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "dos", 3, &error) == MILENA_OK);
    assert(milena_spill_store_close(&store, &error) == MILENA_OK);

    MilenaSpillRecovery recovery;
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 2);
    assert(!recovery.truncated_tail);
    VisitState visited = {0};
    assert(milena_spill_store_visit(path, 1024, 128, count_record,
                                    &visited, &error) == MILENA_OK);
    assert(visited.count == 2 && visited.bytes == 6);

    FILE *file = fopen(path, "ab");
    assert(file != NULL);
    assert(fputc('x', file) != EOF);
    assert(fclose(file) == 0);
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 2);
    assert(recovery.truncated_tail);
    assert(recovery.bytes_discarded == 1);

    file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, (long)(MILENA_SPILL_HEADER_SIZE + 1), SEEK_SET) == 0);
    assert(fputc('X', file) != EOF);
    assert(fclose(file) == 0);
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 1);
    assert(recovery.truncated_tail);

    assert(milena_spill_store_open(path, 80, 128, &store, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "cuatro", 6, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, "12345678901234567890", 20, &error) == MILENA_ERR_OVERFLOW);
    assert(milena_spill_store_close(&store, &error) == MILENA_OK);
    (void)remove(path);
    puts("spill store tests passed");
    return 0;
}
