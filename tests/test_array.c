#include "array.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_ok(MilenaStatus status, const MilenaError *error) {
    if (status != MILENA_OK) {
        fprintf(stderr, "array test failed: %s (%s)\n",
                error ? error->message : "sin detalle",
                milena_status_name(status));
        assert(status == MILENA_OK);
    }
}

static void test_dtypes_and_casts(void) {
    const size_t shape[] = {4};
    const int64_t values[] = {-2, 0, 7, 255};
    const MilenaDType dtypes[] = {
        MILENA_DTYPE_BOOL, MILENA_DTYPE_INT8, MILENA_DTYPE_INT16,
        MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT8,
        MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64,
        MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64,
        MILENA_DTYPE_COMPLEX64, MILENA_DTYPE_COMPLEX128
    };
    MilenaError error;
    milena_error_clear(&error);

    for (size_t i = 0; i < sizeof(dtypes) / sizeof(dtypes[0]); i++) {
        MilenaArray zeros = {0};
        expect_ok(milena_array_zeros(&zeros, dtypes[i], 1, shape, &error), &error);
        assert(zeros.size == 4);
        assert(milena_dtype_size(dtypes[i]) == zeros.itemsize);
        assert(strcmp(milena_dtype_name(dtypes[i]), "unknown") != 0);
        milena_array_release(&zeros);
    }

    MilenaArray source = {0};
    MilenaArray small = {0};
    MilenaArray real = {0};
    expect_ok(milena_array_from_i64(&source, 1, shape, values, &error), &error);
    expect_ok(milena_array_cast(&small, &source, MILENA_DTYPE_INT16, &error), &error);
    expect_ok(milena_array_cast(&real, &source, MILENA_DTYPE_FLOAT32, &error), &error);
    assert(((const int16_t *)milena_array_const_data(&small))[3] == 255);
    assert(((const float *)milena_array_const_data(&real))[0] == -2.0f);

    milena_array_release(&real);
    milena_array_release(&small);
    milena_array_release(&source);
}

static void test_creation_and_reshape(void) {
    const size_t shape[] = {2, 3};
    const double values[] = {1, 2, 3, 4, 5, 6};
    MilenaArray array = {0};
    MilenaArray view = {0};
    MilenaError error;
    milena_error_clear(&error);

    expect_ok(milena_array_from_f64(&array, 2, shape, values, &error), &error);
    assert(array.ndim == 2);
    assert(array.size == 6);
    assert(array.shape[0] == 2 && array.shape[1] == 3);
    assert(array.strides[0] == (ptrdiff_t)(3 * sizeof(double)));
    assert(array.strides[1] == (ptrdiff_t)sizeof(double));
    assert(milena_array_is_contiguous(&array));

    const size_t reshaped[] = {3, 2};
    expect_ok(milena_array_reshape_view(&view, &array, 2, reshaped, &error), &error);
    assert(!((view.flags & MILENA_ARRAY_OWN_DATA) != 0));
    assert(view.storage == array.storage);
    assert(((const double *)milena_array_const_data(&view))[4] == 5.0);

    milena_array_release(&view);
    milena_array_release(&array);
}

static void test_broadcast_add(void) {
    const size_t matrix_shape[] = {2, 3};
    const double matrix_values[] = {1, 2, 3, 4, 5, 6};
    const size_t vector_shape[] = {3};
    const double vector_values[] = {10, 20, 30};
    MilenaArray matrix = {0};
    MilenaArray vector = {0};
    MilenaArray result = {0};
    MilenaError error;
    milena_error_clear(&error);

    expect_ok(milena_array_from_f64(&matrix, 2, matrix_shape, matrix_values, &error), &error);
    expect_ok(milena_array_from_f64(&vector, 1, vector_shape, vector_values, &error), &error);
    expect_ok(milena_array_add(&result, &matrix, &vector, &error), &error);

    const double expected[] = {11, 22, 33, 14, 25, 36};
    assert(result.ndim == 2 && result.shape[0] == 2 && result.shape[1] == 3);
    assert(memcmp(milena_array_const_data(&result), expected, sizeof(expected)) == 0);

    milena_array_release(&result);
    milena_array_release(&vector);
    milena_array_release(&matrix);
}

static void test_slice_views(void) {
    const size_t shape[] = {3, 4};
    const double values[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    MilenaArray source = {0};
    MilenaArray columns = {0};
    MilenaArray rows = {0};
    MilenaError error;
    milena_error_clear(&error);

    expect_ok(milena_array_from_f64(&source, 2, shape, values, &error), &error);
    expect_ok(milena_array_slice_view(&columns, &source, 1, 1, 4, 2, &error), &error);
    expect_ok(milena_array_slice_view(&rows, &source, 0, 1, 3, 1, &error), &error);

    const double expected_columns[] = {1, 3, 5, 7, 9, 11};
    const double expected_rows[] = {4, 5, 6, 7, 8, 9, 10, 11};
    assert(columns.shape[0] == 3 && columns.shape[1] == 2);
    assert(columns.strides[1] == (ptrdiff_t)(2 * sizeof(double)));
    assert(rows.shape[0] == 2 && rows.shape[1] == 4);
    for (size_t row = 0; row < columns.shape[0]; row++) {
        for (size_t column = 0; column < columns.shape[1]; column++) {
            size_t index = row * columns.shape[1] + column;
            const unsigned char *base = (const unsigned char *)milena_array_const_data(&columns);
            const double *value = (const double *)(base + row * columns.strides[0] +
                                                   column * columns.strides[1]);
            assert(*value == expected_columns[index]);
        }
    }
    for (size_t row = 0; row < rows.shape[0]; row++) {
        for (size_t column = 0; column < rows.shape[1]; column++) {
            const unsigned char *base = (const unsigned char *)milena_array_const_data(&rows);
            const double *value = (const double *)(base + row * rows.strides[0] +
                                                   column * rows.strides[1]);
            assert(*value == expected_rows[row * rows.shape[1] + column]);
        }
    }

    milena_array_release(&rows);
    milena_array_release(&columns);
    milena_array_release(&source);
}

static void test_transpose_and_reshape_copy(void) {
    const size_t shape[] = {2, 3};
    const double values[] = {1, 2, 3, 4, 5, 6};
    const size_t axes[] = {1, 0};
    const size_t flat_shape[] = {6};
    MilenaArray source = {0};
    MilenaArray transposed = {0};
    MilenaArray flattened = {0};
    MilenaError error;
    milena_error_clear(&error);

    expect_ok(milena_array_from_f64(&source, 2, shape, values, &error), &error);
    expect_ok(milena_array_transpose_view(&transposed, &source, axes, &error), &error);
    expect_ok(milena_array_reshape_copy(&flattened, &transposed, 1, flat_shape, &error), &error);

    const double expected[] = {1, 4, 2, 5, 3, 6};
    assert(transposed.shape[0] == 3 && transposed.shape[1] == 2);
    assert(transposed.strides[0] == (ptrdiff_t)(sizeof(double) * 1));
    assert(transposed.strides[1] == (ptrdiff_t)(sizeof(double) * 3));
    assert(flattened.shape[0] == 6);
    assert(memcmp(milena_array_const_data(&flattened), expected, sizeof(expected)) == 0);

    milena_array_release(&flattened);
    milena_array_release(&transposed);
    milena_array_release(&source);
}

static void test_boolean_masks_and_where(void) {
    const size_t shape[] = {2, 3};
    const double values[] = {1, -2, 3, 4, -5, 6};
    const double true_values[] = {10, 20, 30, 40, 50, 60};
    const double false_values[] = {-10, -20, -30, -40, -50, -60};
    MilenaArray source = {0};
    MilenaArray mask = {0};
    MilenaArray selected = {0};
    MilenaArray indices = {0};
    MilenaArray when_true = {0};
    MilenaArray when_false = {0};
    MilenaArray chosen = {0};
    MilenaError error;
    milena_error_clear(&error);

    expect_ok(milena_array_from_f64(&source, 2, shape, values, &error), &error);
    expect_ok(milena_array_greater_f64(&mask, &source, 0.0, &error), &error);
    expect_ok(milena_array_boolean_mask(&selected, &source, &mask, &error), &error);
    expect_ok(milena_array_nonzero(&indices, &mask, &error), &error);
    expect_ok(milena_array_from_f64(&when_true, 2, shape, true_values, &error), &error);
    expect_ok(milena_array_from_f64(&when_false, 2, shape, false_values, &error), &error);
    expect_ok(milena_array_where(&chosen, &mask, &when_true, &when_false, &error), &error);

    const double expected_selected[] = {1, 3, 4, 6};
    const int64_t expected_indices[] = {0, 2, 3, 5};
    const double expected_chosen[] = {10, -20, 30, 40, -50, 60};
    assert(selected.ndim == 1 && selected.shape[0] == 4);
    assert(memcmp(milena_array_const_data(&selected), expected_selected,
                  sizeof(expected_selected)) == 0);
    assert(indices.shape[0] == 4);
    assert(memcmp(milena_array_const_data(&indices), expected_indices,
                  sizeof(expected_indices)) == 0);
    assert(memcmp(milena_array_const_data(&chosen), expected_chosen,
                  sizeof(expected_chosen)) == 0);

    milena_array_release(&chosen);
    milena_array_release(&when_false);
    milena_array_release(&when_true);
    milena_array_release(&indices);
    milena_array_release(&selected);
    milena_array_release(&mask);
    milena_array_release(&source);
}

static void test_sum_by_axis(void) {
    const size_t shape[] = {2, 3};
    const double values[] = {1, 2, 3, 4, 5, 6};
    MilenaArray array = {0};
    MilenaArray axis0 = {0};
    MilenaArray axis1 = {0};
    MilenaArray total = {0};
    MilenaError error;
    milena_error_clear(&error);

    expect_ok(milena_array_from_f64(&array, 2, shape, values, &error), &error);
    expect_ok(milena_array_sum(&axis0, &array, 0, false, &error), &error);
    expect_ok(milena_array_sum(&axis1, &array, 1, false, &error), &error);
    expect_ok(milena_array_sum(&total, &array, -1, false, &error), &error);

    const double expected_axis0[] = {5, 7, 9};
    const double expected_axis1[] = {6, 15};
    assert(axis0.ndim == 1 && axis0.shape[0] == 3);
    assert(axis1.ndim == 1 && axis1.shape[0] == 2);
    assert(memcmp(milena_array_const_data(&axis0), expected_axis0,
                  sizeof(expected_axis0)) == 0);
    assert(memcmp(milena_array_const_data(&axis1), expected_axis1,
                  sizeof(expected_axis1)) == 0);
    assert(total.ndim == 0);
    assert(((const double *)milena_array_const_data(&total))[0] == 21.0);

    milena_array_release(&total);
    milena_array_release(&axis1);
    milena_array_release(&axis0);
    milena_array_release(&array);
}

static void test_memory_ownership(void) {
    const size_t shape[] = {4};
    const int64_t values[] = {1, 2, 3, 4};
    MilenaArray source = {0}, view = {0}, result = {0}, empty = {0};
    MilenaError error; milena_error_clear(&error);
    expect_ok(milena_array_from_i64(&source, 1, shape, values, &error), &error);
    expect_ok(milena_array_slice_view(&view, &source, 0, 1, 3, 1, &error), &error);
    milena_array_retain(&source);
    milena_array_release(&source);
    assert(source.storage == NULL);
    milena_array_release(&source);
    assert(view.size == 2);
    assert(((const int64_t *)milena_array_const_data(&view))[0] == 2);
    milena_array_release(&view);
    milena_array_release(&view);
    assert(view.storage == NULL);
    assert(milena_array_median(&result, &empty, &error) != MILENA_OK);
    milena_array_release(&result);
    milena_array_release(&empty);
}

static void test_order_statistics_by_axis(void) {
    const size_t shape[] = {3, 3};
    const int64_t values[] = {1, 4, 7, 2, 5, 8, 3, 6, 9};
    const double expected_median_axis0[] = {2, 5, 8};
    const double expected_percentile_axis1[] = {4, 5, 6};
    MilenaArray array = {0}, median = {0}, percentile = {0}, low = {0}, high = {0};
    MilenaError error;
    milena_error_clear(&error);
    expect_ok(milena_array_from_i64(&array, 2, shape, values, &error), &error);
    expect_ok(milena_array_median_axis(&median, &array, 0, false, &error), &error);
    assert(median.ndim == 1 && median.shape[0] == 3);
    assert(memcmp(milena_array_const_data(&median), expected_median_axis0,
                  sizeof(expected_median_axis0)) == 0);
    expect_ok(milena_array_percentile_axis(&percentile, &array, 50.0, 1, true, &error), &error);
    assert(percentile.ndim == 2 && percentile.shape[0] == 3 && percentile.shape[1] == 1);
    assert(memcmp(milena_array_const_data(&percentile), expected_percentile_axis1,
                  sizeof(expected_percentile_axis1)) == 0);
    expect_ok(milena_array_percentile_axis(&low, &array, 0.0, 1, false, &error), &error);
    expect_ok(milena_array_percentile_axis(&high, &array, 100.0, 1, false, &error), &error);
    assert(((const double *)milena_array_const_data(&low))[0] == 1.0);
    assert(((const double *)milena_array_const_data(&high))[2] == 9.0);
    milena_array_release(&high);
    milena_array_release(&low);
    milena_array_release(&percentile);
    milena_array_release(&median);
    milena_array_release(&array);
}

int main(void) {
    test_dtypes_and_casts();
    test_creation_and_reshape();
    test_broadcast_add();
    test_slice_views();
    test_transpose_and_reshape_copy();
    test_boolean_masks_and_where();
    test_sum_by_axis();
    test_memory_ownership();
    test_order_statistics_by_axis();
    puts("OK: MilenaArray creation, views, broadcasting and reductions");
    return 0;
}
