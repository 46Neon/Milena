#ifndef MILENA_PARTITION_PROTOCOL_REDUCE_H
#define MILENA_PARTITION_PROTOCOL_REDUCE_H

#include "partition_protocol.h"

/* Decodes unordered wire results, rejects duplicates/missing partitions and reduces deterministically. */
MilenaStatus milena_partition_results_reduce_sum(
    const MilenaPhysicalPlan *plan,
    const unsigned char *messages,
    size_t message_count,
    double *result,
    MilenaError *error);

#endif
