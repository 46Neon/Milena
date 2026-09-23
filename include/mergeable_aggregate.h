#ifndef MILENA_MERGEABLE_AGGREGATE_H
#define MILENA_MERGEABLE_AGGREGATE_H

#include "common.h"

#define MILENA_AGGREGATE_WIRE_VERSION 1u
#define MILENA_AGGREGATE_WIRE_SIZE 60u

typedef struct {
    uint64_t count;
    double sum;
    double mean;
    double m2;
    double min;
    double max;
} MilenaAggregateState;

void milena_aggregate_state_init(MilenaAggregateState *state);
MilenaStatus milena_aggregate_state_add(MilenaAggregateState *state,
                                        double value, MilenaError *error);
MilenaStatus milena_aggregate_state_merge(MilenaAggregateState *target,
                                          const MilenaAggregateState *other,
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
