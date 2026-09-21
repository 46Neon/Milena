#ifndef MILENA_PARTITION_EXECUTOR_H
#define MILENA_PARTITION_EXECUTOR_H

#include "partition_plan.h"

typedef MilenaStatus (*MilenaPartitionWorker)(const MilenaPartition *partition,
                                               size_t worker_index,
                                               void *context,
                                               MilenaError *error);

typedef struct {
    size_t max_workers;
    bool fail_fast;
} MilenaPartitionExecutorOptions;

typedef struct {
    size_t partitions_total;
    size_t partitions_completed;
    size_t partitions_failed;
    size_t workers_used;
    bool deterministic_order;
} MilenaPartitionExecutionReport;

MilenaPartitionExecutorOptions milena_partition_executor_options_default(void);

/* Executes validated partitions locally in stable order before network workers exist. */
MilenaStatus milena_partition_execute_local(
    const MilenaPhysicalPlan *plan,
    const MilenaPartitionExecutorOptions *options,
    MilenaPartitionWorker worker,
    void *context,
    MilenaPartitionExecutionReport *report,
    MilenaError *error);

#endif
