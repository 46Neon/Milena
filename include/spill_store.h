#ifndef MILENA_SPILL_STORE_H
#define MILENA_SPILL_STORE_H

#include "common.h"

#define MILENA_SPILL_VERSION 1u
#define MILENA_SPILL_HEADER_SIZE 16u
#define MILENA_SPILL_TRAILER_SIZE 4u

typedef struct {
    FILE *file;
    char *path;
    size_t quota_bytes;
    size_t max_record_bytes;
    size_t bytes_used;
    size_t record_count;
    bool failed; /* A partial write requires close/reopen recovery before appending. */
} MilenaSpillStore;

typedef MilenaStatus (*MilenaSpillVisitFn)(const void *data, size_t length,
                                           size_t record_index, void *context,
                                           MilenaError *error);

typedef struct {
    size_t records_recovered;
    size_t bytes_recovered;
    size_t bytes_discarded;
    bool truncated_tail;
} MilenaSpillRecovery;

MilenaStatus milena_spill_store_open(const char *path, size_t quota_bytes,
                                     size_t max_record_bytes,
                                     MilenaSpillStore *store,
                                     MilenaError *error);
MilenaStatus milena_spill_store_append(MilenaSpillStore *store,
                                       const void *data, size_t length,
                                       MilenaError *error);
MilenaStatus milena_spill_store_close(MilenaSpillStore *store,
                                      MilenaError *error);
/* Validates the append-only prefix and replaces the file without deleting the
 * original first. Valid records over the configured quota/size are rejected. */
MilenaStatus milena_spill_store_recover(const char *path, size_t quota_bytes,
                                        size_t max_record_bytes,
                                        MilenaSpillRecovery *recovery,
                                        MilenaError *error);
/* Replays validated records in order using at most max_record_bytes of payload memory. */
MilenaStatus milena_spill_store_visit(const char *path, size_t quota_bytes,
                                      size_t max_record_bytes,
                                      MilenaSpillVisitFn visitor,
                                      void *context,
                                      MilenaError *error);

#endif
