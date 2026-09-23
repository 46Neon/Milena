#include "partition_plan.h"

static void partition_error(MilenaError *error, MilenaStatus status,
                            const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

void milena_physical_plan_init(MilenaPhysicalPlan *plan) {
    if (!plan) return;
    plan->input_bytes = 0;
    plan->target_partition_bytes = 0;
    plan->partition_count = 0;
    plan->partitions = NULL;
}

void milena_physical_plan_release(MilenaPhysicalPlan *plan) {
    if (!plan) return;
    free(plan->partitions);
    milena_physical_plan_init(plan);
}

MilenaStatus milena_physical_plan_validate(const MilenaPhysicalPlan *plan,
                                           MilenaError *error) {
    if (!plan || (plan->partition_count > 0 && !plan->partitions)) {
        partition_error(error, MILENA_ERR_ARGUMENT,
                        "El plan físico de particiones es inválido");
        return MILENA_ERR_ARGUMENT;
    }
    if (plan->partition_count > MILENA_PARTITION_MAX_COUNT ||
        plan->target_partition_bytes == 0) {
        partition_error(error, MILENA_ERR_OVERFLOW,
                        "El plan físico supera los límites de particionado");
        return MILENA_ERR_OVERFLOW;
    }
    size_t expected_offset = 0;
    for (size_t i = 0; i < plan->partition_count; i++) {
        const MilenaPartition *partition = &plan->partitions[i];
        if (partition->id != i || partition->offset_bytes != expected_offset ||
            partition->length_bytes == 0 ||
            partition->length_bytes > plan->target_partition_bytes ||
            !milena_size_add(partition->offset_bytes,
                             partition->length_bytes, &expected_offset)) {
            partition_error(error, MILENA_ERR_DATA,
                            "Las particiones no cubren el origen de forma contigua");
            return MILENA_ERR_DATA;
        }
    }
    if (expected_offset != plan->input_bytes) {
        partition_error(error, MILENA_ERR_DATA,
                        "El plan físico no cubre exactamente el origen");
        return MILENA_ERR_DATA;
    }
    return MILENA_OK;
}

MilenaStatus milena_physical_plan_build(size_t input_bytes,
                                         size_t target_partition_bytes,
                                         size_t max_partitions,
                                         MilenaPhysicalPlan *plan,
                                         MilenaError *error) {
    if (!plan || target_partition_bytes == 0 || max_partitions == 0 ||
        max_partitions > MILENA_PARTITION_MAX_COUNT) {
        partition_error(error, MILENA_ERR_ARGUMENT,
                        "Los límites del plan físico son obligatorios");
        return MILENA_ERR_ARGUMENT;
    }
    milena_physical_plan_release(plan);
    plan->input_bytes = input_bytes;
    plan->target_partition_bytes = target_partition_bytes;
    if (input_bytes == 0) return MILENA_OK;
    size_t count = input_bytes / target_partition_bytes;
    if (input_bytes % target_partition_bytes != 0) count++;
    if (count > max_partitions) {
        partition_error(error, MILENA_ERR_OVERFLOW,
                        "El origen requiere más particiones que el máximo permitido");
        milena_physical_plan_release(plan);
        return MILENA_ERR_OVERFLOW;
    }
    plan->partitions = (MilenaPartition *)calloc(count, sizeof(*plan->partitions));
    if (!plan->partitions) {
        partition_error(error, MILENA_ERR_MEMORY,
                        "Memoria insuficiente para el plan físico");
        milena_physical_plan_release(plan);
        return MILENA_ERR_MEMORY;
    }
    plan->partition_count = count;
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        size_t remaining = input_bytes - offset;
        size_t length = remaining < target_partition_bytes ? remaining : target_partition_bytes;
        plan->partitions[i].id = i;
        plan->partitions[i].offset_bytes = offset;
        plan->partitions[i].length_bytes = length;
        offset += length;
    }
    MilenaStatus status = milena_physical_plan_validate(plan, error);
    if (status != MILENA_OK) milena_physical_plan_release(plan);
    return status;
}
