#ifndef MILENA_MERGEABLE_AGGREGATE_H
#define MILENA_MERGEABLE_AGGREGATE_H

#include "common.h"

#define MILENA_AGGREGATE_WIRE_VERSION 2u
#define MILENA_AGGREGATE_WIRE_SIZE 68u

typedef struct {
    uint64_t count;
    double sum;
    /* Neumaier correction retained across spill serialization and merges. */
    double sum_compensation;
    double mean;
    double m2;
    double min;
    double max;
} MilenaAggregateState;

typedef struct {
    uint64_t count;
    double sum;
    double mean;
    double variance_population;
    double stddev_population;
    double variance_sample;
    double stddev_sample;
    double min;
    double max;
    bool has_values;
    bool has_sample_variance;
} MilenaAggregateResult;

void milena_aggregate_state_init(MilenaAggregateState *state);
MilenaStatus milena_aggregate_state_add(MilenaAggregateState *state,
                                        double value, MilenaError *error);
MilenaStatus milena_aggregate_state_merge(MilenaAggregateState *target,
                                          const MilenaAggregateState *other,
                                          MilenaError *error);
MilenaStatus milena_aggregate_state_finalize(const MilenaAggregateState *state,
                                             MilenaAggregateResult *result,
                                             MilenaError *error);
MilenaStatus milena_aggregate_state_encode(const MilenaAggregateState *state,
                                           unsigned char *buffer,
                                           size_t capacity, size_t *written,
                                           MilenaError *error);
MilenaStatus milena_aggregate_state_decode(const unsigned char *buffer,
                                           size_t length,
                                           MilenaAggregateState *state,
                                           MilenaError *error);

#endif
