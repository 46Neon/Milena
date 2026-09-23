#ifndef MILENA_EXTERNAL_SORT_H
#define MILENA_EXTERNAL_SORT_H

#include "external_merge.h"

typedef MilenaStatus (*MilenaDoubleSourceFn)(void *context, double *value,
                                             bool *has_value,
                                             MilenaError *error);

/*
 * Stream numeric values into sorted spill runs and merge them in bounded-fan-in
 * passes. RAM is O(run_capacity + max_fan_in); generated runs are removed after
 * successful completion. Both per-file and aggregate scratch quotas are enforced.
 */
MilenaStatus milena_external_sort_doubles(
    MilenaDoubleSourceFn source, void *source_context,
    const char *run_prefix, const char *output_path,
    size_t run_capacity, size_t max_fan_in,
    size_t per_file_quota_bytes, size_t total_scratch_quota_bytes,
    MilenaError *error);

#endif
