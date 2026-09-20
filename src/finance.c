#include "finance.h"

static void finance_error(MilenaError *error, MilenaStatus code,
                          const char *message) {
    if (error) milena_error_set(error, code, 0, 0, 0, message);
}

static bool decimal_valid(const MilenaDecimal *value) {
    return value && value->scale >= 0 && value->scale <= MILENA_DECIMAL_MAX_SCALE;
}

static MilenaStatus decimal_normalize(MilenaDecimal *value, MilenaError *error) {
    if (!decimal_valid(value)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Decimal inválido");
        return MILENA_ERR_ARGUMENT;
    }
    while (value->scale > 0 && value->coefficient % 10 == 0) {
        value->coefficient /= 10;
        value->scale--;
    }
    return MILENA_OK;
}

static bool multiply_power10(int64_t value, int32_t power, int64_t *out) {
    int64_t result = value;
    for (int32_t i = 0; i < power; i++) {
        if (result > INT64_MAX / 10 || result < INT64_MIN / 10) return false;
        result *= 10;
    }
    *out = result;
    return true;
}

static MilenaStatus decimal_align(const MilenaDecimal *left,
                                  const MilenaDecimal *right,
                                  int64_t *left_coefficient,
                                  int64_t *right_coefficient,
                                  int32_t *scale,
                                  MilenaError *error) {
    if (!decimal_valid(left) || !decimal_valid(right)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Decimal inválido");
        return MILENA_ERR_ARGUMENT;
    }
    *scale = left->scale > right->scale ? left->scale : right->scale;
    if (!multiply_power10(left->coefficient, *scale - left->scale,
                          left_coefficient) ||
        !multiply_power10(right->coefficient, *scale - right->scale,
                          right_coefficient)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento al alinear decimales");
        return MILENA_ERR_OVERFLOW;
    }
    return MILENA_OK;
}

MilenaStatus milena_decimal_from_i64(MilenaDecimal *out, int64_t value,
                                     MilenaError *error) {
    if (!out) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Salida decimal nula");
        return MILENA_ERR_ARGUMENT;
    }
    out->coefficient = value;
    out->scale = 0;
    return MILENA_OK;
}

MilenaStatus milena_decimal_from_string(MilenaDecimal *out, const char *text,
                                        MilenaError *error) {
    if (!out || !text || text[0] == '\0') {
        finance_error(error, MILENA_ERR_ARGUMENT, "Texto decimal vacío");
        return MILENA_ERR_ARGUMENT;
    }
    size_t position = 0;
    bool negative = false;
    if (text[position] == '+' || text[position] == '-') {
        negative = text[position] == '-';
        position++;
    }
    if (text[position] == '\0') {
        finance_error(error, MILENA_ERR_PARSE, "Decimal sin dígitos");
        return MILENA_ERR_PARSE;
    }

    uint64_t magnitude = 0;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    int32_t scale = 0;
    bool saw_digit = false;
    bool after_decimal = false;
    for (; text[position] != '\0'; position++) {
        char current = text[position];
        if (current == '.') {
            if (after_decimal) {
                finance_error(error, MILENA_ERR_PARSE, "Más de un separador decimal");
                return MILENA_ERR_PARSE;
            }
            after_decimal = true;
            continue;
        }
        if (current < '0' || current > '9') {
            finance_error(error, MILENA_ERR_PARSE, "Decimal inválido: se esperaba un dígito");
            return MILENA_ERR_PARSE;
        }
        saw_digit = true;
        if (after_decimal) {
            if (scale == MILENA_DECIMAL_MAX_SCALE) {
                finance_error(error, MILENA_ERR_OVERFLOW, "El decimal supera 18 posiciones");
                return MILENA_ERR_OVERFLOW;
            }
            scale++;
        }
        uint64_t digit = (uint64_t)(current - '0');
        if (magnitude > (limit - digit) / 10u) {
            finance_error(error, MILENA_ERR_OVERFLOW, "Decimal fuera del rango int64");
            return MILENA_ERR_OVERFLOW;
        }
        magnitude = magnitude * 10u + digit;
    }
    if (!saw_digit) {
        finance_error(error, MILENA_ERR_PARSE, "Decimal sin dígitos");
        return MILENA_ERR_PARSE;
    }
    if (negative) {
        out->coefficient = magnitude == (uint64_t)INT64_MAX + 1u ?
            INT64_MIN : -(int64_t)magnitude;
    } else {
        out->coefficient = (int64_t)magnitude;
    }
    out->scale = scale;
    return decimal_normalize(out, error);
}

MilenaStatus milena_decimal_add(MilenaDecimal *out, const MilenaDecimal *left,
                                const MilenaDecimal *right, MilenaError *error) {
    if (!out) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Salida decimal nula");
        return MILENA_ERR_ARGUMENT;
    }
    int64_t left_value = 0;
    int64_t right_value = 0;
    int32_t scale = 0;
    MilenaStatus status = decimal_align(left, right, &left_value, &right_value,
                                        &scale, error);
    if (status != MILENA_OK) return status;
    if ((right_value > 0 && left_value > INT64_MAX - right_value) ||
        (right_value < 0 && left_value < INT64_MIN - right_value)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento al sumar decimales");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaDecimal result = {left_value + right_value, scale};
    status = decimal_normalize(&result, error);
    if (status == MILENA_OK) *out = result;
    return status;
}

MilenaStatus milena_decimal_sub(MilenaDecimal *out, const MilenaDecimal *left,
                                const MilenaDecimal *right, MilenaError *error) {
    if (!right) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Decimal derecho nulo");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal negative = *right;
    if (negative.coefficient == INT64_MIN) {
        finance_error(error, MILENA_ERR_OVERFLOW, "No se puede negar el decimal mínimo");
        return MILENA_ERR_OVERFLOW;
    }
    negative.coefficient = -negative.coefficient;
    return milena_decimal_add(out, left, &negative, error);
}

MilenaStatus milena_decimal_compare(const MilenaDecimal *left,
                                    const MilenaDecimal *right, int *result,
                                    MilenaError *error) {
    if (!result) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Resultado de comparación nulo");
        return MILENA_ERR_ARGUMENT;
    }
    int64_t left_value = 0;
    int64_t right_value = 0;
    int32_t scale = 0;
    MilenaStatus status = decimal_align(left, right, &left_value, &right_value,
                                        &scale, error);
    if (status != MILENA_OK) return status;
    *result = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    return MILENA_OK;
}

static MilenaStatus check_currency(const char *currency, MilenaError *error) {
    if (!currency || strlen(currency) != 3 ||
        currency[0] < 'A' || currency[0] > 'Z' ||
        currency[1] < 'A' || currency[1] > 'Z' ||
        currency[2] < 'A' || currency[2] > 'Z') {
        finance_error(error, MILENA_ERR_ARGUMENT, "La moneda debe ser un código ISO de tres letras mayúsculas");
        return MILENA_ERR_ARGUMENT;
    }
    return MILENA_OK;
}

MilenaStatus milena_money_init(MilenaMoney *out, MilenaDecimal amount,
                               const char *currency, MilenaError *error) {
    if (!out || !decimal_valid(&amount)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Dinero inválido");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = check_currency(currency, error);
    if (status != MILENA_OK) return status;
    out->amount = amount;
    memcpy(out->currency, currency, MILENA_CURRENCY_CODE_SIZE);
    return MILENA_OK;
}

static MilenaStatus money_operation(MilenaMoney *out, const MilenaMoney *left,
                                    const MilenaMoney *right, bool subtract,
                                    MilenaError *error) {
    if (!out || !left || !right ||
        strcmp(left->currency, right->currency) != 0) {
        finance_error(error, MILENA_ERR_ARGUMENT, "No se pueden combinar monedas distintas");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal result = {0};
    MilenaStatus status = subtract ?
        milena_decimal_sub(&result, &left->amount, &right->amount, error) :
        milena_decimal_add(&result, &left->amount, &right->amount, error);
    if (status != MILENA_OK) return status;
    *out = *left;
    out->amount = result;
    return MILENA_OK;
}

MilenaStatus milena_money_add(MilenaMoney *out, const MilenaMoney *left,
                              const MilenaMoney *right, MilenaError *error) {
    return money_operation(out, left, right, false, error);
}

MilenaStatus milena_money_sub(MilenaMoney *out, const MilenaMoney *left,
                              const MilenaMoney *right, MilenaError *error) {
    return money_operation(out, left, right, true, error);
}

MilenaStatus milena_rate_init(MilenaRate *out, MilenaDecimal value,
                              MilenaRateKind kind, uint32_t periods_per_year,
                              MilenaError *error) {
    if (!out || !decimal_valid(&value) || kind < MILENA_RATE_NOMINAL ||
        kind > MILENA_RATE_PERIODIC || periods_per_year == 0) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Tasa o período inválido");
        return MILENA_ERR_ARGUMENT;
    }
    out->value = value;
    out->kind = kind;
    out->periods_per_year = periods_per_year;
    return MILENA_OK;
}

static uint64_t decimal_abs_u64(int64_t value) {
    return value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
}

static bool unsigned_mul_checked(uint64_t left, uint64_t right, uint64_t *out) {
    if (right != 0 && left > UINT64_MAX / right) return false;
    *out = left * right;
    return true;
}

static bool unsigned_pow10(int32_t power, uint64_t *out) {
    uint64_t result = 1;
    for (int32_t i = 0; i < power; i++) {
        if (!unsigned_mul_checked(result, 10u, &result)) return false;
    }
    *out = result;
    return true;
}

static bool signed_from_magnitude(uint64_t magnitude, bool negative,
                                  int64_t *out) {
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    if (magnitude > limit) return false;
    if (!negative) {
        *out = (int64_t)magnitude;
    } else if (magnitude == (uint64_t)INT64_MAX + 1u) {
        *out = INT64_MIN;
    } else {
        *out = -(int64_t)magnitude;
    }
    return true;
}

static bool should_round(uint64_t quotient, uint64_t remainder,
                         uint64_t divisor, bool negative,
                         MilenaRoundingMode mode) {
    if (remainder == 0 || mode == MILENA_ROUND_TOWARD_ZERO) return false;
    if (mode == MILENA_ROUND_FLOOR) return negative;
    if (mode == MILENA_ROUND_CEILING) return !negative;
    uint64_t half = divisor / 2u;
    bool above_half = remainder > half;
    bool exact_half = (divisor % 2u == 0u && remainder == half);
    if (mode == MILENA_ROUND_HALF_UP) return above_half || exact_half;
    return above_half || (exact_half && (quotient % 2u != 0u));
}

MilenaStatus milena_decimal_round(MilenaDecimal *out,
                                  const MilenaDecimal *value,
                                  int32_t target_scale,
                                  MilenaRoundingMode mode,
                                  MilenaError *error) {
    if (!out || !decimal_valid(value) || target_scale < 0 ||
        target_scale > MILENA_DECIMAL_MAX_SCALE || mode < MILENA_ROUND_TOWARD_ZERO ||
        mode > MILENA_ROUND_CEILING) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Parámetros de redondeo inválidos");
        return MILENA_ERR_ARGUMENT;
    }
    if (target_scale >= value->scale) {
        int64_t coefficient = 0;
        if (!multiply_power10(value->coefficient, target_scale - value->scale,
                              &coefficient)) {
            finance_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento al ampliar escala");
            return MILENA_ERR_OVERFLOW;
        }
        MilenaDecimal result = {coefficient, target_scale};
        decimal_normalize(&result, error);
        *out = result;
        return MILENA_OK;
    }

    uint64_t divisor = 0;
    if (!unsigned_pow10(value->scale - target_scale, &divisor)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Escala de redondeo fuera de rango");
        return MILENA_ERR_OVERFLOW;
    }
    uint64_t magnitude = decimal_abs_u64(value->coefficient);
    uint64_t quotient = magnitude / divisor;
    uint64_t remainder = magnitude % divisor;
    bool negative = value->coefficient < 0;
    if (should_round(quotient, remainder, divisor, negative, mode)) {
        if (quotient == UINT64_MAX) {
            finance_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento al redondear");
            return MILENA_ERR_OVERFLOW;
        }
        quotient++;
    }
    int64_t coefficient = 0;
    if (!signed_from_magnitude(quotient, negative, &coefficient)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Resultado redondeado fuera del rango int64");
        return MILENA_ERR_OVERFLOW;
    }
    out->coefficient = coefficient;
    out->scale = target_scale;
    decimal_normalize(out, error);
    return MILENA_OK;
}

MilenaStatus milena_decimal_mul(MilenaDecimal *out,
                                const MilenaDecimal *left,
                                const MilenaDecimal *right,
                                MilenaError *error) {
    if (!out || !decimal_valid(left) || !decimal_valid(right)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Parámetros de multiplicación inválidos");
        return MILENA_ERR_ARGUMENT;
    }
    if (left->scale > MILENA_DECIMAL_MAX_SCALE - right->scale) {
        finance_error(error, MILENA_ERR_OVERFLOW, "La escala del producto supera 18 posiciones");
        return MILENA_ERR_OVERFLOW;
    }
    uint64_t magnitude = 0;
    if (!unsigned_mul_checked(decimal_abs_u64(left->coefficient),
                              decimal_abs_u64(right->coefficient), &magnitude)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Desbordamiento al multiplicar decimales");
        return MILENA_ERR_OVERFLOW;
    }
    int64_t coefficient = 0;
    if (!signed_from_magnitude(magnitude,
                               (left->coefficient < 0) != (right->coefficient < 0),
                               &coefficient)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Producto fuera del rango int64");
        return MILENA_ERR_OVERFLOW;
    }
    out->coefficient = coefficient;
    out->scale = left->scale + right->scale;
    decimal_normalize(out, error);
    return MILENA_OK;
}

MilenaStatus milena_decimal_div(MilenaDecimal *out,
                                const MilenaDecimal *left,
                                const MilenaDecimal *right,
                                int32_t target_scale,
                                MilenaRoundingMode mode,
                                MilenaError *error) {
    if (!out || !decimal_valid(left) || !decimal_valid(right) ||
        right->coefficient == 0 || target_scale < 0 ||
        target_scale > MILENA_DECIMAL_MAX_SCALE) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Parámetros de división inválidos");
        return MILENA_ERR_ARGUMENT;
    }
    int32_t numerator_power = target_scale + right->scale;
    uint64_t numerator = decimal_abs_u64(left->coefficient);
    uint64_t denominator = decimal_abs_u64(right->coefficient);
    uint64_t power = 0;
    if (!unsigned_pow10(numerator_power, &power) ||
        !unsigned_mul_checked(numerator, power, &numerator)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Numerador fuera del rango interno");
        return MILENA_ERR_OVERFLOW;
    }
    if (!unsigned_pow10(left->scale, &power) ||
        !unsigned_mul_checked(denominator, power, &denominator)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Denominador fuera del rango interno");
        return MILENA_ERR_OVERFLOW;
    }
    uint64_t quotient = numerator / denominator;
    uint64_t remainder = numerator % denominator;
    bool negative = (left->coefficient < 0) != (right->coefficient < 0);
    if (should_round(quotient, remainder, denominator, negative, mode)) quotient++;
    if (!signed_from_magnitude(quotient, negative, &out->coefficient)) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Cociente fuera del rango int64");
        return MILENA_ERR_OVERFLOW;
    }
    out->scale = target_scale;
    decimal_normalize(out, error);
    return MILENA_OK;
}

static MilenaStatus decimal_one(MilenaDecimal *out, MilenaError *error) {
    return milena_decimal_from_i64(out, 1, error);
}

static MilenaStatus decimal_negate(MilenaDecimal *out, const MilenaDecimal *value,
                                   MilenaError *error) {
    if (!out || !decimal_valid(value)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Decimal inválido");
        return MILENA_ERR_ARGUMENT;
    }
    if (value->coefficient == INT64_MIN) {
        finance_error(error, MILENA_ERR_OVERFLOW, "No se puede negar el decimal mínimo");
        return MILENA_ERR_OVERFLOW;
    }
    *out = *value;
    out->coefficient = -out->coefficient;
    return MILENA_OK;
}

MilenaStatus milena_decimal_pow_uint(MilenaDecimal *out,
                                     const MilenaDecimal *base,
                                     uint32_t exponent,
                                     MilenaError *error) {
    if (!out || !decimal_valid(base)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Base de potencia inválida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal result;
    MilenaDecimal factor = *base;
    MilenaStatus status = decimal_one(&result, error);
    while (status == MILENA_OK && exponent > 0) {
        if (exponent & 1u) {
            MilenaDecimal next;
            status = milena_decimal_mul(&next, &result, &factor, error);
            if (status != MILENA_OK) break;
            result = next;
        }
        exponent >>= 1u;
        if (exponent > 0) {
            MilenaDecimal next;
            status = milena_decimal_mul(&next, &factor, &factor, error);
            if (status != MILENA_OK) break;
            factor = next;
        }
    }
    if (status == MILENA_OK) *out = result;
    return status;
}

static MilenaStatus require_periodic_rate(const MilenaRate *rate,
                                          MilenaError *error) {
    if (!rate || !decimal_valid(&rate->value) ||
        rate->kind != MILENA_RATE_PERIODIC || rate->periods_per_year == 0) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Se requiere una tasa periódica válida");
        return MILENA_ERR_ARGUMENT;
    }
    return MILENA_OK;
}

MilenaStatus milena_rate_nominal_to_effective(MilenaDecimal *out,
                                               const MilenaRate *nominal,
                                               int32_t output_scale,
                                               MilenaRoundingMode mode,
                                               MilenaError *error) {
    if (!out || !nominal || nominal->kind != MILENA_RATE_NOMINAL ||
        nominal->periods_per_year == 0) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Se requiere una tasa nominal válida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal periods;
    MilenaDecimal periodic;
    MilenaDecimal one;
    MilenaDecimal factor;
    MilenaDecimal effective;
    MilenaStatus status = milena_decimal_from_i64(
        &periods, (int64_t)nominal->periods_per_year, error);
    if (status == MILENA_OK) status = milena_decimal_div(
        &periodic, &nominal->value, &periods, output_scale, mode, error);
    if (status == MILENA_OK) status = decimal_one(&one, error);
    if (status == MILENA_OK) status = milena_decimal_add(&factor, &one, &periodic, error);
    if (status == MILENA_OK) status = milena_decimal_pow_uint(
        &factor, &factor, nominal->periods_per_year, error);
    if (status == MILENA_OK) status = milena_decimal_sub(&effective, &factor, &one, error);
    if (status == MILENA_OK) status = milena_decimal_round(
        out, &effective, output_scale, mode, error);
    return status;
}

MilenaStatus milena_future_value(MilenaDecimal *out,
                                 const MilenaDecimal *principal,
                                 const MilenaRate *periodic_rate,
                                 uint32_t periods,
                                 MilenaError *error) {
    MilenaStatus status = require_periodic_rate(periodic_rate, error);
    if (status != MILENA_OK || !out || !decimal_valid(principal)) return status != MILENA_OK ? status : MILENA_ERR_ARGUMENT;
    MilenaDecimal one;
    MilenaDecimal factor;
    MilenaDecimal growth;
    status = decimal_one(&one, error);
    if (status == MILENA_OK) status = milena_decimal_add(&factor, &one,
                                                          &periodic_rate->value, error);
    if (status == MILENA_OK) status = milena_decimal_pow_uint(&growth, &factor, periods, error);
    if (status == MILENA_OK) status = milena_decimal_mul(out, principal, &growth, error);
    return status;
}

MilenaStatus milena_present_value(MilenaDecimal *out,
                                  const MilenaDecimal *future_value,
                                  const MilenaRate *periodic_rate,
                                  uint32_t periods,
                                  int32_t output_scale,
                                  MilenaRoundingMode mode,
                                  MilenaError *error) {
    MilenaStatus status = require_periodic_rate(periodic_rate, error);
    if (status != MILENA_OK || !out || !decimal_valid(future_value)) return status != MILENA_OK ? status : MILENA_ERR_ARGUMENT;
    MilenaDecimal one;
    MilenaDecimal factor;
    status = decimal_one(&one, error);
    if (status == MILENA_OK) status = milena_decimal_add(&factor, &one,
                                                          &periodic_rate->value, error);
    if (status == MILENA_OK) status = milena_decimal_pow_uint(&factor, &factor, periods, error);
    if (status == MILENA_OK) status = milena_decimal_div(out, future_value, &factor,
                                                          output_scale, mode, error);
    return status;
}

MilenaStatus milena_simple_interest(MilenaDecimal *out,
                                    const MilenaDecimal *principal,
                                    const MilenaRate *periodic_rate,
                                    uint32_t periods,
                                    MilenaError *error) {
    MilenaStatus status = require_periodic_rate(periodic_rate, error);
    if (status != MILENA_OK || !out || !decimal_valid(principal)) return status != MILENA_OK ? status : MILENA_ERR_ARGUMENT;
    MilenaDecimal count;
    MilenaDecimal interest;
    MilenaDecimal factor;
    MilenaDecimal one;
    status = milena_decimal_from_i64(&count, (int64_t)periods, error);
    if (status == MILENA_OK) status = milena_decimal_mul(&interest, &periodic_rate->value, &count, error);
    if (status == MILENA_OK) status = decimal_one(&one, error);
    if (status == MILENA_OK) status = milena_decimal_add(&factor, &one, &interest, error);
    if (status == MILENA_OK) status = milena_decimal_mul(out, principal, &factor, error);
    return status;
}

MilenaStatus milena_compound_interest(MilenaDecimal *out,
                                      const MilenaDecimal *principal,
                                      const MilenaRate *periodic_rate,
                                      uint32_t periods,
                                      MilenaError *error) {
    return milena_future_value(out, principal, periodic_rate, periods, error);
}

MilenaStatus milena_annuity_payment(MilenaDecimal *out,
                                    const MilenaDecimal *principal,
                                    const MilenaRate *periodic_rate,
                                    uint32_t periods,
                                    int32_t output_scale,
                                    MilenaRoundingMode mode,
                                    MilenaError *error) {
    MilenaStatus status = require_periodic_rate(periodic_rate, error);
    if (status != MILENA_OK || !out || !decimal_valid(principal) || periods == 0) {
        if (status == MILENA_OK) finance_error(error, MILENA_ERR_ARGUMENT, "Período de amortización inválido");
        return status != MILENA_OK ? status : MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal count;
    status = milena_decimal_from_i64(&count, (int64_t)periods, error);
    if (status != MILENA_OK) return status;
    if (periodic_rate->value.coefficient == 0) {
        return milena_decimal_div(out, principal, &count, output_scale, mode, error);
    }
    MilenaDecimal one;
    MilenaDecimal factor;
    MilenaDecimal inverse;
    MilenaDecimal denominator;
    MilenaDecimal numerator;
    MilenaDecimal payment;
    status = decimal_one(&one, error);
    if (status == MILENA_OK) status = milena_decimal_add(&factor, &one, &periodic_rate->value, error);
    if (status == MILENA_OK) status = milena_decimal_pow_uint(&factor, &factor, periods, error);
    int32_t inverse_scale = output_scale > 4 ? 4 : output_scale;
    if (status == MILENA_OK) status = milena_decimal_div(&inverse, &one, &factor, inverse_scale, mode, error);
    if (status == MILENA_OK) status = milena_decimal_sub(&denominator, &one, &inverse, error);
    if (status == MILENA_OK) status = milena_decimal_mul(&numerator, principal, &periodic_rate->value, error);
    if (status == MILENA_OK) status = milena_decimal_div(&payment, &numerator, &denominator,
                                                          output_scale, mode, error);
    if (status == MILENA_OK) *out = payment;
    return status;
}

static bool date_leap(int32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned date_month_days(int32_t year, unsigned month) {
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return month == 2 && date_leap(year) ? 29u : days[month - 1u];
}

MilenaStatus milena_date_init(MilenaDate *out, int32_t year, unsigned month,
                              unsigned day, MilenaError *error) {
    if (!out || month < 1 || month > 12 || day < 1 || day > date_month_days(year, month)) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Fecha inválida");
        return MILENA_ERR_ARGUMENT;
    }
    out->year = year;
    out->month = month;
    out->day = day;
    out->valid = true;
    return MILENA_OK;
}

MilenaStatus milena_date_compare(const MilenaDate *left, const MilenaDate *right,
                                 int *result, MilenaError *error) {
    if (!left || !right || !result || left->month < 1 || left->month > 12 ||
        right->month < 1 || right->month > 12) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Fecha inválida para comparar");
        return MILENA_ERR_ARGUMENT;
    }
    if (left->year != right->year) *result = left->year < right->year ? -1 : 1;
    else if (left->month != right->month) *result = left->month < right->month ? -1 : 1;
    else *result = left->day < right->day ? -1 : (left->day > right->day ? 1 : 0);
    return MILENA_OK;
}

static int64_t date_serial(MilenaDate date) {
    int64_t year = date.year - (date.month <= 2 ? 1 : 0);
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    uint32_t year_of_era = (uint32_t)(year - era * 400);
    int32_t adjusted_month = (int32_t)date.month + (date.month > 2 ? -3 : 9);
    uint32_t day_of_year = (uint32_t)((153 * adjusted_month + 2) / 5) + date.day - 1u;
    uint32_t day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097 + (int64_t)day_of_era;
}

MilenaStatus milena_date_days_between(const MilenaDate *start,
                                      const MilenaDate *end,
                                      int64_t *days, MilenaError *error) {
    int comparison = 0;
    MilenaStatus status = milena_date_compare(start, end, &comparison, error);
    if (status != MILENA_OK) return status;
    (void)comparison;
    if (!days) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Salida de días nula");
        return MILENA_ERR_ARGUMENT;
    }
    *days = date_serial(*end) - date_serial(*start);
    return MILENA_OK;
}

MilenaStatus milena_period_fraction(const MilenaDate *start,
                                    const MilenaDate *end,
                                    MilenaDayCount convention,
                                    MilenaDecimal *out, MilenaError *error) {
    if (convention < MILENA_DAY_COUNT_ACTUAL_365 || convention > MILENA_DAY_COUNT_ACTUAL_360) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Convención de días inválida");
        return MILENA_ERR_ARGUMENT;
    }
    int64_t days = 0;
    MilenaStatus status = milena_date_days_between(start, end, &days, error);
    if (status != MILENA_OK) return status;
    MilenaDecimal numerator;
    MilenaDecimal denominator;
    status = milena_decimal_from_i64(&numerator, days, error);
    if (status == MILENA_OK) status = milena_decimal_from_i64(
        &denominator, convention == MILENA_DAY_COUNT_ACTUAL_365 ? 365 : 360, error);
    if (status == MILENA_OK) status = milena_decimal_div(
        out, &numerator, &denominator, 15,
        MILENA_ROUND_HALF_EVEN, error);
    return status;
}

void milena_cash_flow_series_init(MilenaCashFlowSeries *series) {
    if (series) memset(series, 0, sizeof(*series));
}

void milena_cash_flow_series_destroy(MilenaCashFlowSeries *series) {
    if (!series) return;
    free(series->items);
    memset(series, 0, sizeof(*series));
}

MilenaStatus milena_cash_flow_series_add(MilenaCashFlowSeries *series,
                                         MilenaCashFlow flow,
                                         MilenaError *error) {
    MilenaDate validated_date;
    if (!series || check_currency(flow.money.currency, error) != MILENA_OK ||
        milena_date_init(&validated_date, flow.date.year, flow.date.month,
                         flow.date.day, error) != MILENA_OK) {
        if (error && error->code == MILENA_OK)
            finance_error(error, MILENA_ERR_ARGUMENT, "Flujo de caja inválido");
        return MILENA_ERR_ARGUMENT;
    }
    flow.date = validated_date;
    if (series->count > 0 && strcmp(series->currency, flow.money.currency) != 0) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Los flujos deben usar la misma moneda");
        return MILENA_ERR_ARGUMENT;
    }
    if (series->count > 0) {
        int comparison = 0;
        MilenaStatus status = milena_date_compare(
            &series->items[series->count - 1].date, &flow.date, &comparison, error);
        if (status != MILENA_OK || comparison > 0) {
            if (status == MILENA_OK)
                finance_error(error, MILENA_ERR_ARGUMENT, "Los flujos deben estar ordenados por fecha");
            return status == MILENA_OK ? MILENA_ERR_ARGUMENT : status;
        }
    }
    if (series->count == series->capacity) {
        size_t capacity = series->capacity == 0 ? 8 : series->capacity * 2;
        if (capacity < series->capacity || capacity > SIZE_MAX / sizeof(*series->items)) {
            finance_error(error, MILENA_ERR_OVERFLOW, "Demasiados flujos de caja");
            return MILENA_ERR_OVERFLOW;
        }
        MilenaCashFlow *items = (MilenaCashFlow *)realloc(
            series->items, capacity * sizeof(*items));
        if (!items) {
            finance_error(error, MILENA_ERR_MEMORY, "No se pudo reservar la serie de flujos");
            return MILENA_ERR_MEMORY;
        }
        series->items = items;
        series->capacity = capacity;
    }
    if (series->count == 0) memcpy(series->currency, flow.money.currency,
                                   MILENA_CURRENCY_CODE_SIZE);
    series->items[series->count++] = flow;
    return MILENA_OK;
}

static MilenaStatus cash_flow_npv_at_rate(const MilenaCashFlowSeries *series,
                                          const MilenaRate *rate,
                                          int32_t output_scale,
                                          MilenaRoundingMode mode,
                                          MilenaDecimal *out,
                                          MilenaError *error) {
    if (!series || series->count == 0 || !out) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Serie de flujos vacía");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal total;
    MilenaStatus status = milena_decimal_from_i64(&total, 0, error);
    for (size_t i = 0; status == MILENA_OK && i < series->count; i++) {
        MilenaDecimal value = series->items[i].money.amount;
        if (i > 0) status = milena_present_value(&value, &value, rate,
                                                  (uint32_t)i, output_scale,
                                                  mode, error);
        if (status == MILENA_OK) status = milena_decimal_add(&total, &total, &value, error);
    }
    if (status == MILENA_OK) status = milena_decimal_round(out, &total,
                                                            output_scale, mode, error);
    return status;
}

MilenaStatus milena_cash_flow_npv(const MilenaCashFlowSeries *series,
                                  const MilenaRate *periodic_rate,
                                  int32_t output_scale,
                                  MilenaRoundingMode mode,
                                  MilenaDecimal *out, MilenaError *error) {
    return cash_flow_npv_at_rate(series, periodic_rate, output_scale, mode,
                                 out, error);
}

MilenaStatus milena_cash_flow_irr(const MilenaCashFlowSeries *series,
                                  int32_t output_scale,
                                  MilenaRoundingMode mode,
                                  MilenaDecimal *out, MilenaError *error) {
    if (!series || series->count < 2 || !out || output_scale < 0 ||
        output_scale > MILENA_DECIMAL_MAX_SCALE) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Parámetros de IRR inválidos");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDecimal low;
    MilenaDecimal high;
    MilenaDecimal one;
    MilenaDecimal denominator;
    MilenaDecimal low_npv;
    MilenaDecimal high_npv;
    MilenaRate low_rate;
    MilenaRate high_rate;
    MilenaStatus status = milena_decimal_from_string(&low, "-0.999999", error);
    if (status == MILENA_OK) status = milena_decimal_from_i64(&high, 10, error);
    if (status == MILENA_OK) status = decimal_one(&one, error);
    if (status == MILENA_OK) status = milena_rate_init(&low_rate, low,
                                                        MILENA_RATE_PERIODIC, 1, error);
    if (status == MILENA_OK) status = milena_rate_init(&high_rate, high,
                                                        MILENA_RATE_PERIODIC, 1, error);
    if (status == MILENA_OK) status = cash_flow_npv_at_rate(series, &low_rate,
                                                             output_scale + 4,
                                                             mode, &low_npv, error);
    if (status == MILENA_OK) status = cash_flow_npv_at_rate(series, &high_rate,
                                                             output_scale + 4,
                                                             mode, &high_npv, error);
    if (status != MILENA_OK) return status;
    int low_sign = low_npv.coefficient < 0 ? -1 : (low_npv.coefficient > 0 ? 1 : 0);
    int high_sign = high_npv.coefficient < 0 ? -1 : (high_npv.coefficient > 0 ? 1 : 0);
    if (low_sign == high_sign && low_sign != 0) {
        finance_error(error, MILENA_ERR_DATA, "No se encontró un intervalo para IRR");
        return MILENA_ERR_DATA;
    }
    for (size_t iteration = 0; iteration < 96; iteration++) {
        MilenaDecimal sum;
        MilenaDecimal midpoint;
        MilenaDecimal two;
        status = milena_decimal_add(&sum, &low, &high, error);
        if (status == MILENA_OK) status = milena_decimal_from_i64(&two, 2, error);
        if (status == MILENA_OK) status = milena_decimal_div(&midpoint, &sum, &two,
                                                              output_scale + 6,
                                                              MILENA_ROUND_HALF_EVEN, error);
        if (status != MILENA_OK) return status;
        MilenaRate midpoint_rate;
        MilenaDecimal midpoint_npv;
        status = milena_rate_init(&midpoint_rate, midpoint, MILENA_RATE_PERIODIC, 1, error);
        if (status == MILENA_OK) status = cash_flow_npv_at_rate(series, &midpoint_rate,
                                                                 output_scale + 4,
                                                                 mode, &midpoint_npv, error);
        if (status != MILENA_OK) return status;
        int midpoint_sign = midpoint_npv.coefficient < 0 ? -1 :
            (midpoint_npv.coefficient > 0 ? 1 : 0);
        if (midpoint_sign == 0) {
            low = midpoint;
            high = midpoint;
            break;
        }
        if (midpoint_sign == low_sign) {
            low = midpoint;
            low_sign = midpoint_sign;
        } else {
            high = midpoint;
            high_sign = midpoint_sign;
        }
    }
    status = milena_decimal_add(&denominator, &low, &high, error);
    if (status == MILENA_OK) status = milena_decimal_from_i64(&one, 2, error);
    if (status == MILENA_OK) status = milena_decimal_div(out, &denominator, &one,
                                                          output_scale, mode, error);
    return status;
}

void milena_amortization_schedule_init(MilenaAmortizationSchedule *schedule) {
    if (schedule) memset(schedule, 0, sizeof(*schedule));
}

void milena_amortization_schedule_destroy(MilenaAmortizationSchedule *schedule) {
    if (!schedule) return;
    free(schedule->rows);
    memset(schedule, 0, sizeof(*schedule));
}

MilenaStatus milena_date_add_months(MilenaDate *out, MilenaDate date,
                                    uint32_t months, MilenaError *error) {
    MilenaDate validated;
    MilenaStatus status = milena_date_init(&validated, date.year, date.month,
                                           date.day, error);
    if (status != MILENA_OK) return status;
    date = validated;
    uint64_t total = (uint64_t)(date.year < 0 ? 0 : date.year) * 12u +
                     (uint64_t)(date.month - 1u) + months;
    int32_t year = (int32_t)(total / 12u);
    unsigned month = (unsigned)(total % 12u + 1u);
    unsigned day = date.day > date_month_days(year, month) ?
        date_month_days(year, month) : date.day;
    return milena_date_init(out, year, month, day, error);
}

MilenaStatus milena_date_add_period_policy(
    MilenaDate *out, MilenaDate date, uint32_t periods,
    MilenaPaymentFrequency frequency, MilenaMonthEndPolicy policy,
    MilenaError *error) {
    uint32_t multiplier = 0;
    switch (frequency) {
        case MILENA_PAYMENT_MONTHLY: multiplier = 1u; break;
        case MILENA_PAYMENT_QUARTERLY: multiplier = 3u; break;
        case MILENA_PAYMENT_SEMIANNUAL: multiplier = 6u; break;
        case MILENA_PAYMENT_ANNUAL: multiplier = 12u; break;
        default:
            finance_error(error, MILENA_ERR_ARGUMENT, "Frecuencia de calendario inválida");
            return MILENA_ERR_ARGUMENT;
    }
    if (periods > UINT32_MAX / multiplier) {
        finance_error(error, MILENA_ERR_OVERFLOW, "Período de calendario fuera de rango");
        return MILENA_ERR_OVERFLOW;
    }
    if (policy < MILENA_MONTH_END_PRESERVE_DAY ||
        policy > MILENA_MONTH_END_STICK_TO_END) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Política de fin de mes inválida");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaDate validated;
    MilenaStatus status = milena_date_init(&validated, date.year, date.month,
                                           date.day, error);
    if (status != MILENA_OK) return status;
    date = validated;
    bool source_is_month_end = date.day == date_month_days(date.year, date.month);
    status = milena_date_add_months(out, date, periods * multiplier, error);
    if (status != MILENA_OK) return status;
    if (policy == MILENA_MONTH_END_STICK_TO_END && source_is_month_end) {
        out->day = date_month_days(out->year, out->month);
    }
    return MILENA_OK;
}

MilenaStatus milena_date_add_period(MilenaDate *out, MilenaDate date,
                                    uint32_t periods,
                                    MilenaPaymentFrequency frequency,
                                    MilenaError *error) {
    return milena_date_add_period_policy(out, date, periods, frequency,
                                         MILENA_MONTH_END_PRESERVE_DAY, error);
}

MilenaStatus milena_amortization_build_frequency_policy(
    MilenaAmortizationSchedule *schedule,
    MilenaMoney principal,
    const MilenaRate *periodic_rate,
    uint32_t periods,
    MilenaDate first_payment_date,
    MilenaPaymentFrequency frequency,
    MilenaMonthEndPolicy policy,
    MilenaError *error) {
    if (!schedule || periods == 0 || principal.amount.coefficient < 0 ||
        require_periodic_rate(periodic_rate, error) != MILENA_OK) {
        finance_error(error, MILENA_ERR_ARGUMENT, "Parámetros de amortización inválidos");
        return MILENA_ERR_ARGUMENT;
    }
    milena_amortization_schedule_init(schedule);
    MilenaDecimal payment_decimal;
    MilenaStatus status = milena_annuity_payment(&payment_decimal, &principal.amount,
                                                  periodic_rate, periods, 2,
                                                  MILENA_ROUND_HALF_EVEN, error);
    if (status != MILENA_OK) return status;
    MilenaDecimal balance = principal.amount;
    for (uint32_t period = 1; period <= periods; period++) {
        MilenaDate date;
        status = milena_date_add_period_policy(&date, first_payment_date,
                                                period - 1u, frequency, policy,
                                                error);
        if (status != MILENA_OK) break;
        MilenaDecimal interest;
        MilenaDecimal principal_paid;
        MilenaDecimal payment = payment_decimal;
        status = milena_decimal_mul(&interest, &balance, &periodic_rate->value, error);
        if (status == MILENA_OK) status = milena_decimal_sub(&principal_paid, &payment, &interest, error);
        if (status == MILENA_OK && period == periods) {
            principal_paid = balance;
            status = milena_decimal_add(&payment, &interest, &principal_paid, error);
        }
        MilenaDecimal next_balance;
        if (status == MILENA_OK) status = milena_decimal_sub(&next_balance, &balance,
                                                              &principal_paid, error);
        if (status != MILENA_OK) break;
        if (next_balance.coefficient < 0) {
            next_balance.coefficient = 0;
            next_balance.scale = 0;
        }
        if (schedule->count == schedule->capacity) {
            size_t capacity = schedule->capacity == 0 ? 8 : schedule->capacity * 2;
            MilenaAmortizationRow *rows = (MilenaAmortizationRow *)realloc(
                schedule->rows, capacity * sizeof(*rows));
            if (!rows) {
                status = MILENA_ERR_MEMORY;
                finance_error(error, status, "No se pudo reservar el calendario de amortización");
                break;
            }
            schedule->rows = rows;
            schedule->capacity = capacity;
        }
        MilenaAmortizationRow *row = &schedule->rows[schedule->count];
        row->period = period;
        row->date = date;
        status = milena_money_init(&row->payment, payment, principal.currency, error);
        if (status == MILENA_OK) status = milena_money_init(&row->interest, interest, principal.currency, error);
        if (status == MILENA_OK) status = milena_money_init(&row->principal, principal_paid, principal.currency, error);
        if (status == MILENA_OK) status = milena_money_init(&row->balance, next_balance, principal.currency, error);
        if (status != MILENA_OK) break;
        schedule->count++;
        balance = next_balance;
    }
    if (status != MILENA_OK) milena_amortization_schedule_destroy(schedule);
    return status;
}

MilenaStatus milena_amortization_build_frequency(
    MilenaAmortizationSchedule *schedule,
    MilenaMoney principal,
    const MilenaRate *periodic_rate,
    uint32_t periods,
    MilenaDate first_payment_date,
    MilenaPaymentFrequency frequency,
    MilenaError *error) {
    return milena_amortization_build_frequency_policy(
        schedule, principal, periodic_rate, periods, first_payment_date,
        frequency, MILENA_MONTH_END_PRESERVE_DAY, error);
}

MilenaStatus milena_amortization_build(MilenaAmortizationSchedule *schedule,
                                        MilenaMoney principal,
                                        const MilenaRate *periodic_rate,
                                        uint32_t periods,
                                        MilenaDate first_payment_date,
                                        MilenaError *error) {
    return milena_amortization_build_frequency(schedule, principal, periodic_rate,
                                                periods, first_payment_date,
                                                MILENA_PAYMENT_MONTHLY, error);
}
