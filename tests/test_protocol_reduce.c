#include "partition_protocol_reduce.h"

#include <assert.h>
#include <math.h>

static void encode(double value, size_t id, unsigned char *messages) {
    MilenaPartitionResult result = {id, MILENA_OK, true, value};
    MilenaError error;
    size_t written = 0;
    milena_error_clear(&error);
    assert(milena_partition_result_encode(
               &result, messages + id * MILENA_PARTITION_RESULT_WIRE_SIZE,
               MILENA_PARTITION_RESULT_WIRE_SIZE, &written, &error) == MILENA_OK);
}

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(300, 100, 4, &plan, &error) == MILENA_OK);
    unsigned char messages[3 * MILENA_PARTITION_RESULT_WIRE_SIZE];
    encode(1.0, 0, messages);
    encode(2.0, 1, messages);
    encode(3.0, 2, messages);
    /* Arrival order may differ; each wire slot is still independently decoded. */
    unsigned char unordered[3 * MILENA_PARTITION_RESULT_WIRE_SIZE];
    memcpy(unordered, messages + 2 * MILENA_PARTITION_RESULT_WIRE_SIZE,
           MILENA_PARTITION_RESULT_WIRE_SIZE);
    memcpy(unordered + MILENA_PARTITION_RESULT_WIRE_SIZE, messages,
           MILENA_PARTITION_RESULT_WIRE_SIZE);
    memcpy(unordered + 2 * MILENA_PARTITION_RESULT_WIRE_SIZE,
           messages + MILENA_PARTITION_RESULT_WIRE_SIZE,
           MILENA_PARTITION_RESULT_WIRE_SIZE);
    double result = 0.0;
    assert(milena_partition_results_reduce_sum(&plan, unordered, 3,
                                               &result, &error) == MILENA_OK);
    assert(fabs(result - 6.0) < 1e-12);
    memcpy(unordered + 2 * MILENA_PARTITION_RESULT_WIRE_SIZE,
           unordered + MILENA_PARTITION_RESULT_WIRE_SIZE,
           MILENA_PARTITION_RESULT_WIRE_SIZE);
    assert(milena_partition_results_reduce_sum(&plan, unordered, 3,
                                               &result, &error) == MILENA_ERR_DATA);
    milena_physical_plan_release(&plan);
    puts("protocol reduction tests passed");
    return 0;
}
