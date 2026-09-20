#include "array.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail_status(MilenaStatus status, const MilenaError *error,
                        const char *file, int line) {
    if (status != MILENA_OK) {
        fprintf(stderr, "%s:%d: unexpected %s: %s\n", file, line,
                milena_status_name(status), error->message);
        abort();
    }
}

#define OK(call) fail_status((call), &error, __FILE__, __LINE__)
#define EXPECT(call, wanted) do {                                             \
    MilenaStatus actual_ = (call);                                            \
    if (actual_ != (wanted)) {                                                \
        fprintf(stderr, "%s:%d: expected %s, got %s (%s)\n",               \
                __FILE__, __LINE__, milena_status_name(wanted),               \
                milena_status_name(actual_), error.message);                  \
        abort();                                                               \
    }                                                                          \
} while (0)

static void from_raw(MilenaArray *out, MilenaDType dtype, size_t ndim,
                     const size_t *shape, const void *values,
                     MilenaError *error) {
    fail_status(milena_array_zeros(out, dtype, ndim, shape, error), error,
                __FILE__, __LINE__);
    if (out->size != 0) {
        void *destination = NULL;
        fail_status(milena_array_mut_data(out, &destination, error), error,
                    __FILE__, __LINE__);
        memcpy(destination, values, out->size * out->itemsize);
    }
}

static void contiguous_copy(MilenaArray *out, const MilenaArray *source,
                            MilenaError *error) {
    fail_status(milena_array_cast(out, source, source->dtype, error), error,
                __FILE__, __LINE__);
}

static double scalar_f64(const MilenaArray *array) {
    assert(array->dtype == MILENA_DTYPE_FLOAT64 && array->size == 1);
    return *(const double *)milena_array_const_data(array);
}

static int64_t scalar_i64(const MilenaArray *array) {
    assert(array->dtype == MILENA_DTYPE_INT64 && array->size == 1);
    return *(const int64_t *)milena_array_const_data(array);
}

static uint64_t scalar_u64(const MilenaArray *array) {
    assert(array->dtype == MILENA_DTYPE_UINT64 && array->size == 1);
    return *(const uint64_t *)milena_array_const_data(array);
}

static void test_promotion_matrix(void) {
    MilenaError error;
    milena_error_clear(&error);
    const MilenaDType types[] = {
        MILENA_DTYPE_BOOL, MILENA_DTYPE_INT8, MILENA_DTYPE_INT16,
        MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT8,
        MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64,
        MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64
    };
    const MilenaDType expected[11][11] = {
        {MILENA_DTYPE_UINT8, MILENA_DTYPE_INT16, MILENA_DTYPE_INT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT8, MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_INT16, MILENA_DTYPE_INT8, MILENA_DTYPE_INT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_INT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_INT16, MILENA_DTYPE_INT16, MILENA_DTYPE_INT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_INT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_INT32, MILENA_DTYPE_INT32, MILENA_DTYPE_INT32, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_INT32, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_UINT8, MILENA_DTYPE_INT16, MILENA_DTYPE_INT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT8, MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_UINT16, MILENA_DTYPE_INT32, MILENA_DTYPE_INT32, MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_UINT32, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_UINT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_UINT64, MILENA_DTYPE_UINT64, MILENA_DTYPE_UINT64, MILENA_DTYPE_UINT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64},
        {MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64, MILENA_DTYPE_FLOAT64}
    };
    for (size_t row = 0; row < 11; ++row) {
        for (size_t column = 0; column < 11; ++column) {
            MilenaDType promoted = MILENA_DTYPE_BOOL;
            OK(milena_dtype_promote(types[row], types[column], &promoted,
                                    &error));
            assert(promoted == expected[row][column]);
        }
    }
    MilenaDType ignored = MILENA_DTYPE_BOOL;
    EXPECT(milena_dtype_promote(MILENA_DTYPE_COMPLEX64,
                                MILENA_DTYPE_FLOAT64, &ignored, &error),
           MILENA_ERR_UNSUPPORTED);
}

static void test_casts_and_atomic_failure(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {4};
    const double values[] = {3.9, -3.9, 0.0, 255.0};
    MilenaArray source = {0}, i16 = {0}, boolean = {0};
    from_raw(&source, MILENA_DTYPE_FLOAT64, 1, shape, values, &error);
    OK(milena_array_cast(&i16, &source, MILENA_DTYPE_INT16, &error));
    const int16_t expected[] = {3, -3, 0, 255};
    assert(memcmp(milena_array_const_data(&i16), expected,
                  sizeof(expected)) == 0);
    OK(milena_array_cast(&boolean, &source, MILENA_DTYPE_BOOL, &error));
    const bool *truth = (const bool *)milena_array_const_data(&boolean);
    assert(truth[0] && truth[1] && !truth[2] && truth[3]);

    const size_t one[] = {1};
    const double nan_value[] = {NAN};
    MilenaArray nan_array = {0}, old = {0};
    const int64_t sentinel[] = {77};
    from_raw(&nan_array, MILENA_DTYPE_FLOAT64, 1, one, nan_value, &error);
    from_raw(&old, MILENA_DTYPE_INT64, 1, one, sentinel, &error);
    MilenaArrayStorage *old_storage = old.storage;
    EXPECT(milena_array_cast(&old, &nan_array, MILENA_DTYPE_INT64, &error),
           MILENA_ERR_TYPE);
    assert(old.storage == old_storage && scalar_i64(&old) == 77);
    OK(milena_array_cast(&boolean, &nan_array, MILENA_DTYPE_BOOL, &error));
    assert(*(const bool *)milena_array_const_data(&boolean));

    const double infinity[] = {INFINITY};
    MilenaArray inf_array = {0};
    from_raw(&inf_array, MILENA_DTYPE_FLOAT64, 1, one, infinity, &error);
    EXPECT(milena_array_cast(&old, &inf_array, MILENA_DTYPE_UINT64, &error),
           MILENA_ERR_TYPE);
    const double huge[] = {(double)FLT_MAX * 2.0};
    MilenaArray huge_array = {0};
    from_raw(&huge_array, MILENA_DTYPE_FLOAT64, 1, one, huge, &error);
    EXPECT(milena_array_cast(&old, &huge_array, MILENA_DTYPE_FLOAT32, &error),
           MILENA_ERR_OVERFLOW);
    const uint64_t maximum[] = {UINT64_MAX};
    MilenaArray u64 = {0};
    from_raw(&u64, MILENA_DTYPE_UINT64, 1, one, maximum, &error);
    EXPECT(milena_array_cast(&old, &u64, MILENA_DTYPE_INT64, &error),
           MILENA_ERR_OVERFLOW);

    MilenaArray complex = {0};
    OK(milena_array_zeros(&complex, MILENA_DTYPE_COMPLEX64, 1, one, &error));
    EXPECT(milena_array_cast(&old, &complex, MILENA_DTYPE_FLOAT32, &error),
           MILENA_ERR_UNSUPPORTED);

    milena_array_release(&complex); milena_array_release(&u64);
    milena_array_release(&huge_array); milena_array_release(&inf_array);
    milena_array_release(&old); milena_array_release(&nan_array);
    milena_array_release(&boolean); milena_array_release(&i16);
    milena_array_release(&source);
}

static void test_arithmetic_overflow_and_division(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t two[] = {2};
    const int8_t signed_values[] = {-1, 120};
    const uint8_t unsigned_values[] = {250, 10};
    MilenaArray a = {0}, b = {0}, result = {0};
    from_raw(&a, MILENA_DTYPE_INT8, 1, two, signed_values, &error);
    from_raw(&b, MILENA_DTYPE_UINT8, 1, two, unsigned_values, &error);
    OK(milena_array_add(&result, &a, &b, &error));
    assert(result.dtype == MILENA_DTYPE_INT16);
    const int16_t mixed_expected[] = {249, 130};
    assert(memcmp(milena_array_const_data(&result), mixed_expected,
                  sizeof(mixed_expected)) == 0);

    const size_t one[] = {1};
    const int8_t x_value[] = {120}, y_value[] = {10};
    MilenaArray x = {0}, y = {0};
    from_raw(&x, MILENA_DTYPE_INT8, 1, one, x_value, &error);
    from_raw(&y, MILENA_DTYPE_INT8, 1, one, y_value, &error);
    EXPECT(milena_array_add(&result, &x, &y, &error), MILENA_ERR_OVERFLOW);
    assert(error.code == MILENA_ERR_OVERFLOW);
    assert(strcmp(error.message,
                  "La operación entera está fuera de rango") == 0);
    const uint8_t low[] = {1}, high[] = {2};
    MilenaArray u_low = {0}, u_high = {0};
    from_raw(&u_low, MILENA_DTYPE_UINT8, 1, one, low, &error);
    from_raw(&u_high, MILENA_DTYPE_UINT8, 1, one, high, &error);
    EXPECT(milena_array_subtract(&result, &u_low, &u_high, &error),
           MILENA_ERR_OVERFLOW);

    const int64_t dividend[] = {-7}, divisor[] = {2}, zero[] = {0};
    MilenaArray p = {0}, q = {0}, z = {0};
    from_raw(&p, MILENA_DTYPE_INT64, 1, one, dividend, &error);
    from_raw(&q, MILENA_DTYPE_INT64, 1, one, divisor, &error);
    from_raw(&z, MILENA_DTYPE_INT64, 1, one, zero, &error);
    OK(milena_array_divide(&result, &p, &q, &error));
    assert(result.dtype == MILENA_DTYPE_FLOAT64);
    assert(scalar_f64(&result) == -3.5);
    EXPECT(milena_array_divide(&result, &p, &z, &error),
           MILENA_ERR_ARGUMENT);

    const uint64_t umax[] = {UINT64_MAX};
    const int64_t minus_one[] = {-1};
    MilenaArray large = {0}, negative = {0};
    from_raw(&large, MILENA_DTYPE_UINT64, 1, one, umax, &error);
    from_raw(&negative, MILENA_DTYPE_INT64, 1, one, minus_one, &error);
    OK(milena_array_add(&result, &large, &negative, &error));
    assert(result.dtype == MILENA_DTYPE_FLOAT64);
    assert(isfinite(scalar_f64(&result)));

    MilenaArray complex = {0};
    OK(milena_array_zeros(&complex, MILENA_DTYPE_COMPLEX128, 1, one, &error));
    EXPECT(milena_array_add(&result, &complex, &complex, &error),
           MILENA_ERR_UNSUPPORTED);

    milena_array_release(&complex); milena_array_release(&negative);
    milena_array_release(&large); milena_array_release(&z);
    milena_array_release(&q); milena_array_release(&p);
    milena_array_release(&u_high); milena_array_release(&u_low);
    milena_array_release(&y); milena_array_release(&x);
    milena_array_release(&result); milena_array_release(&b);
    milena_array_release(&a);
}

static void test_broadcast_and_strided_kernels(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t matrix_shape[] = {2, 3};
    const int16_t matrix_values[] = {1, 2, 3, 4, 5, 6};
    const size_t vector_shape[] = {3};
    const uint8_t vector_values[] = {10, 20, 30};
    MilenaArray matrix = {0}, vector = {0}, sum = {0};
    from_raw(&matrix, MILENA_DTYPE_INT16, 2, matrix_shape,
             matrix_values, &error);
    from_raw(&vector, MILENA_DTYPE_UINT8, 1, vector_shape,
             vector_values, &error);
    OK(milena_array_add(&sum, &matrix, &vector, &error));
    const int16_t expected[] = {11, 22, 33, 14, 25, 36};
    assert(memcmp(milena_array_const_data(&sum), expected,
                  sizeof(expected)) == 0);

    const size_t axes[] = {1, 0};
    MilenaArray transposed = {0}, doubled = {0}, copy = {0};
    OK(milena_array_transpose_view(&transposed, &matrix, axes, &error));
    OK(milena_array_add(&doubled, &transposed, &transposed, &error));
    const int16_t transposed_expected[] = {2, 8, 4, 10, 6, 12};
    assert(memcmp(milena_array_const_data(&doubled), transposed_expected,
                  sizeof(transposed_expected)) == 0);

    MilenaSlice reverse_slice = {false, 0, false, 0, -1};
    MilenaArray reverse = {0}, reverse_sum = {0};
    OK(milena_array_slice_view_ex(&reverse, &matrix, 1,
                                  &reverse_slice, &error));
    OK(milena_array_add(&reverse_sum, &reverse, &matrix, &error));
    const int16_t reverse_expected[] = {4, 4, 4, 10, 10, 10};
    assert(memcmp(milena_array_const_data(&reverse_sum), reverse_expected,
                  sizeof(reverse_expected)) == 0);

    const size_t scalar_shape[] = {1, 1};
    const int16_t scalar_value[] = {7};
    const size_t target_shape[] = {2, 3};
    MilenaArray scalar = {0}, broadcast = {0};
    from_raw(&scalar, MILENA_DTYPE_INT16, 2, scalar_shape,
             scalar_value, &error);
    OK(milena_array_broadcast_view(&broadcast, &scalar, 2,
                                   target_shape, &error));
    assert(broadcast.strides[0] == 0 && broadcast.strides[1] == 0);
    OK(milena_array_add(&copy, &matrix, &broadcast, &error));
    const int16_t plus_seven[] = {8, 9, 10, 11, 12, 13};
    assert(memcmp(milena_array_const_data(&copy), plus_seven,
                  sizeof(plus_seven)) == 0);

    /* Metamorphic: x + 0 == x, including a non-contiguous x. */
    const int16_t zero_value[] = {0};
    MilenaArray zero = {0}, identity = {0}, identity_copy = {0};
    from_raw(&zero, MILENA_DTYPE_INT16, 2, scalar_shape, zero_value, &error);
    OK(milena_array_add(&identity, &transposed, &zero, &error));
    contiguous_copy(&identity_copy, &transposed, &error);
    assert(memcmp(milena_array_const_data(&identity),
                  milena_array_const_data(&identity_copy),
                  identity.size * identity.itemsize) == 0);

    milena_array_release(&identity_copy); milena_array_release(&identity);
    milena_array_release(&zero); milena_array_release(&broadcast);
    milena_array_release(&scalar); milena_array_release(&reverse_sum);
    milena_array_release(&reverse); milena_array_release(&copy);
    milena_array_release(&doubled); milena_array_release(&transposed);
    milena_array_release(&sum); milena_array_release(&vector);
    milena_array_release(&matrix);
}

static void test_comparisons_and_predicates(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {4};
    const double floating[] = {NAN, INFINITY, -1.0, 2.0};
    const int16_t integers[] = {0, 1, -1, 3};
    MilenaArray a = {0}, b = {0}, equal = {0}, less = {0}, greater = {0};
    MilenaArray isnan_result = {0}, finite_result = {0};
    from_raw(&a, MILENA_DTYPE_FLOAT64, 1, shape, floating, &error);
    from_raw(&b, MILENA_DTYPE_INT16, 1, shape, integers, &error);
    OK(milena_array_equal(&equal, &a, &b, &error));
    OK(milena_array_less(&less, &a, &b, &error));
    OK(milena_array_greater(&greater, &a, &b, &error));
    const bool expected_equal[] = {false, false, true, false};
    const bool expected_less[] = {false, false, false, true};
    const bool expected_greater[] = {false, true, false, false};
    assert(memcmp(milena_array_const_data(&equal), expected_equal,
                  sizeof(expected_equal)) == 0);
    assert(memcmp(milena_array_const_data(&less), expected_less,
                  sizeof(expected_less)) == 0);
    assert(memcmp(milena_array_const_data(&greater), expected_greater,
                  sizeof(expected_greater)) == 0);
    OK(milena_array_isnan(&isnan_result, &a, &error));
    OK(milena_array_isfinite(&finite_result, &a, &error));
    const bool expected_nan[] = {true, false, false, false};
    const bool expected_finite[] = {false, false, true, true};
    assert(memcmp(milena_array_const_data(&isnan_result), expected_nan,
                  sizeof(expected_nan)) == 0);
    assert(memcmp(milena_array_const_data(&finite_result), expected_finite,
                  sizeof(expected_finite)) == 0);
    OK(milena_array_isfinite(&finite_result, &b, &error));
    const bool *all_finite = (const bool *)milena_array_const_data(&finite_result);
    for (size_t index = 0; index < 4; ++index) assert(all_finite[index]);

    milena_array_release(&finite_result); milena_array_release(&isnan_result);
    milena_array_release(&greater); milena_array_release(&less);
    milena_array_release(&equal); milena_array_release(&b);
    milena_array_release(&a);
}

static void test_reductions_and_stability(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {2, 3};
    const int32_t values[] = {1, 2, 3, 4, 5, 6};
    MilenaArray source = {0}, total = {0}, axis = {0}, product = {0};
    from_raw(&source, MILENA_DTYPE_INT32, 2, shape, values, &error);
    OK(milena_array_sum(&total, &source, -1, false, &error));
    assert(total.dtype == MILENA_DTYPE_INT64 && scalar_i64(&total) == 21);
    OK(milena_array_sum(&axis, &source, 1, true, &error));
    const int64_t row_sums[] = {6, 15};
    assert(axis.ndim == 2 && axis.shape[0] == 2 && axis.shape[1] == 1);
    assert(memcmp(milena_array_const_data(&axis), row_sums,
                  sizeof(row_sums)) == 0);
    OK(milena_array_sum(&axis, &source, -2, false, &error));
    const int64_t column_sums[] = {5, 7, 9};
    assert(axis.ndim == 1 && axis.shape[0] == 3);
    assert(memcmp(milena_array_const_data(&axis), column_sums,
                  sizeof(column_sums)) == 0);
    OK(milena_array_prod(&product, &source, -1, false, &error));
    assert(scalar_i64(&product) == 720);

    const size_t stable_shape[] = {3};
    const double stable_values[] = {1.0e16, 1.0, -1.0e16};
    MilenaArray stable = {0}, mean = {0};
    from_raw(&stable, MILENA_DTYPE_FLOAT64, 1, stable_shape,
             stable_values, &error);
    OK(milena_array_mean(&mean, &stable, &error));
    assert(fabs(scalar_f64(&mean) - (1.0 / 3.0)) < 1e-15);

    const size_t four[] = {4};
    const double stats_values[] = {1, 2, 3, 4};
    MilenaArray stats = {0}, minimum = {0}, maximum = {0};
    MilenaArray variance = {0}, deviation = {0};
    from_raw(&stats, MILENA_DTYPE_FLOAT64, 1, four, stats_values, &error);
    OK(milena_array_min(&minimum, &stats, &error));
    OK(milena_array_max(&maximum, &stats, &error));
    OK(milena_array_variance(&variance, &stats, &error));
    OK(milena_array_std(&deviation, &stats, &error));
    assert(scalar_f64(&minimum) == 1.0 && scalar_f64(&maximum) == 4.0);
    assert(fabs(scalar_f64(&variance) - 1.25) < 1e-15);
    assert(fabs(scalar_f64(&deviation) - sqrt(1.25)) < 1e-15);

    const int64_t overflow_values[] = {INT64_MAX, 1};
    const size_t pair[] = {2};
    MilenaArray overflow = {0};
    from_raw(&overflow, MILENA_DTYPE_INT64, 1, pair,
             overflow_values, &error);
    EXPECT(milena_array_sum(&total, &overflow, -1, false, &error),
           MILENA_ERR_OVERFLOW);

    const size_t empty_shape[] = {2, 0};
    MilenaArray empty = {0}, empty_sum = {0}, empty_prod = {0};
    OK(milena_array_zeros(&empty, MILENA_DTYPE_UINT16, 2,
                          empty_shape, &error));
    OK(milena_array_sum(&empty_sum, &empty, -1, false, &error));
    OK(milena_array_prod(&empty_prod, &empty, -1, false, &error));
    assert(scalar_u64(&empty_sum) == 0u && scalar_u64(&empty_prod) == 1u);
    EXPECT(milena_array_mean(&mean, &empty, &error), MILENA_ERR_ARGUMENT);
    EXPECT(milena_array_min_axis(&minimum, &empty, 1, false, &error),
           MILENA_ERR_ARGUMENT);

    const double nan_values[] = {1.0, NAN};
    MilenaArray nan_array = {0};
    from_raw(&nan_array, MILENA_DTYPE_FLOAT64, 1, pair, nan_values, &error);
    OK(milena_array_min(&minimum, &nan_array, &error));
    assert(isnan(scalar_f64(&minimum)));

    /* Metamorphic: global sum is invariant under transpose. */
    const size_t axes[] = {1, 0};
    MilenaArray transpose = {0}, transpose_sum = {0};
    OK(milena_array_transpose_view(&transpose, &source, axes, &error));
    OK(milena_array_sum(&transpose_sum, &transpose, -1, false, &error));
    assert(scalar_i64(&transpose_sum) == 21);

    milena_array_release(&transpose_sum); milena_array_release(&transpose);
    milena_array_release(&nan_array); milena_array_release(&empty_prod);
    milena_array_release(&empty_sum); milena_array_release(&empty);
    milena_array_release(&overflow); milena_array_release(&deviation);
    milena_array_release(&variance); milena_array_release(&maximum);
    milena_array_release(&minimum); milena_array_release(&stats);
    milena_array_release(&mean); milena_array_release(&stable);
    milena_array_release(&product); milena_array_release(&axis);
    milena_array_release(&total); milena_array_release(&source);
}

static void test_percentiles_and_arg_extrema(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {2, 3};
    const double values[] = {1, 100, 3, 4, 5, 6};
    MilenaArray source = {0}, reverse = {0}, median = {0};
    MilenaArray percentile = {0}, axis = {0}, argmin = {0}, argmax = {0};
    from_raw(&source, MILENA_DTYPE_FLOAT64, 2, shape, values, &error);
    MilenaSlice reversed = {false, 0, false, 0, -1};
    OK(milena_array_slice_view_ex(&reverse, &source, 1, &reversed, &error));
    OK(milena_array_median(&median, &reverse, &error));
    assert(scalar_f64(&median) == 4.5);
    OK(milena_array_percentile(&percentile, &reverse, 25.0, &error));
    assert(scalar_f64(&percentile) == 3.25);
    OK(milena_array_percentile_axis(&axis, &reverse, 50.0, 1, true,
                                    &error));
    const double axis_expected[] = {3.0, 5.0};
    assert(axis.ndim == 2 && axis.shape[0] == 2 && axis.shape[1] == 1);
    assert(memcmp(milena_array_const_data(&axis), axis_expected,
                  sizeof(axis_expected)) == 0);
    EXPECT(milena_array_percentile(&percentile, &source, -0.1, &error),
           MILENA_ERR_ARGUMENT);
    EXPECT(milena_array_percentile(&percentile, &source, 100.1, &error),
           MILENA_ERR_ARGUMENT);
    OK(milena_array_argmin(&argmin, &reverse, &error));
    OK(milena_array_argmax(&argmax, &reverse, &error));
    assert(scalar_i64(&argmin) == 2 && scalar_i64(&argmax) == 1);

    const size_t pair[] = {2};
    const double nan_values[] = {1.0, NAN};
    MilenaArray nan_array = {0};
    from_raw(&nan_array, MILENA_DTYPE_FLOAT64, 1, pair, nan_values, &error);
    OK(milena_array_percentile(&percentile, &nan_array, 50.0, &error));
    assert(isnan(scalar_f64(&percentile)));
    EXPECT(milena_array_argmax(&argmax, &nan_array, &error),
           MILENA_ERR_ARGUMENT);

    milena_array_release(&nan_array); milena_array_release(&argmax);
    milena_array_release(&argmin); milena_array_release(&axis);
    milena_array_release(&percentile); milena_array_release(&median);
    milena_array_release(&reverse); milena_array_release(&source);
}


static void test_mixed_i64_u64_boundaries(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {4};
    const int64_t signed_values[] = {
        INT64_C(9007199254740991), INT64_C(9007199254740992),
        INT64_MAX, -1
    };
    const uint64_t unsigned_values[] = {
        UINT64_C(9007199254740992), UINT64_C(9007199254740993),
        UINT64_C(1) << 63, UINT64_MAX
    };
    MilenaArray signed_array = {0}, unsigned_array = {0};
    MilenaArray add = {0}, subtract = {0}, divide = {0};
    MilenaArray equal = {0}, less = {0}, greater = {0}, reverse_greater = {0};
    from_raw(&signed_array, MILENA_DTYPE_INT64, 1, shape,
             signed_values, &error);
    from_raw(&unsigned_array, MILENA_DTYPE_UINT64, 1, shape,
             unsigned_values, &error);
    OK(milena_array_add(&add, &signed_array, &unsigned_array, &error));
    OK(milena_array_subtract(&subtract, &signed_array, &unsigned_array,
                             &error));
    OK(milena_array_divide(&divide, &signed_array, &unsigned_array, &error));
    OK(milena_array_equal(&equal, &signed_array, &unsigned_array, &error));
    OK(milena_array_less(&less, &signed_array, &unsigned_array, &error));
    OK(milena_array_greater(&greater, &signed_array, &unsigned_array, &error));
    OK(milena_array_greater(&reverse_greater, &unsigned_array, &signed_array,
                            &error));
    const double *add_values = (const double *)milena_array_const_data(&add);
    const double *sub_values =
        (const double *)milena_array_const_data(&subtract);
    const double *div_values =
        (const double *)milena_array_const_data(&divide);
    const bool *equal_values = (const bool *)milena_array_const_data(&equal);
    const bool *less_values = (const bool *)milena_array_const_data(&less);
    const bool *greater_values =
        (const bool *)milena_array_const_data(&greater);
    const bool *reverse_values =
        (const bool *)milena_array_const_data(&reverse_greater);
    for (size_t index = 0; index < 4; ++index) {
        double a = (double)signed_values[index];
        double b = (double)unsigned_values[index];
        assert(add_values[index] == a + b);
        assert(sub_values[index] == a - b);
        assert(div_values[index] == a / b);
        assert(!equal_values[index]);
        assert(less_values[index]);
        assert(!greater_values[index]);
        assert(reverse_values[index]);
    }
    /* Exact integral comparison must not collapse 2^53 and 2^53+1. */
    assert(!equal_values[1] && less_values[1]);
    /* INT64_MAX is strictly below 2^63 despite both rounding to that f64. */
    assert(!equal_values[2] && less_values[2]);

    milena_array_release(&reverse_greater); milena_array_release(&greater);
    milena_array_release(&less); milena_array_release(&equal);
    milena_array_release(&divide); milena_array_release(&subtract);
    milena_array_release(&add); milena_array_release(&unsigned_array);
    milena_array_release(&signed_array);
}

static void test_extra_kernel_edges(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t one[] = {1};
    const int64_t maximum[] = {INT64_MAX}, two[] = {2};
    const int64_t minimum[] = {INT64_MIN}, positive_one[] = {1};
    MilenaArray max_array = {0}, two_array = {0}, min_array = {0};
    MilenaArray one_array = {0}, result = {0};
    from_raw(&max_array, MILENA_DTYPE_INT64, 1, one, maximum, &error);
    from_raw(&two_array, MILENA_DTYPE_INT64, 1, one, two, &error);
    from_raw(&min_array, MILENA_DTYPE_INT64, 1, one, minimum, &error);
    from_raw(&one_array, MILENA_DTYPE_INT64, 1, one, positive_one, &error);
    EXPECT(milena_array_multiply(&result, &max_array, &two_array, &error),
           MILENA_ERR_OVERFLOW);
    EXPECT(milena_array_subtract(&result, &min_array, &one_array, &error),
           MILENA_ERR_OVERFLOW);

    const double numerator[] = {1.0};
    const double positive_zero[] = {0.0};
    const double negative_zero[] = {-0.0};
    MilenaArray n = {0}, pz = {0}, nz = {0};
    from_raw(&n, MILENA_DTYPE_FLOAT64, 1, one, numerator, &error);
    from_raw(&pz, MILENA_DTYPE_FLOAT64, 1, one, positive_zero, &error);
    from_raw(&nz, MILENA_DTYPE_FLOAT64, 1, one, negative_zero, &error);
    EXPECT(milena_array_divide(&result, &n, &pz, &error),
           MILENA_ERR_ARGUMENT);
    EXPECT(milena_array_divide(&result, &n, &nz, &error),
           MILENA_ERR_ARGUMENT);

    const size_t left_shape[] = {2, 3};
    const size_t right_shape[] = {2, 2};
    MilenaArray left = {0}, right = {0};
    OK(milena_array_zeros(&left, MILENA_DTYPE_FLOAT64, 2, left_shape, &error));
    OK(milena_array_zeros(&right, MILENA_DTYPE_FLOAT64, 2, right_shape, &error));
    EXPECT(milena_array_add(&result, &left, &right, &error),
           MILENA_ERR_ARGUMENT);

    milena_array_release(&right); milena_array_release(&left);
    milena_array_release(&nz); milena_array_release(&pz);
    milena_array_release(&n); milena_array_release(&result);
    milena_array_release(&one_array); milena_array_release(&min_array);
    milena_array_release(&two_array); milena_array_release(&max_array);
}

static void test_portable_large_statistics(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t pair[] = {2};
    const double equal_maxima[] = {DBL_MAX, DBL_MAX};
    MilenaArray equal = {0}, mean = {0}, variance = {0}, deviation = {0};
    from_raw(&equal, MILENA_DTYPE_FLOAT64, 1, pair, equal_maxima, &error);
    OK(milena_array_mean(&mean, &equal, &error));
    OK(milena_array_variance(&variance, &equal, &error));
    OK(milena_array_std(&deviation, &equal, &error));
    assert(scalar_f64(&mean) == DBL_MAX);
    assert(scalar_f64(&variance) == 0.0);
    assert(scalar_f64(&deviation) == 0.0);

    const double opposite[] = {DBL_MAX, -DBL_MAX};
    MilenaArray spread = {0};
    from_raw(&spread, MILENA_DTYPE_FLOAT64, 1, pair, opposite, &error);
    OK(milena_array_mean(&mean, &spread, &error));
    assert(scalar_f64(&mean) == 0.0);
    EXPECT(milena_array_variance(&variance, &spread, &error),
           MILENA_ERR_OVERFLOW);
    OK(milena_array_std(&deviation, &spread, &error));
    assert(scalar_f64(&deviation) == DBL_MAX);

    const double same_infinities[] = {INFINITY, INFINITY};
    MilenaArray infinity = {0};
    from_raw(&infinity, MILENA_DTYPE_FLOAT64, 1, pair,
             same_infinities, &error);
    OK(milena_array_mean(&mean, &infinity, &error));
    OK(milena_array_variance(&variance, &infinity, &error));
    assert(isinf(scalar_f64(&mean)) && scalar_f64(&mean) > 0.0);
    assert(isnan(scalar_f64(&variance)));

    const double mixed_infinities[] = {INFINITY, -INFINITY};
    MilenaArray mixed = {0};
    from_raw(&mixed, MILENA_DTYPE_FLOAT64, 1, pair,
             mixed_infinities, &error);
    OK(milena_array_mean(&mean, &mixed, &error));
    assert(isnan(scalar_f64(&mean)));

    const size_t empty_shape[] = {0, 3};
    MilenaArray empty = {0}, empty_output = {0}, global_keepdims = {0};
    OK(milena_array_zeros(&empty, MILENA_DTYPE_FLOAT64, 2,
                          empty_shape, &error));
    OK(milena_array_mean_axis(&empty_output, &empty, 1, false, &error));
    assert(empty_output.ndim == 1 && empty_output.shape[0] == 0 &&
           empty_output.size == 0);
    EXPECT(milena_array_mean_axis(&mean, &equal, 1, false, &error),
           MILENA_ERR_ARGUMENT);
    EXPECT(milena_array_mean_axis(&mean, &equal, -3, false, &error),
           MILENA_ERR_ARGUMENT);
    OK(milena_array_mean_axis(&global_keepdims, &equal, -1, true, &error));
    assert(global_keepdims.ndim == 1 && global_keepdims.shape[0] == 1 &&
           scalar_f64(&global_keepdims) == DBL_MAX);

    milena_array_release(&global_keepdims); milena_array_release(&empty_output);
    milena_array_release(&empty); milena_array_release(&mixed);
    milena_array_release(&infinity); milena_array_release(&spread);
    milena_array_release(&deviation); milena_array_release(&variance);
    milena_array_release(&mean); milena_array_release(&equal);
}

static void test_percentile_extremes_and_ties(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t pair[] = {2};
    const double extremes[] = {-DBL_MAX, DBL_MAX};
    MilenaArray source = {0}, result = {0};
    from_raw(&source, MILENA_DTYPE_FLOAT64, 1, pair, extremes, &error);
    OK(milena_array_percentile(&result, &source, 0.0, &error));
    assert(scalar_f64(&result) == -DBL_MAX);
    OK(milena_array_percentile(&result, &source, 50.0, &error));
    assert(scalar_f64(&result) == 0.0);
    OK(milena_array_percentile(&result, &source, 100.0, &error));
    assert(scalar_f64(&result) == DBL_MAX);

    const double positive[] = {DBL_MAX / 2.0, DBL_MAX};
    MilenaArray same_sign = {0};
    from_raw(&same_sign, MILENA_DTYPE_FLOAT64, 1, pair, positive, &error);
    OK(milena_array_percentile(&result, &same_sign, 50.0, &error));
    assert(isfinite(scalar_f64(&result)) &&
           scalar_f64(&result) == DBL_MAX * 0.75);

    const double left_infinite[] = {-INFINITY, 1.0};
    const double right_infinite[] = {1.0, INFINITY};
    const double both_infinite[] = {-INFINITY, INFINITY};
    MilenaArray left = {0}, right = {0}, both = {0};
    from_raw(&left, MILENA_DTYPE_FLOAT64, 1, pair, left_infinite, &error);
    from_raw(&right, MILENA_DTYPE_FLOAT64, 1, pair, right_infinite, &error);
    from_raw(&both, MILENA_DTYPE_FLOAT64, 1, pair, both_infinite, &error);
    OK(milena_array_percentile(&result, &left, 50.0, &error));
    assert(isinf(scalar_f64(&result)) && scalar_f64(&result) < 0.0);
    OK(milena_array_percentile(&result, &right, 50.0, &error));
    assert(isinf(scalar_f64(&result)) && scalar_f64(&result) > 0.0);
    OK(milena_array_percentile(&result, &both, 50.0, &error));
    assert(isnan(scalar_f64(&result)));
    OK(milena_array_percentile(&result, &both, 0.0, &error));
    assert(isinf(scalar_f64(&result)) && scalar_f64(&result) < 0.0);
    OK(milena_array_percentile(&result, &both, 100.0, &error));
    assert(isinf(scalar_f64(&result)) && scalar_f64(&result) > 0.0);

    const size_t four[] = {4};
    const int64_t ties[] = {9, 1, 1, 9};
    MilenaArray tie_array = {0}, argmin = {0}, argmax = {0};
    from_raw(&tie_array, MILENA_DTYPE_INT64, 1, four, ties, &error);
    OK(milena_array_argmin(&argmin, &tie_array, &error));
    OK(milena_array_argmax(&argmax, &tie_array, &error));
    assert(scalar_i64(&argmin) == 1);
    assert(scalar_i64(&argmax) == 0);

    milena_array_release(&argmax); milena_array_release(&argmin);
    milena_array_release(&tie_array); milena_array_release(&both);
    milena_array_release(&right); milena_array_release(&left);
    milena_array_release(&same_sign); milena_array_release(&result);
    milena_array_release(&source);
}

int main(void) {
    test_promotion_matrix();
    test_casts_and_atomic_failure();
    test_arithmetic_overflow_and_division();
    test_broadcast_and_strided_kernels();
    test_comparisons_and_predicates();
    test_reductions_and_stability();
    test_percentiles_and_arg_extrema();
    test_mixed_i64_u64_boundaries();
    test_extra_kernel_edges();
    test_portable_large_statistics();
    test_percentile_extremes_and_ties();
    puts("OK: worker3 dtype, kernels, broadcasting and reductions");
    return 0;
}
