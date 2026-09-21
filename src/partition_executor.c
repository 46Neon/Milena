#include "partition_executor.h"

static void executor_error(MilenaError *error, MilenaStatus status,
                           const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

MilenaPartitionExecutorOptions milena_partition_executor_options_default(void) {
    MilenaPartitionExecutorOptions options = {1u, true};
    return options;
}

MilenaStatus milena_partition_execute_local(
    const MilenaPhysicalPlan *plan,
    const MilenaPartitionExecutorOptions *requested,
    MilenaPartitionWorker worker,
    void *context,
    MilenaPartitionExecutionReport *report,
    MilenaError *error) {
    MilenaPartitionExecutorOptions defaults =
        milena_partition_executor_options_default();
    const MilenaPartitionExecutorOptions *options = requested ? requested : &defaults;
    if (!plan || !worker || options->max_workers == 0 || options->max_workers > 1) {
        executor_error(error, MILENA_ERR_ARGUMENT,
                       "El ejecutor local requiere un único worker determinista");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_physical_plan_validate(plan, error);
    if (status != MILENA_OK) return status;
    if (report) {
        memset(report, 0, sizeof(*report));
        report->partitions_total = plan->partition_count;
        report->workers_used = plan->partition_count > 0 ? 1u : 0u;
        report->deterministic_order = true;
    }
    if (error) milena_error_clear(error);
    for (size_t i = 0; i < plan->partition_count; i++) {
        status = worker(&plan->partitions[i], 0, context, error);
        if (status != MILENA_OK) {
            if (report) report->partitions_failed++;
            if (options->fail_fast) return status;
            continue;
        }
        if (report) report->partitions_completed++;
    }
    if (report && report->partitions_failed > 0) {
        executor_error(error, MILENA_ERR_DATA,
                       "Una o más particiones no pudieron ejecutarse");
        return MILENA_ERR_DATA;
    }
    return MILENA_OK;
}
