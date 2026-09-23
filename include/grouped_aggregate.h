#ifndef MILENA_GROUPED_AGGREGATE_H
#define MILENA_GROUPED_AGGREGATE_H

#include "mergeable_aggregate.h"
#include "spill_store.h"

#define MILENA_GROUPED_DEFAULT_MAX_RUNS 4096u
#define MILENA_GROUPED_HARD_MAX_RUNS 65536u

/* A bounded, spillable grouped reducer. Keys are opaque byte strings; output
 * is emitted in ascending unsigned-byte lexicographic order. */
typedef struct {
    size_t key_length;
    const unsigned char *key;
    MilenaAggregateResult aggregate;
} MilenaGroupedAggregateResult;

typedef MilenaStatus (*MilenaGroupedAggregateVisitFn)(
    const MilenaGroupedAggregateResult *result, void *context,
    MilenaError *error);

typedef struct {
    size_t key_length;
    MilenaAggregateState aggregate;
} MilenaGroupedAggregateEntry;

typedef struct {
    char *spill_path;
    size_t memory_budget_bytes;
    size_t workspace_budget_bytes;
    size_t max_key_bytes;
    size_t max_runs;
    size_t group_capacity;
    size_t group_count;
    size_t table_capacity;
    MilenaGroupedAggregateEntry *groups;
    unsigned char *key_arena;
    size_t *table;
    MilenaSpillStore spill;
    bool spill_open;
    bool finalized;
    bool failed;
    MilenaStatus failure_status;
} MilenaGroupedAggregate;

/* memory_budget_bytes bounds the resident map and external-sort payload
 * buffers, reserving bounded space for temporary path strings; it excludes
 * caller allocations and libc/stdio internals. spill_quota_bytes bounds the
 * append-only input spill; external-sort runs are independently capped at
 * 2x that quota in aggregate. max_key_bytes bounds every accepted key/record.
 * The scratch path must not already exist; concurrent writers are unsupported.
 * Any flush failure makes the handle terminal: add/finalize reject retries to
 * prevent replaying a partially persisted map and double-counting its prefix. */
MilenaStatus milena_grouped_aggregate_open(
    const char *spill_path, size_t memory_budget_bytes, size_t max_key_bytes,
    size_t spill_quota_bytes, MilenaGroupedAggregate *grouped,
    MilenaError *error);
/* max_runs caps the number of initial sorted runs accepted for one finalization.
 * Pairwise merge keeps at most three run files open at once. */
MilenaStatus milena_grouped_aggregate_open_with_max_runs(
    const char *spill_path, size_t memory_budget_bytes, size_t max_key_bytes,
    size_t spill_quota_bytes, size_t max_runs,
    MilenaGroupedAggregate *grouped, MilenaError *error);
MilenaStatus milena_grouped_aggregate_add(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    double value, MilenaError *error);
MilenaStatus milena_grouped_aggregate_add_null(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    MilenaError *error);
MilenaStatus milena_grouped_aggregate_add_invalid(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    MilenaError *error);
/* Finalization sorts bounded runs and pairwise-merges them externally; cost is
 * O(R log R), memory stays within the configured reducer budget, and callback
 * order is deterministic unsigned-byte lexicographic order. Temporary sort
 * runs are capped at twice spill_quota_bytes in addition to the source spill. */
MilenaStatus milena_grouped_aggregate_finalize(
    MilenaGroupedAggregate *grouped, MilenaGroupedAggregateVisitFn visitor,
    void *context, size_t *groups_emitted, MilenaError *error);
MilenaStatus milena_grouped_aggregate_close(MilenaGroupedAggregate *grouped,
                                            MilenaError *error);

#endif
