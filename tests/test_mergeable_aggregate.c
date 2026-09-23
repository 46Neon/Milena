#include "mergeable_aggregate.h"
#include "spill_store.h"

#include <assert.h>

static MilenaStatus merge_record(const void *data, size_t length,
                                 size_t index, void *context,
                                 MilenaError *error) {
    (void)index;
    MilenaAggregateState partial;
    MilenaStatus status = milena_aggregate_state_decode(data, length, &partial, error);
    if (status != MILENA_OK) return status;
    return milena_aggregate_state_merge((MilenaAggregateState *)context,
                                        &partial, error);
}

int main(void) {
    MilenaError error;
    milena_error_clear(&error);
    MilenaAggregateState left, right, serial;
    milena_aggregate_state_init(&left);
    milena_aggregate_state_init(&right);
    milena_aggregate_state_init(&serial);
    const double values[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    for (size_t i = 0; i < 3; i++) assert(milena_aggregate_state_add(&left, values[i], &error) == MILENA_OK);
    for (size_t i = 3; i < 6; i++) assert(milena_aggregate_state_add(&right, values[i], &error) == MILENA_OK);
    for (size_t i = 0; i < 6; i++) assert(milena_aggregate_state_add(&serial, values[i], &error) == MILENA_OK);
    assert(milena_aggregate_state_merge(&left, &right, &error) == MILENA_OK);
    assert(left.count == serial.count && left.sum == serial.sum &&
           left.sum_compensation == serial.sum_compensation);
    assert(fabs(left.mean - serial.mean) < 1e-12);
    assert(fabs(left.m2 - serial.m2) < 1e-12);
    assert(left.min == 1.0 && left.max == 6.0);
    MilenaAggregateResult finalized;
    assert(milena_aggregate_state_finalize(&left, &finalized, &error) == MILENA_OK);
    assert(finalized.count == 6 && finalized.has_values && finalized.has_sample_variance);
    assert(fabs(finalized.variance_population - (35.0 / 12.0)) < 1e-12);
    assert(fabs(finalized.variance_sample - 3.5) < 1e-12);
    assert(fabs(finalized.stddev_sample - sqrt(3.5)) < 1e-12);
    MilenaAggregateState singleton; milena_aggregate_state_init(&singleton);
    assert(milena_aggregate_state_add(&singleton, 4.0, &error) == MILENA_OK);
    assert(milena_aggregate_state_finalize(&singleton, &finalized, &error) == MILENA_OK);
    assert(finalized.has_values && !finalized.has_sample_variance && finalized.mean == 4.0);
    MilenaAggregateState empty; milena_aggregate_state_init(&empty);
    assert(milena_aggregate_state_finalize(&empty, &finalized, &error) == MILENA_OK);
    assert(!finalized.has_values && finalized.count == 0);

    /* Cancellation-heavy partial states retain their low-order bits through
     * merge and versioned serialization (the exact sum is 2, not 0). */
    const double cancellation_left[] = {1e16, 1.0};
    const double cancellation_right[] = {1.0, -1e16};
    MilenaAggregateState cancel_a, cancel_b;
    milena_aggregate_state_init(&cancel_a);
    milena_aggregate_state_init(&cancel_b);
    for (size_t i = 0; i < 2; ++i) {
        assert(milena_aggregate_state_add(&cancel_a, cancellation_left[i], &error) == MILENA_OK);
        assert(milena_aggregate_state_add(&cancel_b, cancellation_right[i], &error) == MILENA_OK);
    }
    assert(cancel_a.sum_compensation != 0.0);
    unsigned char wire[MILENA_AGGREGATE_WIRE_SIZE]; size_t written = 0;
    MilenaAggregateState decoded;
    assert(milena_aggregate_state_encode(&cancel_a, wire, sizeof(wire), &written, &error) == MILENA_OK);
    assert(milena_aggregate_state_decode(wire, written, &decoded, &error) == MILENA_OK);
    assert(decoded.sum == cancel_a.sum &&
           decoded.sum_compensation == cancel_a.sum_compensation);
    assert(milena_aggregate_state_merge(&decoded, &cancel_b, &error) == MILENA_OK);
    assert(milena_aggregate_state_finalize(&decoded, &finalized, &error) == MILENA_OK);
    assert(finalized.count == 4 && finalized.sum == 2.0);

    assert(milena_aggregate_state_encode(&left, wire, sizeof(wire), &written, &error) == MILENA_OK);
    assert(written == sizeof(wire));
    assert(milena_aggregate_state_decode(wire, written, &decoded, &error) == MILENA_OK);
    assert(decoded.count == left.count && decoded.sum == left.sum &&
           decoded.sum_compensation == left.sum_compensation &&
           decoded.mean == left.mean && decoded.m2 == left.m2);
    wire[20] ^= 1u;
    assert(milena_aggregate_state_decode(wire, written, &decoded, &error) == MILENA_ERR_DATA);

    const char *path = "milena-aggregate-spill.bin";
    (void)remove(path);
    MilenaSpillStore store = {0};
    assert(milena_spill_store_open(path, 1024, MILENA_AGGREGATE_WIRE_SIZE,
                                   &store, &error) == MILENA_OK);
    assert(milena_aggregate_state_encode(&left, wire, sizeof(wire), &written, &error) == MILENA_OK);
    assert(milena_spill_store_append(&store, wire, written, &error) == MILENA_OK);
    assert(milena_spill_store_close(&store, &error) == MILENA_OK);
    MilenaAggregateState replayed; milena_aggregate_state_init(&replayed);
    assert(milena_spill_store_visit(path, 1024, MILENA_AGGREGATE_WIRE_SIZE,
                                    merge_record, &replayed, &error) == MILENA_OK);
    assert(replayed.count == left.count && replayed.sum == left.sum);
    assert(fabs(replayed.m2 - left.m2) < 1e-12);
    (void)remove(path);
    puts("mergeable aggregate tests passed");
    return 0;
}
