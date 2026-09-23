#include "partition_reduce.h"

static void reduce_error(MilenaError *error, MilenaStatus status,
                         const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

MilenaStatus milena_partition_reduce_sum(
    const MilenaPhysicalPlan *plan,
    const MilenaPartitionPartial *partials,
    size_t partial_count,
    double *result,
    MilenaError *error) {
    if (!plan || !result || (partial_count > 0 && !partials) ||
        partial_count != plan->partition_count) {
        reduce_error(error, MILENA_ERR_ARGUMENT,
                     "La reducción requiere una parcial por partición");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_physical_plan_validate(plan, error);
    if (status != MILENA_OK) return status;
    double sum = 0.0;
    double compensation = 0.0;
    for (size_t i = 0; i < partial_count; i++) {
        if (partials[i].partition_id != i) {
            reduce_error(error, MILENA_ERR_DATA,
                         "Las parciales no están ordenadas por partición");
            return MILENA_ERR_DATA;
        }
        if (!partials[i].valid) continue;
        if (!isfinite(partials[i].value)) {
            reduce_error(error, MILENA_ERR_DATA,
                         "Una parcial contiene un valor no finito");
            return MILENA_ERR_DATA;
        }
        double corrected = partials[i].value - compensation;
        double next = sum + corrected;
        compensation = (next - sum) - corrected;
        sum = next;
    }
    *result = sum;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}
