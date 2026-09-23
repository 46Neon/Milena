#include "grouped_aggregate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK_OK(expr) do { \
    MilenaStatus check_status = (expr); \
    if (check_status != MILENA_OK) { \
        fprintf(stderr, "unexpected status %s at %s:%d: %s\n", \
                milena_status_name(check_status), __FILE__, __LINE__, error.message); \
        assert(check_status == MILENA_OK); \
    } \
} while (0)

typedef struct {
    char keys[256][32];
    MilenaAggregateResult results[256];
    size_t count;
} Capture;

typedef struct { char previous[16]; size_t count; } OrderCheck;

static MilenaStatus check_order(const MilenaGroupedAggregateResult *result,
                                void *context, MilenaError *error) {
    (void)error;
    OrderCheck *check = context;
    assert(result->key_length < sizeof(check->previous));
    if (check->count != 0) {
        size_t common = result->key_length < strlen(check->previous) ?
            result->key_length : strlen(check->previous);
        int order = memcmp(check->previous, result->key, common);
        assert(order < 0 || (order == 0 && strlen(check->previous) < result->key_length));
    }
    memcpy(check->previous, result->key, result->key_length);
    check->previous[result->key_length] = '\0';
    check->count++;
    return MILENA_OK;
}

static MilenaStatus capture_result(const MilenaGroupedAggregateResult *result,
                                   void *context, MilenaError *error) {
    Capture *capture = context;
    (void)error;
    assert(capture->count < 256);
    assert(result->key_length < sizeof(capture->keys[0]));
    memcpy(capture->keys[capture->count], result->key, result->key_length);
    capture->keys[capture->count][result->key_length] = '\0';
    capture->results[capture->count] = result->aggregate;
    capture->count++;
    return MILENA_OK;
}

int main(void) {
    const char *path = "tests/grouped-aggregate.spill";
    remove(path);
    MilenaError error;
    milena_error_init(&error);
    MilenaGroupedAggregate grouped;
    /* This budget fits only a few resident keys and forces multiple spills. */
    CHECK_OK(milena_grouped_aggregate_open(path, 2048, 64, 1024u * 1024u,
                                            &grouped, &error));
    assert(grouped.group_capacity < 8);
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "z", 1, 4.0, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "b", 1, 2.0, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "a", 1, 1.0, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "b", 1, 6.0, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "a", 1, 3.0, &error));
    CHECK_OK(milena_grouped_aggregate_add_null(&grouped, "a", 1, &error));
    CHECK_OK(milena_grouped_aggregate_add_invalid(&grouped, "a", 1, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "z", 1, 8.0, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "", 0, 5.0, &error));
    CHECK_OK(milena_grouped_aggregate_add_null(&grouped, "n", 1, &error));
    CHECK_OK(milena_grouped_aggregate_add_invalid(&grouped, "n", 1, &error));
    Capture capture = {0};
    size_t emitted = 0;
    CHECK_OK(milena_grouped_aggregate_finalize(&grouped, capture_result,
                                                &capture, &emitted, &error));
    assert(emitted == 5 && capture.count == 5);
    assert(strcmp(capture.keys[0], "") == 0);
    assert(strcmp(capture.keys[1], "a") == 0);
    assert(strcmp(capture.keys[2], "b") == 0);
    assert(strcmp(capture.keys[3], "n") == 0);
    assert(strcmp(capture.keys[4], "z") == 0);
    assert(capture.results[0].count == 1 && capture.results[0].sum == 5.0);
    assert(capture.results[1].count == 2 && capture.results[1].sum == 4.0 &&
           capture.results[1].null_count == 1 && capture.results[1].invalid_count == 1);
    assert(capture.results[2].count == 2 && capture.results[2].sum == 8.0);
    assert(capture.results[3].count == 0 && capture.results[3].null_count == 1 &&
           capture.results[3].invalid_count == 1 && !capture.results[3].has_values);
    assert(capture.results[4].count == 2 && capture.results[4].sum == 12.0);
    assert(milena_grouped_aggregate_add(&grouped, "x", 1, 1.0, &error) == MILENA_ERR_ARGUMENT);
    CHECK_OK(milena_grouped_aggregate_close(&grouped, &error));
    assert(remove(path) == 0);

    /* Exact typed INT64 states merge across spill runs; all-null groups keep
     * their explicit counters and empty value state. */
    CHECK_OK(milena_grouped_aggregate_open(path, 2048, 64, 1024u * 1024u,
                                            &grouped, &error));
    CHECK_OK(milena_grouped_aggregate_add_int64(&grouped, "large", 5,
        INT64_C(9007199254740993), &error));
    CHECK_OK(milena_grouped_aggregate_add_int64(&grouped, "large", 5, 1, &error));
    CHECK_OK(milena_grouped_aggregate_add_null(&grouped, "empty", 5, &error));
    for (size_t i = 0; i < 12; ++i) {
        char key[16];
        (void)snprintf(key, sizeof(key), "i%02zu", i);
        CHECK_OK(milena_grouped_aggregate_add_int64(&grouped, key, strlen(key),
                                                     (int64_t)i, &error));
    }
    CHECK_OK(milena_grouped_aggregate_add_int64(&grouped, "large", 5, 1, &error));
    Capture integer_capture = {0};
    CHECK_OK(milena_grouped_aggregate_finalize(&grouped, capture_result,
        &integer_capture, &emitted, &error));
    assert(emitted == 14 && integer_capture.count == 14);
    bool found_large = false, found_empty = false;
    for (size_t i = 0; i < integer_capture.count; ++i) {
        if (strcmp(integer_capture.keys[i], "large") == 0) {
            found_large = true;
            assert(integer_capture.results[i].has_integer_sum &&
                   integer_capture.results[i].integer_sum == INT64_C(9007199254740995));
        }
        if (strcmp(integer_capture.keys[i], "empty") == 0) {
            found_empty = true;
            assert(integer_capture.results[i].count == 0 &&
                   integer_capture.results[i].null_count == 1 &&
                   !integer_capture.results[i].has_values);
        }
    }
    assert(found_large && found_empty);
    CHECK_OK(milena_grouped_aggregate_close(&grouped, &error));
    assert(remove(path) == 0);

    FILE *stale = fopen(path, "wb");
    assert(stale != NULL);
    assert(fclose(stale) == 0);
    assert(milena_grouped_aggregate_open(path, 2048, 64, 1024, &grouped, &error) == MILENA_ERR_ARGUMENT);
    assert(remove(path) == 0);

    /* Many distinct keys force multiple external merge passes rather than a
     * spill rescan per output group. The callback remains deterministic. */
    CHECK_OK(milena_grouped_aggregate_open(path, 2048, 64, 1024u * 1024u,
                                            &grouped, &error));
    for (size_t i = 0; i < 120; ++i) {
        char key[16];
        (void)snprintf(key, sizeof(key), "key%03zu", i);
        CHECK_OK(milena_grouped_aggregate_add(&grouped, key, strlen(key),
                                               (double)i, &error));
    }
    OrderCheck order = {{0}, 0};
    CHECK_OK(milena_grouped_aggregate_finalize(&grouped, check_order, &order,
                                                &emitted, &error));
    assert(emitted == 120 && order.count == 120);
    CHECK_OK(milena_grouped_aggregate_close(&grouped, &error));
    assert(remove(path) == 0);

    /* A tiny spill quota must fail instead of exceeding its configured limit. */
    CHECK_OK(milena_grouped_aggregate_open(path, 2048, 64, 150,
                                            &grouped, &error));
    for (size_t i = 0; i < grouped.group_capacity; ++i) {
        char key = (char)('a' + i);
        CHECK_OK(milena_grouped_aggregate_add(&grouped, &key, 1, (double)i, &error));
    }
    assert(milena_grouped_aggregate_add(&grouped, "z", 1, 9.0, &error) == MILENA_ERR_OVERFLOW);
    assert(grouped.failed && grouped.failure_status == MILENA_ERR_OVERFLOW);
    size_t persisted_prefix = grouped.spill.record_count;
    assert(persisted_prefix > 0 && persisted_prefix < grouped.group_capacity);
    /* Retrying either operation must not append the unflushed map again. */
    assert(milena_grouped_aggregate_add(&grouped, "z", 1, 9.0, &error) == MILENA_ERR_OVERFLOW);
    assert(milena_grouped_aggregate_finalize(&grouped, check_order, NULL,
                                               &emitted, &error) == MILENA_ERR_OVERFLOW);
    assert(grouped.spill.record_count == persisted_prefix);
    (void)milena_grouped_aggregate_close(&grouped, NULL);
    (void)remove(path);

    /* A configured run cap fails before creating an unbounded run set and
     * cleanup removes the owned spill file. */
    CHECK_OK(milena_grouped_aggregate_open_with_max_runs(path, 2048, 64,
        1024u * 1024u, 1u, &grouped, &error));
    for (size_t i = 0; i < 120; ++i) {
        char key[16];
        (void)snprintf(key, sizeof(key), "cap%03zu", i);
        CHECK_OK(milena_grouped_aggregate_add(&grouped, key, strlen(key),
                                               (double)i, &error));
    }
    assert(milena_grouped_aggregate_finalize(&grouped, check_order, NULL,
        &emitted, &error) == MILENA_ERR_OVERFLOW);
    (void)milena_grouped_aggregate_close(&grouped, NULL);
    char run_path[256];
    (void)snprintf(run_path, sizeof(run_path), "%s.group.p0.r0", path);
    assert(fopen(run_path, "rb") == NULL);
    (void)remove(path);

    /* Reject budgets that cannot hold even one group. */
    assert(milena_grouped_aggregate_open(path, 1, 8, 1024, &grouped, &error) == MILENA_ERR_ARGUMENT);
    puts("grouped aggregate spill tests: ok");
    return 0;
}
