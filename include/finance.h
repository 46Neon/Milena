#ifndef MILENA_FINANCE_H
#define MILENA_FINANCE_H

#include "common.h"
#include "sst_dates.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MILENA_DECIMAL_MAX_SCALE 18
#define MILENA_CURRENCY_CODE_SIZE 4

typedef struct {
    int64_t coefficient;
    int32_t scale;
} MilenaDecimal;

typedef struct {
    MilenaDecimal amount;
    char currency[MILENA_CURRENCY_CODE_SIZE];
} MilenaMoney;

typedef enum {
    MILENA_RATE_NOMINAL = 0,
    MILENA_RATE_EFFECTIVE,
    MILENA_RATE_PERIODIC
} MilenaRateKind;

typedef struct {
    MilenaDecimal value;
    MilenaRateKind kind;
    uint32_t periods_per_year;
} MilenaRate;

typedef enum {
    MILENA_ROUND_TOWARD_ZERO = 0,
    MILENA_ROUND_HALF_UP,
    MILENA_ROUND_HALF_EVEN,
    MILENA_ROUND_FLOOR,
    MILENA_ROUND_CEILING
} MilenaRoundingMode;

MilenaStatus milena_decimal_from_i64(MilenaDecimal *out, int64_t value,
                                     MilenaError *error);
MilenaStatus milena_decimal_from_string(MilenaDecimal *out, const char *text,
                                        MilenaError *error);
MilenaStatus milena_decimal_add(MilenaDecimal *out, const MilenaDecimal *left,
                                const MilenaDecimal *right, MilenaError *error);
MilenaStatus milena_decimal_sub(MilenaDecimal *out, const MilenaDecimal *left,
                                const MilenaDecimal *right, MilenaError *error);
MilenaStatus milena_decimal_compare(const MilenaDecimal *left,
                                    const MilenaDecimal *right, int *result,
                                    MilenaError *error);
MilenaStatus milena_decimal_round(MilenaDecimal *out,
                                  const MilenaDecimal *value,
                                  int32_t target_scale,
                                  MilenaRoundingMode mode,
                                  MilenaError *error);
MilenaStatus milena_decimal_mul(MilenaDecimal *out,
                                const MilenaDecimal *left,
                                const MilenaDecimal *right,
                                MilenaError *error);
MilenaStatus milena_decimal_div(MilenaDecimal *out,
                                const MilenaDecimal *left,
                                const MilenaDecimal *right,
                                int32_t target_scale,
                                MilenaRoundingMode mode,
                                MilenaError *error);

MilenaStatus milena_money_init(MilenaMoney *out, MilenaDecimal amount,
                               const char *currency, MilenaError *error);
MilenaStatus milena_money_add(MilenaMoney *out, const MilenaMoney *left,
                              const MilenaMoney *right, MilenaError *error);
MilenaStatus milena_money_sub(MilenaMoney *out, const MilenaMoney *left,
                              const MilenaMoney *right, MilenaError *error);

MilenaStatus milena_rate_init(MilenaRate *out, MilenaDecimal value,
                              MilenaRateKind kind, uint32_t periods_per_year,
                              MilenaError *error);
MilenaStatus milena_rate_nominal_to_effective(MilenaDecimal *out,
                                               const MilenaRate *nominal,
                                               int32_t output_scale,
                                               MilenaRoundingMode mode,
                                               MilenaError *error);
MilenaStatus milena_decimal_pow_uint(MilenaDecimal *out,
                                     const MilenaDecimal *base,
                                     uint32_t exponent,
                                     MilenaError *error);
MilenaStatus milena_future_value(MilenaDecimal *out,
                                 const MilenaDecimal *principal,
                                 const MilenaRate *periodic_rate,
                                 uint32_t periods,
                                 MilenaError *error);
MilenaStatus milena_present_value(MilenaDecimal *out,
                                  const MilenaDecimal *future_value,
                                  const MilenaRate *periodic_rate,
                                  uint32_t periods,
                                  int32_t output_scale,
                                  MilenaRoundingMode mode,
                                  MilenaError *error);
MilenaStatus milena_simple_interest(MilenaDecimal *out,
                                    const MilenaDecimal *principal,
                                    const MilenaRate *periodic_rate,
                                    uint32_t periods,
                                    MilenaError *error);
MilenaStatus milena_compound_interest(MilenaDecimal *out,
                                      const MilenaDecimal *principal,
                                      const MilenaRate *periodic_rate,
                                      uint32_t periods,
                                      MilenaError *error);
MilenaStatus milena_annuity_payment(MilenaDecimal *out,
                                    const MilenaDecimal *principal,
                                    const MilenaRate *periodic_rate,
                                    uint32_t periods,
                                    int32_t output_scale,
                                    MilenaRoundingMode mode,
                                    MilenaError *error);

typedef enum {
    MILENA_DAY_COUNT_ACTUAL_365 = 0,
    MILENA_DAY_COUNT_ACTUAL_360
} MilenaDayCount;

MilenaStatus milena_date_init(MilenaDate *out, int32_t year, uint8_t month,
                              uint8_t day, MilenaError *error);
MilenaStatus milena_date_compare(const MilenaDate *left, const MilenaDate *right,
                                 int *result, MilenaError *error);
MilenaStatus milena_date_days_between(const MilenaDate *start,
                                      const MilenaDate *end,
                                      int64_t *days, MilenaError *error);
typedef enum {
    MILENA_PAYMENT_MONTHLY = 0,
    MILENA_PAYMENT_QUARTERLY,
    MILENA_PAYMENT_SEMIANNUAL,
    MILENA_PAYMENT_ANNUAL
} MilenaPaymentFrequency;

typedef enum {
    MILENA_MONTH_END_PRESERVE_DAY = 0,
    MILENA_MONTH_END_STICK_TO_END
} MilenaMonthEndPolicy;

MilenaStatus milena_period_fraction(const MilenaDate *start,
                                    const MilenaDate *end,
                                    MilenaDayCount convention,
                                    MilenaDecimal *out, MilenaError *error);
MilenaStatus milena_date_add_months(MilenaDate *out, MilenaDate date,
                                     uint32_t months, MilenaError *error);
MilenaStatus milena_date_add_period(MilenaDate *out, MilenaDate date,
                                    uint32_t periods,
                                    MilenaPaymentFrequency frequency,
                                    MilenaError *error);
MilenaStatus milena_date_add_period_policy(
    MilenaDate *out, MilenaDate date, uint32_t periods,
    MilenaPaymentFrequency frequency, MilenaMonthEndPolicy policy,
    MilenaError *error);

typedef struct {
    MilenaDate date;
    MilenaMoney money;
} MilenaCashFlow;

typedef struct {
    MilenaCashFlow *items;
    size_t count;
    size_t capacity;
    char currency[MILENA_CURRENCY_CODE_SIZE];
} MilenaCashFlowSeries;

void milena_cash_flow_series_init(MilenaCashFlowSeries *series);
void milena_cash_flow_series_destroy(MilenaCashFlowSeries *series);
MilenaStatus milena_cash_flow_series_add(MilenaCashFlowSeries *series,
                                         MilenaCashFlow flow,
                                         MilenaError *error);
MilenaStatus milena_cash_flow_npv(const MilenaCashFlowSeries *series,
                                  const MilenaRate *periodic_rate,
                                  int32_t output_scale,
                                  MilenaRoundingMode mode,
                                  MilenaDecimal *out, MilenaError *error);
MilenaStatus milena_cash_flow_irr(const MilenaCashFlowSeries *series,
                                  int32_t output_scale,
                                  MilenaRoundingMode mode,
                                  MilenaDecimal *out, MilenaError *error);

typedef struct {
    uint32_t period;
    MilenaDate date;
    MilenaMoney payment;
    MilenaMoney interest;
    MilenaMoney principal;
    MilenaMoney balance;
} MilenaAmortizationRow;

typedef struct {
    MilenaAmortizationRow *rows;
    size_t count;
    size_t capacity;
} MilenaAmortizationSchedule;

void milena_amortization_schedule_init(MilenaAmortizationSchedule *schedule);
void milena_amortization_schedule_destroy(MilenaAmortizationSchedule *schedule);
MilenaStatus milena_amortization_build(MilenaAmortizationSchedule *schedule,
                                        MilenaMoney principal,
                                        const MilenaRate *periodic_rate,
                                        uint32_t periods,
                                        MilenaDate first_payment_date,
                                        MilenaError *error);
MilenaStatus milena_amortization_build_frequency(
    MilenaAmortizationSchedule *schedule,
    MilenaMoney principal,
    const MilenaRate *periodic_rate,
    uint32_t periods,
    MilenaDate first_payment_date,
    MilenaPaymentFrequency frequency,
    MilenaError *error);
MilenaStatus milena_amortization_build_frequency_policy(
    MilenaAmortizationSchedule *schedule,
    MilenaMoney principal,
    const MilenaRate *periodic_rate,
    uint32_t periods,
    MilenaDate first_payment_date,
    MilenaPaymentFrequency frequency,
    MilenaMonthEndPolicy policy,
    MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
