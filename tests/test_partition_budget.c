#include "partition_executor.h"

#include <assert.h>

static MilenaStatus noop_worker(const MilenaPartition *partition,
                                size_t worker_index, void *context,
                                MilenaError *error) {
    (void)partition;
    (void)worker_index;
    (void)context;
    (void)error;
    return MILENA_OK;
}

static bool cancel_after_one_claim(void *context) {
    size_t *checks = (size_t *)context;
    return (*checks)++ >= 1;
}

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(1024, 256, 8, &plan, &error) == MILENA_OK);

    MilenaPartitionExecutorOptions options =
        milena_partition_executor_options_default();
    options.max_workers = 2;
    options.max_bytes_per_worker = 1024;
    MilenaPartitionExecutionReport report = {0};
    assert(milena_partition_execute_local(&plan, &options, noop_worker,
                                          NULL, &report, &error) == MILENA_OK);
    assert(report.bytes_assigned == 1024);

    options.max_bytes_per_worker = 128;
    assert(milena_partition_execute_local(&plan, &options, noop_worker,
                                          NULL, &report, &error) != MILENA_OK);
    assert(strstr(error.message, "bytes") != NULL);

    size_t checks = 0;
    options.max_bytes_per_worker = 0;
    options.cancel = cancel_after_one_claim;
    assert(milena_partition_execute_local(&plan, &options, noop_worker,
                                          &checks, &report, &error) != MILENA_OK);
    assert(report.cancelled);
    milena_physical_plan_release(&plan);
    puts("partition budget tests passed");
    return 0;
}
