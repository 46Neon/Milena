#include "spill_store.h"

#include <assert.h>

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

    FILE *file = fopen(path, "ab");
    assert(file != NULL);
    assert(fputc('x', file) != EOF);
    assert(fclose(file) == 0);
    assert(milena_spill_store_recover(path, 1024, 128, &recovery, &error) == MILENA_OK);
    assert(recovery.records_recovered == 2);
    assert(recovery.truncated_tail);
    assert(recovery.bytes_discarded == 1);

    assert(milena_spill_store_open(path, 30, 128, &store, &error) == MILENA_ERR_OVERFLOW);
    (void)remove(path);
    puts("spill store tests passed");
    return 0;
}
