#ifndef MILENA_PARTITION_PROTOCOL_H
#define MILENA_PARTITION_PROTOCOL_H

#include "partition_reduce.h"

#define MILENA_PARTITION_PROTOCOL_VERSION 2u
#define MILENA_PARTITION_RESULT_WIRE_SIZE 40u

typedef struct {
    size_t partition_id;
    MilenaStatus status;
    bool valid;
    double value;
} MilenaPartitionResult;

MilenaStatus milena_partition_result_encode(
    const MilenaPartitionResult *result,
    unsigned char *buffer,
    size_t capacity,
    size_t *written,
    MilenaError *error);

MilenaStatus milena_partition_result_decode(
    const unsigned char *buffer,
    size_t length,
    MilenaPartitionResult *result,
    MilenaError *error);

#endif
