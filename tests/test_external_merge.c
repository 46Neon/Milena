#include "external_merge.h"

#include <assert.h>

static void put_double(double value, unsigned char out[8]) {
    uint64_t bits = 0; memcpy(&bits, &value, sizeof(bits));
    for (size_t i = 0; i < 8; i++) out[i] = (unsigned char)(bits >> (8u * i));
}
static double get_double(const unsigned char in[8]) {
    uint64_t bits = 0; for (size_t i = 0; i < 8; i++) bits |= ((uint64_t)in[i]) << (8u * i);
    double value = 0.0; memcpy(&value, &bits, sizeof(value)); return value;
}
static void make_run(const char *path, const double *values, size_t count) {
    MilenaSpillStore store = {0}; MilenaError error; milena_error_clear(&error);
    assert(milena_spill_store_open(path, 4096, sizeof(double), &store, &error) == MILENA_OK);
    for (size_t i = 0; i < count; i++) { unsigned char record[8]; put_double(values[i], record); assert(milena_spill_store_append(&store, record, sizeof(record), &error) == MILENA_OK); }
    assert(milena_spill_store_close(&store, &error) == MILENA_OK);
}
typedef struct { const double *expected; size_t count; size_t index; } Check;
static MilenaStatus check_value(const void *data, size_t length, size_t index,
                                void *context, MilenaError *error) {
    (void)error; Check *check = (Check *)context;
    assert(length == sizeof(double) && index == check->index && index < check->count);
    double value = get_double((const unsigned char *)data);
    assert(value == check->expected[index]); check->index++;
    return MILENA_OK;
}

int main(void) {
    const char *run_a = "milena-run-a.spill";
    const char *run_b = "milena-run-b.spill";
    const char *output = "milena-merged.spill";
    const char *bad = "milena-unsorted.spill";
    (void)remove(run_a); (void)remove(run_b); (void)remove(output);
    (void)remove("milena-merged.spill.partial"); (void)remove(bad);
    const double a[] = {1.0, 3.0, 5.0}, b[] = {2.0, 4.0, 6.0};
    make_run(run_a, a, 3); make_run(run_b, b, 3);
    const char *runs[] = {run_a, run_b};
    MilenaError error; milena_error_clear(&error);
    assert(milena_external_merge_double_runs(runs, 2, output, 2, 4096, &error) == MILENA_OK);
    const double expected[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    Check check = {expected, 6, 0};
    assert(milena_spill_store_visit(output, 4096, sizeof(double), check_value, &check, &error) == MILENA_OK);
    assert(check.index == 6);
    assert(milena_external_merge_double_runs(runs, 2, output, 2, 4096, &error) == MILENA_ERR_ARGUMENT);
    assert(milena_external_merge_double_runs(runs, 2, "milena-too-small.spill", 2, 50, &error) == MILENA_ERR_OVERFLOW);

    const double unsorted[] = {3.0, 1.0}; make_run(bad, unsorted, 2);
    const char *bad_runs[] = {bad};
    assert(milena_external_merge_double_runs(bad_runs, 1, "milena-bad-output.spill", 2, 4096, &error) == MILENA_ERR_DATA);
    assert(fopen("milena-bad-output.spill", "rb") == NULL);
    (void)remove(run_a); (void)remove(run_b); (void)remove(output);
    (void)remove("milena-merged.spill.partial"); (void)remove("milena-too-small.spill.partial");
    (void)remove(bad); (void)remove("milena-bad-output.spill.partial");
    puts("external merge tests passed");
    return 0;
}
