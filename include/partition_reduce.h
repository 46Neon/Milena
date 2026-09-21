#ifndef MILENA_PARTITION_REDUCE_H
#define MILENA_PARTITION_REDUCE_H

#include "partition_plan.h"

typedef struct {
    size_t partition_id;
    double value;
    bool valid;
} MilenaPartitionPartial;

/* Reduces one partial per partition in stable partition-id order. */
MilenaStatus milena_partition_reduce_sum(
    const MilenaPhysicalPlan *plan,
    const MilenaPartitionPartial *partials,
    size_t partial_count,
    double *result,
    MilenaError *error);

#endif
