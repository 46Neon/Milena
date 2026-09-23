#ifndef MILENA_EXTERNAL_MERGE_H
#define MILENA_EXTERNAL_MERGE_H

#include "spill_store.h"

/*
 * Merge at most max_fan_in already-sorted spill runs of finite binary64 values.
 * Each input record is exactly one little-endian binary64. Output is a new
 * spill file; existing output paths are never overwritten. RAM is O(max_fan_in).
 */
MilenaStatus milena_external_merge_double_runs(
    const char *const *input_paths, size_t input_count,
    const char *output_path, size_t max_fan_in,
    size_t per_file_quota_bytes, MilenaError *error);

#endif
