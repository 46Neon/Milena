#include "mergeable_aggregate.h"

#include <float.h>

static const unsigned char MAGIC[4] = {'M', 'A', 'G', '1'};

static void aggregate_error(MilenaError *error, MilenaStatus status,
                            const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
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
static uint32_t checksum(const unsigned char *data, size_t length) {
    uint32_t value = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) { value ^= data[i]; value *= UINT32_C(16777619); }
    return value;
}
static void put_double(unsigned char *out, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    put_u64(out, bits);
}
static double get_double(const unsigned char *in) {
    uint64_t bits = get_u64(in);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static bool state_is_valid(const MilenaAggregateState *state) {
    if (!state || !isfinite(state->sum) || !isfinite(state->mean) ||
        !isfinite(state->m2) || !isfinite(state->min) || !isfinite(state->max)) return false;
    if (state->count == 0) return state->sum == 0.0 && state->mean == 0.0 &&
                                  state->m2 == 0.0 && state->min == 0.0 && state->max == 0.0;
    return state->min <= state->max && state->m2 >= 0.0;
}

void milena_aggregate_state_init(MilenaAggregateState *state) {
    if (state) memset(state, 0, sizeof(*state));
}

MilenaStatus milena_aggregate_state_add(MilenaAggregateState *state,
                                        double value, MilenaError *error) {
    if (!state || !isfinite(value) || !state_is_valid(state)) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Estado o valor inválido para agregar");
        return MILENA_ERR_ARGUMENT;
    }
    if (state->count == UINT64_MAX) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Se agotó el contador de agregación");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaAggregateState next = *state;
    uint64_t new_count = state->count + 1;
    double delta = value - state->mean;
    double new_mean = state->mean + delta / (double)new_count;
    double delta2 = value - new_mean;
    double new_m2 = state->m2 + delta * delta2;
    double new_sum = state->sum + value;
    if (!isfinite(new_mean) || !isfinite(new_m2) || !isfinite(new_sum)) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento numérico en agregación");
        return MILENA_ERR_OVERFLOW;
    }
    next.count = new_count; next.mean = new_mean; next.m2 = new_m2; next.sum = new_sum;
    if (state->count == 0 || value < next.min) next.min = value;
    if (state->count == 0 || value > next.max) next.max = value;
    *state = next;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_aggregate_state_merge(MilenaAggregateState *target,
                                          const MilenaAggregateState *other,
                                          MilenaError *error) {
    if (!target || !state_is_valid(target) || !state_is_valid(other)) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Estados inválidos para combinar");
        return MILENA_ERR_ARGUMENT;
    }
    if (other->count == 0) { if (error) milena_error_clear(error); return MILENA_OK; }
    if (target->count == 0) { *target = *other; if (error) milena_error_clear(error); return MILENA_OK; }
    if (other->count > UINT64_MAX - target->count) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento del contador al combinar");
        return MILENA_ERR_OVERFLOW;
    }
    uint64_t count = target->count + other->count;
    double delta = other->mean - target->mean;
    double ratio = (double)other->count / (double)count;
    double mean = target->mean + delta * ratio;
    double cross = delta * delta * ((double)target->count * (double)other->count / (double)count);
    double m2 = target->m2 + other->m2 + cross;
    double sum = target->sum + other->sum;
    if (!isfinite(mean) || !isfinite(m2) || !isfinite(sum)) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento numérico al combinar agregados");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaAggregateState merged = {count, sum, mean, m2,
                                    fmin(target->min, other->min),
                                    fmax(target->max, other->max)};
    *target = merged;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_aggregate_state_encode(const MilenaAggregateState *state,
                                           unsigned char *buffer,
                                           size_t capacity, size_t *written,
                                           MilenaError *error) {
    if (!state_is_valid(state) || !buffer || !written ||
        capacity < MILENA_AGGREGATE_WIRE_SIZE) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Estado de agregación no serializable");
        return MILENA_ERR_ARGUMENT;
    }
    if (sizeof(double) != sizeof(uint64_t) || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024) {
        aggregate_error(error, MILENA_ERR_UNSUPPORTED, "La plataforma no utiliza IEEE-754 binary64");
        return MILENA_ERR_UNSUPPORTED;
    }
    memcpy(buffer, MAGIC, 4); put_u32(buffer + 4, MILENA_AGGREGATE_WIRE_VERSION);
    put_u64(buffer + 8, state->count); put_double(buffer + 16, state->sum);
    put_double(buffer + 24, state->mean); put_double(buffer + 32, state->m2);
    put_double(buffer + 40, state->min); put_double(buffer + 48, state->max);
    put_u32(buffer + 56, checksum(buffer, 56)); *written = MILENA_AGGREGATE_WIRE_SIZE;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_aggregate_state_decode(const unsigned char *buffer,
                                           size_t length,
                                           MilenaAggregateState *state,
                                           MilenaError *error) {
    if (!buffer || !state || length != MILENA_AGGREGATE_WIRE_SIZE) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Longitud de agregado serializado inválida");
        return MILENA_ERR_ARGUMENT;
    }
    if (sizeof(double) != sizeof(uint64_t) || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024) {
        aggregate_error(error, MILENA_ERR_UNSUPPORTED, "La plataforma no utiliza IEEE-754 binary64");
        return MILENA_ERR_UNSUPPORTED;
    }
    if (memcmp(buffer, MAGIC, 4) != 0 || get_u32(buffer + 4) != MILENA_AGGREGATE_WIRE_VERSION) {
        aggregate_error(error, MILENA_ERR_PARSE, "Magic o versión de agregado inválidos");
        return MILENA_ERR_PARSE;
    }
    if (get_u32(buffer + 56) != checksum(buffer, 56)) {
        aggregate_error(error, MILENA_ERR_DATA, "Checksum del agregado no coincide");
        return MILENA_ERR_DATA;
    }
    MilenaAggregateState decoded = {get_u64(buffer + 8), get_double(buffer + 16),
                                    get_double(buffer + 24), get_double(buffer + 32),
                                    get_double(buffer + 40), get_double(buffer + 48)};
    if (!state_is_valid(&decoded)) {
        aggregate_error(error, MILENA_ERR_DATA, "Estado de agregado corrupto o no finito");
        return MILENA_ERR_DATA;
    }
    *state = decoded;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}
