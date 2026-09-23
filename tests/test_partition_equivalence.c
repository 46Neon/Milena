#include "partition_executor.h"

#include <assert.h>

static double value_at(size_t position) {
    return (double)((position % 97u) + 1u) / 7.0;
}

static MilenaStatus sum_partition(const MilenaPartition *partition,
                                  size_t worker_index, void *context,
                                  MilenaError *error) {
    (void)worker_index;
    (void)error;
    double *sum = (double *)context;
    for (size_t i = 0; i < partition->length_bytes; i++) {
        *sum += value_at(partition->offset_bytes + i);
    }
    return MILENA_OK;
}

int main(void) {
    const size_t input_bytes = 10003;
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(input_bytes, 257, 64,
                                      &plan, &error) == MILENA_OK);

    double monolithic = 0.0;
    for (size_t i = 0; i < input_bytes; i++) monolithic += value_at(i);
    double partitioned = 0.0;
    MilenaPartitionExecutionReport report = {0};
    assert(milena_partition_execute_local(
               &plan, NULL, sum_partition, &partitioned, &report,
               &error) == MILENA_OK);
    assert(report.partitions_completed == plan.partition_count);
    assert(partitioned == monolithic);
    milena_physical_plan_release(&plan);
    puts("partition equivalence tests passed");
    return 0;
}
