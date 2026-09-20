#include "array.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ok(MilenaStatus status, MilenaError *error) {
    if (status != MILENA_OK) {
        fprintf(stderr, "unexpected %s: %s\n", milena_status_name(status),
                error->message);
        abort();
    }
}

static void expect(MilenaStatus actual, MilenaStatus expected) {
    if (actual != expected) {
        fprintf(stderr, "expected %s, got %s\n",
                milena_status_name(expected), milena_status_name(actual));
        abort();
    }
}

static int64_t i64_at(const MilenaArray *array, size_t index) {
    size_t remaining = index;
    size_t offset = array->byte_offset;
    for (size_t axis = array->ndim; axis > 0; --axis) {
        size_t current = axis - 1u;
        size_t coordinate = remaining % array->shape[current];
        remaining /= array->shape[current];
        ptrdiff_t stride = array->strides[current];
        size_t magnitude = stride < 0 ?
            (size_t)(-(stride + 1)) + 1u : (size_t)stride;
        if (stride < 0) offset -= coordinate * magnitude;
        else offset += coordinate * magnitude;
    }
    const unsigned char *base =
        (const unsigned char *)milena_array_const_data(array);
    size_t relative = offset >= array->byte_offset ?
        offset - array->byte_offset : array->byte_offset - offset;
    int64_t result = 0;
    if (offset >= array->byte_offset) memcpy(&result, base + relative, sizeof(result));
    else memcpy(&result, base - relative, sizeof(result));
    return result;
}

static void test_validation_and_state(void) {
    MilenaError error;
    milena_error_clear(&error);
    MilenaArray zero;
    milena_array_init(&zero);
    expect(milena_array_validate(&zero, &error), MILENA_ERR_ARGUMENT);
    milena_array_release(&zero);

    const size_t shape[] = {2};
    const int64_t values[] = {1, 2};
    MilenaArray array = {0};
    ok(milena_array_from_i64(&array, 1, shape, values, &error), &error);
    ok(milena_array_validate(&array, &error), &error);
    size_t old_itemsize = array.itemsize;
    array.itemsize = 1;
    expect(milena_array_validate(&array, &error), MILENA_ERR_TYPE);
    array.itemsize = old_itemsize;
    array.size = 3;
    expect(milena_array_validate(&array, &error), MILENA_ERR_ARGUMENT);
    array.size = 2;
    array.flags |= 0x80000000u;
    expect(milena_array_validate(&array, &error), MILENA_ERR_ARGUMENT);
    array.flags &= ~0x80000000u;
    ok(milena_array_validate(&array, &error), &error);
    milena_array_release(&array);
}

static void test_failure_atomic_and_aliasing(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {2};
    const int64_t old_values[] = {91, 92};
    const int64_t a_values[] = {1, 2};
    const int64_t b_values[] = {3, 4};
    MilenaArray output = {0}, a = {0}, b = {0}, condition = {0};
    ok(milena_array_from_i64(&output, 1, shape, old_values, &error), &error);
    ok(milena_array_from_i64(&a, 1, shape, a_values, &error), &error);
    ok(milena_array_from_i64(&b, 1, shape, b_values, &error), &error);
    ok(milena_array_zeros(&condition, MILENA_DTYPE_BOOL, 1, shape, &error), &error);
    MilenaArrayStorage *old_storage = output.storage;
    expect(milena_array_where(&output, &a, &a, &b, &error), MILENA_ERR_TYPE);
    assert(output.storage == old_storage && i64_at(&output, 0) == 91);

    MilenaArray f64 = {0};
    const double f_values[] = {5.0, 6.0};
    ok(milena_array_from_f64(&f64, 1, shape, f_values, &error), &error);
    expect(milena_array_where(&output, &condition, &a, &f64, &error),
           MILENA_ERR_TYPE);
    assert(output.storage == old_storage && i64_at(&output, 1) == 92);
    expect(milena_array_add(&a, &a, &b, &error), MILENA_ERR_ARGUMENT);
    assert(i64_at(&a, 0) == 1);

    MilenaArray shared = {0};
    ok(milena_array_share(&shared, &a, &error), &error);
    expect(milena_array_add(&shared, &a, &b, &error), MILENA_ERR_ARGUMENT);
    assert(shared.storage == a.storage);

    const size_t impossible[] = {SIZE_MAX, 2};
    expect(milena_array_zeros(&output, MILENA_DTYPE_INT64, 2,
                              impossible, &error), MILENA_ERR_OVERFLOW);
    assert(output.storage == old_storage && i64_at(&output, 0) == 91);

    milena_array_release(&shared);
    milena_array_release(&f64);
    milena_array_release(&condition);
    milena_array_release(&b);
    milena_array_release(&a);
    milena_array_release(&output);
}

static void test_share_views_and_lifetime(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {4};
    const int64_t values[] = {10, 20, 30, 40};
    MilenaArray parent = {0}, share = {0}, view = {0};
    ok(milena_array_from_i64(&parent, 1, shape, values, &error), &error);
    ok(milena_array_share(&share, &parent, &error), &error);
    assert(share.storage == parent.storage && share.shape != parent.shape &&
           share.strides != parent.strides);
    MilenaSlice reverse = {false, 0, false, 0, -1};
    ok(milena_array_slice_view_ex(&view, &parent, 0, &reverse, &error), &error);
    assert(view.storage == parent.storage && i64_at(&view, 0) == 40);
    milena_array_release(&parent);
    assert(i64_at(&view, 3) == 10 && i64_at(&share, 2) == 30);
    milena_array_release(&share);
    assert(i64_at(&view, 1) == 30);
    milena_array_release(&view);
}

static void test_signed_slices_and_where_views(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {6};
    const int64_t values[] = {0, 1, 2, 3, 4, 5};
    MilenaArray source = {0}, reverse = {0}, composed = {0};
    ok(milena_array_from_i64(&source, 1, shape, values, &error), &error);
    MilenaSlice reverse_slice = {false, 0, false, 0, -1};
    ok(milena_array_slice_view_ex(&reverse, &source, 0,
                                  &reverse_slice, &error), &error);
    assert(reverse.shape[0] == 6 && reverse.strides[0] == -(ptrdiff_t)sizeof(int64_t));
    for (size_t i = 0; i < 6; ++i) assert(i64_at(&reverse, i) == (int64_t)(5u - i));
    MilenaSlice every_second = {true, 1, true, 6, 2};
    ok(milena_array_slice_view_ex(&composed, &reverse, 0,
                                  &every_second, &error), &error);
    assert(composed.shape[0] == 3 && i64_at(&composed, 0) == 4 &&
           i64_at(&composed, 2) == 0);

    MilenaSlice clipped = {true, -100, true, 100, 3};
    MilenaArray clip_view = {0};
    ok(milena_array_slice_view_ex(&clip_view, &source, 0, &clipped, &error), &error);
    assert(clip_view.shape[0] == 2 && i64_at(&clip_view, 1) == 3);
    MilenaSlice bad = {false, 0, false, 0, 0};
    MilenaArray failed = {0};
    expect(milena_array_slice_view_ex(&failed, &source, 0, &bad, &error),
           MILENA_ERR_ARGUMENT);

    MilenaArray true_reverse = {0}, false_reverse = {0};
    const int64_t true_values[] = {10, 20, 30, 40, 50, 60};
    const int64_t false_values[] = {-10, -20, -30, -40, -50, -60};
    MilenaArray true_base = {0}, false_base = {0};
    ok(milena_array_from_i64(&true_base, 1, shape, true_values, &error), &error);
    ok(milena_array_from_i64(&false_base, 1, shape, false_values, &error), &error);
    ok(milena_array_slice_view_ex(&true_reverse, &true_base, 0,
                                  &reverse_slice, &error), &error);
    ok(milena_array_slice_view_ex(&false_reverse, &false_base, 0,
                                  &reverse_slice, &error), &error);
    MilenaArray mask_base = {0}, mask_reverse = {0}, selected = {0};
    ok(milena_array_zeros(&mask_base, MILENA_DTYPE_BOOL, 1, shape, &error), &error);
    void *mutable_data = NULL;
    ok(milena_array_mut_data(&mask_base, &mutable_data, &error), &error);
    bool mask_values[] = {true, false, true, false, true, false};
    memcpy(mutable_data, mask_values, sizeof(mask_values));
    ok(milena_array_slice_view_ex(&mask_reverse, &mask_base, 0,
                                  &reverse_slice, &error), &error);
    ok(milena_array_where(&selected, &mask_reverse, &true_reverse,
                          &false_reverse, &error), &error);
    const int64_t expected[] = {-60, 50, -40, 30, -20, 10};
    assert(memcmp(milena_array_const_data(&selected), expected,
                  sizeof(expected)) == 0);

    milena_array_release(&selected); milena_array_release(&mask_reverse);
    milena_array_release(&mask_base); milena_array_release(&false_reverse);
    milena_array_release(&true_reverse); milena_array_release(&false_base);
    milena_array_release(&true_base); milena_array_release(&clip_view);
    milena_array_release(&composed); milena_array_release(&reverse);
    milena_array_release(&source);
}

static void test_broadcast_and_readonly(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {3};
    const int64_t values[] = {7, 8, 9};
    const size_t target[] = {2, 3};
    MilenaArray source = {0}, view = {0};
    ok(milena_array_from_i64(&source, 1, shape, values, &error), &error);
    ok(milena_array_broadcast_view(&view, &source, 2, target, &error), &error);
    assert(view.storage == source.storage && view.shape[0] == 2 &&
           view.strides[0] == 0 &&
           (view.flags & MILENA_ARRAY_READONLY) != 0u);
    assert(i64_at(&view, 4) == 8);
    assert(milena_array_data(&view) == NULL);
    void *pointer = (void *)(uintptr_t)1u;
    expect(milena_array_mut_data(&view, &pointer, &error), MILENA_ERR_ARGUMENT);
    assert(pointer == NULL);

    const size_t bad_target[] = {2, 2};
    MilenaArray failed = {0};
    expect(milena_array_broadcast_view(&failed, &source, 2,
                                       bad_target, &error), MILENA_ERR_ARGUMENT);
    milena_array_release(&view);
    milena_array_release(&source);
}

static size_t delete_calls;
static void tracked_delete(void *data, void *context) {
    size_t *counter = (size_t *)context;
    ++*counter;
    free(data);
}

static void test_external_buffers(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t shape[] = {4};
    int64_t *owned = (int64_t *)malloc(4u * sizeof(int64_t));
    assert(owned != NULL);
    for (size_t i = 0; i < 4; ++i) owned[i] = (int64_t)(i + 1u);
    delete_calls = 0;
    MilenaArray external = {0}, view = {0};
    ok(milena_array_from_buffer(&external, MILENA_DTYPE_INT64, 1, shape,
                                NULL, owned, 4u * sizeof(int64_t), 0, 0,
                                tracked_delete, &delete_calls, &error), &error);
    const size_t reshaped[] = {2, 2};
    ok(milena_array_reshape_view(&view, &external, 2, reshaped, &error), &error);
    milena_array_release(&external);
    assert(delete_calls == 0 && i64_at(&view, 3) == 4);
    milena_array_release(&view);
    assert(delete_calls == 1);

    int64_t borrowed[] = {4, 3, 2, 1};
    const ptrdiff_t negative_stride[] = {-(ptrdiff_t)sizeof(int64_t)};
    MilenaArray negative = {0};
    ok(milena_array_from_buffer(&negative, MILENA_DTYPE_INT64, 1, shape,
                                negative_stride, borrowed, sizeof(borrowed),
                                3u * sizeof(int64_t), MILENA_ARRAY_READONLY,
                                NULL, NULL, &error), &error);
    assert(i64_at(&negative, 0) == 1 && i64_at(&negative, 3) == 4);
    milena_array_release(&negative);

    unsigned char *unaligned = (unsigned char *)malloc(32);
    assert(unaligned != NULL);
    MilenaArray invalid = {0};
    const size_t one[] = {1};
    expect(milena_array_from_buffer(&invalid, MILENA_DTYPE_INT64, 1, one,
                                    NULL, unaligned + 1, 16, 0, 0, NULL,
                                    NULL, &error), MILENA_ERR_ARGUMENT);
    free(unaligned);

    int64_t scalar_value = 42;
    MilenaArray scalar = {0};
    ok(milena_array_from_buffer(&scalar, MILENA_DTYPE_INT64, 0, NULL,
                                NULL, &scalar_value, sizeof(scalar_value), 0,
                                MILENA_ARRAY_READONLY, NULL, NULL, &error),
       &error);
    assert(scalar.ndim == 0 && scalar.size == 1 &&
           *(const int64_t *)milena_array_const_data(&scalar) == 42);
    milena_array_release(&scalar);

    int64_t value = 3;
    const ptrdiff_t zero_stride[] = {0};
    const size_t two[] = {2};
    expect(milena_array_from_buffer(&invalid, MILENA_DTYPE_INT64, 1, two,
                                    zero_stride, &value, sizeof(value), 0, 0,
                                    NULL, NULL, &error), MILENA_ERR_ARGUMENT);
}

static void test_empty_scalar_contiguity_and_swap(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t empty_shape[] = {2, 0, 3};
    MilenaArray empty = {0};
    ok(milena_array_zeros(&empty, MILENA_DTYPE_FLOAT64, 3,
                          empty_shape, &error), &error);
    assert(empty.size == 0 && milena_array_is_contiguous(&empty));
    assert(milena_array_const_data(&empty) == NULL);
    void *pointer = (void *)(uintptr_t)1u;
    ok(milena_array_mut_data(&empty, &pointer, &error), &error);
    assert(pointer == NULL);

    MilenaArray scalar = {0};
    ok(milena_array_zeros(&scalar, MILENA_DTYPE_FLOAT64, 0, NULL, &error),
       &error);
    assert(scalar.size == 1 && scalar.ndim == 0 &&
           milena_array_is_contiguous(&scalar));
    double *scalar_data = NULL;
    void *raw = NULL;
    ok(milena_array_mut_data(&scalar, &raw, &error), &error);
    scalar_data = (double *)raw;
    *scalar_data = 2.5;

    MilenaArray noncontiguous = {0};
    const size_t shape[] = {4};
    const int64_t values[] = {1, 2, 3, 4};
    MilenaArray base = {0};
    ok(milena_array_from_i64(&base, 1, shape, values, &error), &error);
    MilenaSlice step = {false, 0, false, 0, 2};
    ok(milena_array_slice_view_ex(&noncontiguous, &base, 0, &step, &error),
       &error);
    assert(!milena_array_is_contiguous(&noncontiguous));

    MilenaArrayStorage *empty_storage = empty.storage;
    MilenaArrayStorage *scalar_storage = scalar.storage;
    milena_array_swap(&empty, &scalar);
    assert(empty.storage == scalar_storage && scalar.storage == empty_storage);

    milena_array_release(&noncontiguous); milena_array_release(&base);
    milena_array_release(&scalar); milena_array_release(&empty);
}

int main(void) {
    test_validation_and_state();
    test_failure_atomic_and_aliasing();
    test_share_views_and_lifetime();
    test_signed_slices_and_where_views();
    test_broadcast_and_readonly();
    test_external_buffers();
    test_empty_scalar_contiguity_and_swap();
    puts("OK: worker2 array memory model, views, buffers and atomic outputs");
    return 0;
}
