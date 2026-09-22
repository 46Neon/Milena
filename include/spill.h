#ifndef MILENA_SPILL_H
#define MILENA_SPILL_H

#include "common.h"

/* Canonical scalar spill contract (MLSPILL1): little-endian uint32 key length,
 * key bytes, IEEE-754 double, followed by FNV-1a checksum.  Grouped stream
 * spilling uses the same MLSPILL1 magic/checksum discipline with a typed
 * record (key length, key, per-metric validity byte + double values). */
typedef struct { const char *directory; size_t max_bytes; size_t partitions; } MilenaSpillPolicy;
typedef struct { char path[512]; size_t bytes; size_t records; uint64_t checksum; } MilenaSpillMetadata;
MilenaSpillPolicy milena_spill_policy_default(void);
MilenaStatus milena_spill_write(const MilenaSpillPolicy *, const char *, const char *const *, const double *, size_t, MilenaSpillMetadata *, MilenaError *);
MilenaStatus milena_spill_read(const MilenaSpillPolicy *, const MilenaSpillMetadata *, char **, double *, size_t, size_t *, MilenaError *);
MilenaStatus milena_spill_remove(MilenaSpillMetadata *, MilenaError *);
#endif
