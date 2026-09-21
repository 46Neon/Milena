#include "partition_executor.h"

#include <assert.h>

static MilenaStatus collect_partition(const MilenaPartition *partition,
                                      size_t worker_index, void *context,
                                      MilenaError *error) {
    (void)error;
    size_t *next = (size_t *)context;
    assert(worker_index == 0);
    assert(partition->id == *next);
    (*next)++;
    return MILENA_OK;
}

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(1024, 256, 8, &plan, &error) == MILENA_OK);

    size_t next = 0;
    MilenaPartitionExecutionReport report = {0};
    MilenaPartitionExecutorOptions options =
        milena_partition_executor_options_default();
    assert(milena_partition_execute_local(&plan, &options,
                                          collect_partition, &next,
                                          &report, &error) == MILENA_OK);
    assert(next == plan.partition_count);
    assert(report.partitions_total == 4);
    assert(report.partitions_completed == 4);
    assert(report.partitions_failed == 0);
    assert(report.workers_used == 1);
    assert(report.deterministic_order);

    options.max_workers = 2;
    assert(milena_partition_execute_local(&plan, &options,
                                          collect_partition, &next,
                                          &report, &error) == MILENA_ERR_ARGUMENT);
    milena_physical_plan_release(&plan);
    puts("partition executor tests passed");
    return 0;
}
