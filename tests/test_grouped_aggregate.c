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
    char keys[8][16];
    MilenaAggregateResult results[8];
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
    assert(capture->count < 8);
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
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "z", 1, 8.0, &error));
    CHECK_OK(milena_grouped_aggregate_add(&grouped, "", 0, 5.0, &error));
    Capture capture = {0};
    size_t emitted = 0;
    CHECK_OK(milena_grouped_aggregate_finalize(&grouped, capture_result,
                                                &capture, &emitted, &error));
    assert(emitted == 4 && capture.count == 4);
    assert(strcmp(capture.keys[0], "") == 0);
    assert(strcmp(capture.keys[1], "a") == 0);
    assert(strcmp(capture.keys[2], "b") == 0);
    assert(strcmp(capture.keys[3], "z") == 0);
    assert(capture.results[0].count == 1 && capture.results[0].sum == 5.0);
    assert(capture.results[1].count == 2 && capture.results[1].sum == 4.0);
    assert(capture.results[2].count == 2 && capture.results[2].sum == 8.0);
    assert(capture.results[3].count == 2 && capture.results[3].sum == 12.0);
    assert(milena_grouped_aggregate_add(&grouped, "x", 1, 1.0, &error) == MILENA_ERR_ARGUMENT);
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
    CHECK_OK(milena_grouped_aggregate_open(path, 2048, 64, 100,
                                            &grouped, &error));
    for (size_t i = 0; i < grouped.group_capacity; ++i) {
        char key = (char)('a' + i);
        CHECK_OK(milena_grouped_aggregate_add(&grouped, &key, 1, (double)i, &error));
    }
    assert(milena_grouped_aggregate_add(&grouped, "z", 1, 9.0, &error) == MILENA_ERR_OVERFLOW);
    (void)milena_grouped_aggregate_close(&grouped, NULL);
    (void)remove(path);

    /* Reject budgets that cannot hold even one group. */
    assert(milena_grouped_aggregate_open(path, 1, 8, 1024, &grouped, &error) == MILENA_ERR_ARGUMENT);
    puts("grouped aggregate spill tests: ok");
    return 0;
}
