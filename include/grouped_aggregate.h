#ifndef MILENA_GROUPED_AGGREGATE_H
#define MILENA_GROUPED_AGGREGATE_H

#include "mergeable_aggregate.h"
#include "spill_store.h"

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
    size_t max_key_bytes;
    size_t group_capacity;
    size_t group_count;
    size_t table_capacity;
    MilenaGroupedAggregateEntry *groups;
    unsigned char *key_arena;
    size_t *table;
    MilenaSpillStore spill;
    bool spill_open;
    bool finalized;
} MilenaGroupedAggregate;

/* memory_budget_bytes bounds reducer-owned heap buffers, including the map,
 * one maximal spill payload, and bounded finalization key buffers; it excludes
 * caller allocations and libc/stdio internals. spill_quota_bytes bounds the
 * append-only spill; max_key_bytes bounds every accepted key and record. The
 * scratch path must not already exist, and concurrent writers are unsupported. */
MilenaStatus milena_grouped_aggregate_open(
    const char *spill_path, size_t memory_budget_bytes, size_t max_key_bytes,
    size_t spill_quota_bytes, MilenaGroupedAggregate *grouped,
    MilenaError *error);
MilenaStatus milena_grouped_aggregate_add(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    double value, MilenaError *error);
/* Finalization scans the bounded spill repeatedly, so memory stays bounded
 * independent of group cardinality; callback order is deterministic. */
MilenaStatus milena_grouped_aggregate_finalize(
    MilenaGroupedAggregate *grouped, MilenaGroupedAggregateVisitFn visitor,
    void *context, size_t *groups_emitted, MilenaError *error);
MilenaStatus milena_grouped_aggregate_close(MilenaGroupedAggregate *grouped,
                                            MilenaError *error);

#endif
