#ifndef MILENA_MERGEABLE_AGGREGATE_H
#define MILENA_MERGEABLE_AGGREGATE_H

#include "common.h"

#define MILENA_AGGREGATE_WIRE_VERSION 4u
#define MILENA_AGGREGATE_WIRE_SIZE 96u

typedef enum {
    MILENA_AGGREGATE_VALUE_NONE = 0,
    MILENA_AGGREGATE_VALUE_FLOAT64 = 1,
    MILENA_AGGREGATE_VALUE_INT64 = 2
} MilenaAggregateValueKind;

typedef struct {
    uint64_t count; /* valid observations */
    uint64_t null_count;
    uint64_t invalid_count;
    int64_t integer_sum;
    MilenaAggregateValueKind value_kind;
    double sum;
    /* Neumaier correction retained across spill serialization and merges. */
    double sum_compensation;
    double mean;
    double m2;
    double min;
    double max;
} MilenaAggregateState;

typedef struct {
    uint64_t count; /* valid observations */
    uint64_t null_count;
    uint64_t invalid_count;
    int64_t integer_sum;
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
    bool has_integer_sum;
} MilenaAggregateResult;

void milena_aggregate_state_init(MilenaAggregateState *state);
MilenaStatus milena_aggregate_state_add(MilenaAggregateState *state,
                                        double value, MilenaError *error);
MilenaStatus milena_aggregate_state_add_int64(MilenaAggregateState *state,
                                              int64_t value,
                                              MilenaError *error);
MilenaStatus milena_aggregate_state_add_null(MilenaAggregateState *state,
                                             MilenaError *error);
MilenaStatus milena_aggregate_state_add_invalid(MilenaAggregateState *state,
                                                MilenaError *error);
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
