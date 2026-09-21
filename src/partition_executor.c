#include "partition_executor.h"

#include <threads.h>

static void executor_error(MilenaError *error, MilenaStatus status,
                           const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

MilenaPartitionExecutorOptions milena_partition_executor_options_default(void) {
    MilenaPartitionExecutorOptions options = {1u, true};
    return options;
}

typedef struct {
    const MilenaPhysicalPlan *plan;
    const MilenaPartitionExecutorOptions *options;
    MilenaPartitionWorker worker;
    void *context;
    MilenaPartitionExecutionReport *report;
    MilenaError first_error;
    size_t next_index;
    mtx_t lock;
    bool failed;
} LocalExecution;

static int local_worker(void *opaque) {
    LocalExecution *execution = (LocalExecution *)opaque;
    for (;;) {
        size_t index;
        if (mtx_lock(&execution->lock) != thrd_success) return -1;
        if (execution->next_index >= execution->plan->partition_count ||
            (execution->failed && execution->options->fail_fast)) {
            (void)mtx_unlock(&execution->lock);
            return 0;
        }
        index = execution->next_index++;
        (void)mtx_unlock(&execution->lock);

        MilenaError worker_error;
        milena_error_clear(&worker_error);
        MilenaStatus status = execution->worker(
            &execution->plan->partitions[index], index, execution->context,
            &worker_error);
        if (mtx_lock(&execution->lock) != thrd_success) return -1;
        if (status != MILENA_OK) {
            execution->failed = true;
            if (execution->first_error.code == MILENA_OK) {
                execution->first_error = worker_error;
            }
            execution->report->partitions_failed++;
        } else {
            execution->report->partitions_completed++;
        }
        (void)mtx_unlock(&execution->lock);
    }
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
    if (!plan || !worker || options->max_workers == 0 ||
        options->max_workers > MILENA_PARTITION_MAX_COUNT) {
        executor_error(error, MILENA_ERR_ARGUMENT,
                       "El ejecutor local requiere un número válido de workers");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_physical_plan_validate(plan, error);
    if (status != MILENA_OK) return status;
    MilenaPartitionExecutionReport local_report = {0};
    MilenaPartitionExecutionReport *effective_report = report ? report : &local_report;
    memset(effective_report, 0, sizeof(*effective_report));
    effective_report->partitions_total = plan->partition_count;
    effective_report->workers_used = plan->partition_count == 0 ? 0u :
        (options->max_workers < plan->partition_count ?
         options->max_workers : plan->partition_count);
    effective_report->deterministic_order = true;
    if (error) milena_error_clear(error);
    if (plan->partition_count == 0) return MILENA_OK;
#if defined(__STDC_NO_THREADS__)
    if (options->max_workers > 1) {
        executor_error(error, MILENA_ERR_UNSUPPORTED,
                       "El runtime no ofrece threads C11 para ejecución concurrente");
        return MILENA_ERR_UNSUPPORTED;
    }
    LocalExecution execution = {plan, options, worker, context, effective_report,
                                {0}, 0, {0}, false};
    milena_error_clear(&execution.first_error);
    return local_worker(&execution) == 0 ? MILENA_OK : MILENA_ERR_INTERNAL;
#else
    LocalExecution execution = {plan, options, worker, context, effective_report,
                                {0}, 0, {0}, false};
    milena_error_clear(&execution.first_error);
    if (mtx_init(&execution.lock, mtx_plain) != thrd_success) {
        executor_error(error, MILENA_ERR_INTERNAL,
                       "No se pudo inicializar el coordinador local");
        return MILENA_ERR_INTERNAL;
    }
    size_t worker_count = effective_report->workers_used;
    thrd_t threads[MILENA_PARTITION_MAX_COUNT];
    size_t created = 0;
    for (; created < worker_count; created++) {
        if (thrd_create(&threads[created], local_worker, &execution) != thrd_success) {
            execution.failed = true;
            executor_error(error, MILENA_ERR_INTERNAL,
                           "No se pudo crear un worker local");
            break;
        }
    }
    for (size_t i = 0; i < created; i++) (void)thrd_join(threads[i], NULL);
    mtx_destroy(&execution.lock);
    if (created != worker_count) return MILENA_ERR_INTERNAL;
    if (execution.failed) {
        if (error && execution.first_error.code != MILENA_OK) *error = execution.first_error;
        else executor_error(error, MILENA_ERR_DATA,
                            "Una o más particiones no pudieron ejecutarse");
        return MILENA_ERR_DATA;
    }
    return MILENA_OK;
#endif
}
