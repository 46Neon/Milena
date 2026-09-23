#include "partition_protocol.h"

#include <string.h>

static const unsigned char MAGIC[4] = {'M', 'L', 'P', '1'};

static void protocol_error(MilenaError *error, MilenaStatus status,
                           const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static uint64_t checksum(const unsigned char *data, size_t length) {
    uint64_t value = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        value ^= data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static void put_u32(unsigned char *out, uint32_t value) {
    for (size_t i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static uint32_t get_u32(const unsigned char *in) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++) value |= ((uint32_t)in[i]) << (i * 8);
    return value;
}

static void put_u64(unsigned char *out, uint64_t value) {
    for (size_t i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static uint64_t get_u64(const unsigned char *in) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value |= ((uint64_t)in[i]) << (i * 8);
    return value;
}

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double bits_double(uint64_t bits) {
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

MilenaStatus milena_partition_result_encode(
    const MilenaPartitionResult *result, unsigned char *buffer,
    size_t capacity, size_t *written, MilenaError *error) {
    if (!result || !buffer || !written || capacity < MILENA_PARTITION_RESULT_WIRE_SIZE ||
        result->status < MILENA_OK || result->status > MILENA_ERR_INTERNAL ||
        (result->valid && !isfinite(result->value))) {
        protocol_error(error, MILENA_ERR_ARGUMENT,
                       "El resultado de partición no se puede serializar");
        return MILENA_ERR_ARGUMENT;
    }
    if ((uintmax_t)result->partition_id > UINT64_MAX) {
        protocol_error(error, MILENA_ERR_OVERFLOW,
                       "El identificador de partición no cabe en el protocolo");
        return MILENA_ERR_OVERFLOW;
    }
    memset(buffer, 0, MILENA_PARTITION_RESULT_WIRE_SIZE);
    memcpy(buffer, MAGIC, sizeof(MAGIC));
    put_u64(buffer + 4, MILENA_PARTITION_PROTOCOL_VERSION);
    put_u64(buffer + 12, (uint64_t)result->partition_id);
    put_u64(buffer + 20, (uint64_t)result->status |
                         (result->valid ? UINT64_C(1) << 32 : 0));
    put_u64(buffer + 28, double_bits(result->value));
    put_u32(buffer + 36, (uint32_t)checksum(buffer, 36));
    *written = MILENA_PARTITION_RESULT_WIRE_SIZE;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_partition_result_decode(
    const unsigned char *buffer, size_t length,
    MilenaPartitionResult *result, MilenaError *error) {
    if (!buffer || !result || length != MILENA_PARTITION_RESULT_WIRE_SIZE) {
        protocol_error(error, MILENA_ERR_ARGUMENT,
                       "El mensaje de partición tiene una longitud inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (memcmp(buffer, MAGIC, sizeof(MAGIC)) != 0 ||
        get_u64(buffer + 4) != MILENA_PARTITION_PROTOCOL_VERSION) {
        protocol_error(error, MILENA_ERR_PARSE,
                       "Versión o firma de protocolo de partición inválida");
        return MILENA_ERR_PARSE;
    }
    if ((uint32_t)checksum(buffer, 36) != get_u32(buffer + 36)) {
        protocol_error(error, MILENA_ERR_DATA,
                       "El checksum del resultado de partición no coincide");
        return MILENA_ERR_DATA;
    }
    uint64_t flags = get_u64(buffer + 20);
    uint64_t raw_status = flags & UINT64_C(0xffffffff);
    if (raw_status > MILENA_ERR_INTERNAL) {
        protocol_error(error, MILENA_ERR_DATA,
                       "El estado del resultado de partición es inválido");
        return MILENA_ERR_DATA;
    }
    result->partition_id = (size_t)get_u64(buffer + 12);
    if ((uint64_t)result->partition_id != get_u64(buffer + 12)) {
        protocol_error(error, MILENA_ERR_OVERFLOW,
                       "El identificador de partición no cabe en esta plataforma");
        return MILENA_ERR_OVERFLOW;
    }
    result->status = (MilenaStatus)raw_status;
    result->valid = (flags & (UINT64_C(1) << 32)) != 0;
    result->value = bits_double(get_u64(buffer + 28));
    if (result->valid && !isfinite(result->value)) {
        protocol_error(error, MILENA_ERR_DATA,
                       "El resultado de partición contiene un valor no finito");
        return MILENA_ERR_DATA;
    }
    if (error) milena_error_clear(error);
    return MILENA_OK;
}
