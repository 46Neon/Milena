#ifndef MILENA_PARTITION_PLAN_H
#define MILENA_PARTITION_PLAN_H

#include "common.h"

#define MILENA_PARTITION_MAX_COUNT 4096u

typedef struct {
    size_t id;
    size_t offset_bytes;
    size_t length_bytes;
} MilenaPartition;

typedef struct {
    size_t input_bytes;
    size_t target_partition_bytes;
    size_t partition_count;
    MilenaPartition *partitions;
} MilenaPhysicalPlan;

void milena_physical_plan_init(MilenaPhysicalPlan *plan);
void milena_physical_plan_release(MilenaPhysicalPlan *plan);

/* Creates contiguous local partitions. It does not claim CSV record boundaries. */
MilenaStatus milena_physical_plan_build(size_t input_bytes,
                                         size_t target_partition_bytes,
                                         size_t max_partitions,
                                         MilenaPhysicalPlan *plan,
                                         MilenaError *error);

/* Validates coverage, ordering, non-overlap and stable identifiers. */
MilenaStatus milena_physical_plan_validate(const MilenaPhysicalPlan *plan,
                                           MilenaError *error);

#endif
