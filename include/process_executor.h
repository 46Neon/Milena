#ifndef MILENA_PROCESS_EXECUTOR_H
#define MILENA_PROCESS_EXECUTOR_H

#include "partition_executor.h"

typedef struct {
    size_t partitions_total;
    size_t partitions_completed;
    size_t partitions_failed;
    bool process_isolation;
    bool unsupported_platform;
} MilenaProcessExecutionReport;

/* Runs one validated partition per isolated local process via a pipe contract. */
MilenaStatus milena_partition_execute_processes(
    const MilenaPhysicalPlan *plan,
    MilenaPartitionWorker worker,
    void *context,
    MilenaProcessExecutionReport *report,
    MilenaError *error);

#endif
