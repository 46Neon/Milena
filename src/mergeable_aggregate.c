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
static bool state_total_count(const MilenaAggregateState *state, uint64_t *total) {
    if (!state || !total || state->null_count > UINT64_MAX - state->count)
        return false;
    uint64_t partial = state->count + state->null_count;
    if (state->invalid_count > UINT64_MAX - partial) return false;
    *total = partial + state->invalid_count;
    return true;
}

static bool state_is_valid(const MilenaAggregateState *state) {
    uint64_t total = 0;
    if (!state || !state_total_count(state, &total) ||
        !isfinite(state->sum) || !isfinite(state->sum_compensation) ||
        !isfinite(state->sum + state->sum_compensation) || !isfinite(state->mean) ||
        !isfinite(state->m2) || !isfinite(state->min) || !isfinite(state->max))
        return false;
    if (state->count == 0) return state->sum == 0.0 &&
        state->sum_compensation == 0.0 && state->mean == 0.0 &&
        state->m2 == 0.0 && state->min == 0.0 && state->max == 0.0;
    return total >= state->count && state->min <= state->max && state->m2 >= 0.0;
}

/* Neumaier summation retains low-order bits even when a later value is larger
 * than the running total. Keeping both components in the state makes partial
 * aggregates safe to spill and combine in the reducer's deterministic order. */
static bool compensated_add(MilenaAggregateState *state, double value) {
    double next = state->sum + value;
    if (!isfinite(next)) return false;
    double adjustment;
    if (fabs(state->sum) >= fabs(value))
        adjustment = (state->sum - next) + value;
    else
        adjustment = (value - next) + state->sum;
    double compensation = state->sum_compensation + adjustment;
    if (!isfinite(adjustment) || !isfinite(compensation) ||
        !isfinite(next + compensation)) return false;
    state->sum = next;
    state->sum_compensation = compensation;
    return true;
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
    uint64_t total_count = 0;
    if (!state_total_count(state, &total_count) || total_count == UINT64_MAX) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Se agotó el contador de agregación");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaAggregateState next = *state;
    uint64_t new_count = state->count + 1;
    double delta = value - state->mean;
    double new_mean = state->mean + delta / (double)new_count;
    double delta2 = value - new_mean;
    double new_m2 = state->m2 + delta * delta2;
    if (!isfinite(new_mean) || !isfinite(new_m2) || !compensated_add(&next, value)) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento numérico en agregación");
        return MILENA_ERR_OVERFLOW;
    }
    next.count = new_count; next.mean = new_mean; next.m2 = new_m2;
    if (state->count == 0 || value < next.min) next.min = value;
    if (state->count == 0 || value > next.max) next.max = value;
    *state = next;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

static MilenaStatus aggregate_state_add_missing(MilenaAggregateState *state,
                                                bool invalid,
                                                MilenaError *error) {
    uint64_t total = 0;
    if (!state || !state_is_valid(state)) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Estado inválido para contar valor ausente");
        return MILENA_ERR_ARGUMENT;
    }
    if (!state_total_count(state, &total) || total == UINT64_MAX ||
        (invalid ? state->invalid_count : state->null_count) == UINT64_MAX) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Se agotó el contador de valores ausentes");
        return MILENA_ERR_OVERFLOW;
    }
    if (invalid) state->invalid_count++;
    else state->null_count++;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_aggregate_state_add_null(MilenaAggregateState *state,
                                             MilenaError *error) {
    return aggregate_state_add_missing(state, false, error);
}

MilenaStatus milena_aggregate_state_add_invalid(MilenaAggregateState *state,
                                                MilenaError *error) {
    return aggregate_state_add_missing(state, true, error);
}

MilenaStatus milena_aggregate_state_merge(MilenaAggregateState *target,
                                          const MilenaAggregateState *other,
                                          MilenaError *error) {
    if (!target || !state_is_valid(target) || !state_is_valid(other)) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Estados inválidos para combinar");
        return MILENA_ERR_ARGUMENT;
    }
    if (other->count > UINT64_MAX - target->count ||
        other->null_count > UINT64_MAX - target->null_count ||
        other->invalid_count > UINT64_MAX - target->invalid_count) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento de contadores al combinar");
        return MILENA_ERR_OVERFLOW;
    }
    uint64_t count = target->count + other->count;
    uint64_t null_count = target->null_count + other->null_count;
    uint64_t invalid_count = target->invalid_count + other->invalid_count;
    if (null_count > UINT64_MAX - count ||
        invalid_count > UINT64_MAX - count - null_count) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento del total de observaciones al combinar");
        return MILENA_ERR_OVERFLOW;
    }
    if (other->count == 0) {
        target->null_count = null_count;
        target->invalid_count = invalid_count;
        if (error) milena_error_clear(error);
        return MILENA_OK;
    }
    if (target->count == 0) {
        MilenaAggregateState merged = *other;
        merged.null_count = null_count;
        merged.invalid_count = invalid_count;
        *target = merged;
        if (error) milena_error_clear(error);
        return MILENA_OK;
    }
    double delta = other->mean - target->mean;
    double ratio = (double)other->count / (double)count;
    double mean = target->mean + delta * ratio;
    double cross = delta * delta * ((double)target->count * (double)other->count / (double)count);
    double m2 = target->m2 + other->m2 + cross;
    MilenaAggregateState merged = *target;
    if (!isfinite(mean) || !isfinite(m2) ||
        !compensated_add(&merged, other->sum) ||
        !compensated_add(&merged, other->sum_compensation)) {
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento numérico al combinar agregados");
        return MILENA_ERR_OVERFLOW;
    }
    merged.count = count;
    merged.null_count = null_count;
    merged.invalid_count = invalid_count;
    merged.mean = mean;
    merged.m2 = m2;
    merged.min = fmin(target->min, other->min);
    merged.max = fmax(target->max, other->max);
    *target = merged;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_aggregate_state_finalize(const MilenaAggregateState *state,
                                             MilenaAggregateResult *result,
                                             MilenaError *error) {
    if (!state || !result || !state_is_valid(state)) {
        aggregate_error(error, MILENA_ERR_ARGUMENT, "Estado inválido para finalizar agregación");
        return MILENA_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->count = state->count;
    result->null_count = state->null_count;
    result->invalid_count = state->invalid_count;
    result->has_values = state->count > 0;
    if (state->count == 0) { if (error) milena_error_clear(error); return MILENA_OK; }
    result->sum = state->sum + state->sum_compensation; result->mean = state->mean;
    result->variance_population = state->m2 / (double)state->count;
    result->stddev_population = sqrt(result->variance_population);
    result->min = state->min; result->max = state->max;
    if (state->count >= 2) {
        result->variance_sample = state->m2 / (double)(state->count - 1);
        result->stddev_sample = sqrt(result->variance_sample);
        result->has_sample_variance = true;
    }
    if (!isfinite(result->variance_population) || !isfinite(result->stddev_population) ||
        (result->has_sample_variance && (!isfinite(result->variance_sample) || !isfinite(result->stddev_sample)))) {
        memset(result, 0, sizeof(*result));
        aggregate_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento al finalizar agregación");
        return MILENA_ERR_OVERFLOW;
    }
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
    put_u64(buffer + 8, state->count);
    put_u64(buffer + 16, state->null_count);
    put_u64(buffer + 24, state->invalid_count);
    put_double(buffer + 32, state->sum);
    put_double(buffer + 40, state->sum_compensation);
    put_double(buffer + 48, state->mean); put_double(buffer + 56, state->m2);
    put_double(buffer + 64, state->min); put_double(buffer + 72, state->max);
    put_u32(buffer + 80, checksum(buffer, 80)); *written = MILENA_AGGREGATE_WIRE_SIZE;
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
    if (get_u32(buffer + 80) != checksum(buffer, 80)) {
        aggregate_error(error, MILENA_ERR_DATA, "Checksum del agregado no coincide");
        return MILENA_ERR_DATA;
    }
    MilenaAggregateState decoded = {
        get_u64(buffer + 8), get_u64(buffer + 16), get_u64(buffer + 24),
        get_double(buffer + 32), get_double(buffer + 40),
        get_double(buffer + 48), get_double(buffer + 56),
        get_double(buffer + 64), get_double(buffer + 72)
    };
    if (!state_is_valid(&decoded)) {
        aggregate_error(error, MILENA_ERR_DATA, "Estado de agregado corrupto o no finito");
        return MILENA_ERR_DATA;
    }
    *state = decoded;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}
