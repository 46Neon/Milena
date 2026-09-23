#include "process_executor.h"

#include <assert.h>

static MilenaStatus isolated_worker(const MilenaPartition *partition,
                                    size_t worker_index, void *context,
                                    MilenaError *error) {
    (void)partition; (void)worker_index; (void)context; (void)error;
    return MILENA_OK;
}

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(2048, 512, 8, &plan, &error) == MILENA_OK);
    MilenaProcessExecutionReport report = {0};
    MilenaStatus status = milena_partition_execute_processes(
        &plan, isolated_worker, NULL, &report, &error);
#if defined(_WIN32)
    assert(status == MILENA_ERR_UNSUPPORTED);
    assert(report.unsupported_platform);
#else
    assert(status == MILENA_OK);
    assert(report.process_isolation);
    assert(report.partitions_completed == plan.partition_count);
    assert(report.partitions_failed == 0);
#endif
    milena_physical_plan_release(&plan);
    puts("process executor tests passed");
    return 0;
}
