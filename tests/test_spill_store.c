#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "spill_store.h"

#include <assert.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

    /* Exclusive creation refuses existing paths without modifying user data. */
    const char *exclusive_path = "milena-test-spill-exclusive.bin";
    (void)remove(exclusive_path);
    MilenaSpillStore exclusive = {0};
    assert(milena_spill_store_create_exclusive(exclusive_path, 1024, 128,
                                               &exclusive, &error) == MILENA_OK);
    assert(milena_spill_store_append(&exclusive, "owned", 5, &error) == MILENA_OK);
    assert(milena_spill_store_close(&exclusive, &error) == MILENA_OK);
#ifndef _WIN32
    struct stat exclusive_info;
    assert(stat(exclusive_path, &exclusive_info) == 0);
    assert((exclusive_info.st_mode & 0077) == 0);
    const char *symlink_target = "milena-test-spill-target.bin";
    const char *symlink_path = "milena-test-spill-link.bin";
    FILE *target_file = fopen(symlink_target, "wb");
    assert(target_file != NULL);
    assert(fwrite("keep", 1, 4, target_file) == 4);
    assert(fclose(target_file) == 0);
    (void)remove(symlink_path);
    assert(symlink(symlink_target, symlink_path) == 0);
    assert(milena_spill_store_create_exclusive(symlink_path, 1024, 128,
                                               &exclusive, &error) == MILENA_ERR_ARGUMENT);
    assert(file_size(symlink_target) == 4);
    assert(remove(symlink_path) == 0);
    assert(remove(symlink_target) == 0);
#endif
    size_t exclusive_size = file_size(exclusive_path);
    assert(milena_spill_store_create_exclusive(exclusive_path, 1024, 128,
                                               &exclusive, &error) == MILENA_ERR_ARGUMENT);
    assert(file_size(exclusive_path) == exclusive_size);
    (void)remove(exclusive_path);

    (void)remove(path);
    puts("spill store tests passed");
    return 0;
}
