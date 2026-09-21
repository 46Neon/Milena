#include "partition_executor.h"

#include <assert.h>

static MilenaStatus record_partition(const MilenaPartition *partition,
                                     size_t worker_index, void *context,
                                     MilenaError *error) {
    (void)worker_index;
    (void)error;
    size_t *seen = (size_t *)context;
    /* Each partition owns one slot, so workers do not share mutable slots. */
    seen[partition->id] = partition->length_bytes;
    return MILENA_OK;
}

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(4096, 256, 32, &plan, &error) == MILENA_OK);
    size_t seen[16] = {0};
    MilenaPartitionExecutorOptions options =
        milena_partition_executor_options_default();
    options.max_workers = 4;
    MilenaPartitionExecutionReport report = {0};
    assert(milena_partition_execute_local(&plan, &options, record_partition,
                                          seen, &report, &error) == MILENA_OK);
    assert(report.workers_used == 4);
    assert(report.partitions_completed == plan.partition_count);
    for (size_t i = 0; i < plan.partition_count; i++) assert(seen[i] > 0);
    milena_physical_plan_release(&plan);
    puts("partition concurrency tests passed");
    return 0;
}
