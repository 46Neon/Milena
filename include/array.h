#ifndef MILENA_ARRAY_H
#define MILENA_ARRAY_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MILENA_DTYPE_BOOL = 0,
    MILENA_DTYPE_INT8,
    MILENA_DTYPE_INT16,
    MILENA_DTYPE_INT32,
    MILENA_DTYPE_INT64,
    MILENA_DTYPE_UINT8,
    MILENA_DTYPE_UINT16,
    MILENA_DTYPE_UINT32,
    MILENA_DTYPE_UINT64,
    MILENA_DTYPE_FLOAT32,
    MILENA_DTYPE_FLOAT64,
    MILENA_DTYPE_COMPLEX64,
    MILENA_DTYPE_COMPLEX128
} MilenaDType;

typedef struct MilenaArrayStorage MilenaArrayStorage;
typedef void (*MilenaArrayBufferDeleter)(void *data, void *context);

typedef struct {
    bool has_start;
    ptrdiff_t start;
    bool has_stop;
    ptrdiff_t stop;
    ptrdiff_t step;
} MilenaSlice;

typedef struct {
    MilenaArrayStorage *storage;
    MilenaDType dtype;
    size_t ndim;
    size_t *shape;
    ptrdiff_t *strides;
    size_t itemsize;
    size_t size;
    size_t byte_offset;
    unsigned flags;
} MilenaArray;

#define MILENA_ARRAY_OWN_DATA 1u
#define MILENA_ARRAY_READONLY 2u

const char *milena_dtype_name(MilenaDType dtype);
size_t milena_dtype_size(MilenaDType dtype);

/* A descriptor must be initialized (with this function or {0}) before use. */
void milena_array_init(MilenaArray *array);
MilenaStatus milena_array_validate(const MilenaArray *array, MilenaError *error);
void milena_array_swap(MilenaArray *left, MilenaArray *right);

MilenaStatus milena_array_zeros(MilenaArray *out, MilenaDType dtype,
                                size_t ndim, const size_t *shape,
                                MilenaError *error);
MilenaStatus milena_array_from_f64(MilenaArray *out, size_t ndim,
                                   const size_t *shape, const double *values,
                                   MilenaError *error);
MilenaStatus milena_array_from_i64(MilenaArray *out, size_t ndim,
                                   const size_t *shape, const int64_t *values,
                                   MilenaError *error);
MilenaStatus milena_array_from_buffer(MilenaArray *out, MilenaDType dtype,
                                      size_t ndim, const size_t *shape,
                                      const ptrdiff_t *strides, void *data,
                                      size_t nbytes, size_t byte_offset,
                                      unsigned flags,
                                      MilenaArrayBufferDeleter deleter,
                                      void *deleter_context,
                                      MilenaError *error);
MilenaStatus milena_array_share(MilenaArray *out, const MilenaArray *source,
                                MilenaError *error);
MilenaStatus milena_array_cast(MilenaArray *out, const MilenaArray *source,
                               MilenaDType dtype, MilenaError *error);
MilenaStatus milena_array_greater_f64(MilenaArray *out,
                                      const MilenaArray *source,
                                      double threshold, MilenaError *error);
MilenaStatus milena_array_boolean_mask(MilenaArray *out,
                                       const MilenaArray *source,
                                       const MilenaArray *mask,
                                       MilenaError *error);
MilenaStatus milena_array_nonzero(MilenaArray *out, const MilenaArray *mask,
                                  MilenaError *error);
MilenaStatus milena_array_where(MilenaArray *out,
                                const MilenaArray *condition,
                                const MilenaArray *when_true,
                                const MilenaArray *when_false,
                                MilenaError *error);

/* Deprecated: retains storage only and does not duplicate descriptor metadata.
 * Use milena_array_share for a separately releasable descriptor. */
void milena_array_retain(MilenaArray *array);
void milena_array_release(MilenaArray *array);

MilenaStatus milena_array_reshape_view(MilenaArray *out,
                                       const MilenaArray *source,
                                       size_t ndim, const size_t *shape,
                                       MilenaError *error);
MilenaStatus milena_array_slice_view(MilenaArray *out,
                                     const MilenaArray *source,
                                     size_t axis, size_t start, size_t stop,
                                     size_t step, MilenaError *error);
MilenaStatus milena_array_slice_view_ex(MilenaArray *out,
                                        const MilenaArray *source,
                                        size_t axis, const MilenaSlice *slice,
                                        MilenaError *error);
MilenaStatus milena_array_transpose_view(MilenaArray *out,
                                         const MilenaArray *source,
                                         const size_t *axes,
                                         MilenaError *error);
MilenaStatus milena_array_broadcast_view(MilenaArray *out,
                                         const MilenaArray *source,
                                         size_t ndim, const size_t *shape,
                                         MilenaError *error);
MilenaStatus milena_array_reshape_copy(MilenaArray *out,
                                       const MilenaArray *source,
                                       size_t ndim, const size_t *shape,
                                       MilenaError *error);
MilenaStatus milena_array_add(MilenaArray *out, const MilenaArray *left,
                              const MilenaArray *right, MilenaError *error);
MilenaStatus milena_array_subtract(MilenaArray *out, const MilenaArray *left,
                                   const MilenaArray *right, MilenaError *error);
MilenaStatus milena_array_multiply(MilenaArray *out, const MilenaArray *left,
                                   const MilenaArray *right, MilenaError *error);
MilenaStatus milena_array_divide(MilenaArray *out, const MilenaArray *left,
                                 const MilenaArray *right, MilenaError *error);
MilenaStatus milena_array_sum(MilenaArray *out, const MilenaArray *source,
                              int axis, bool keepdims, MilenaError *error);
MilenaStatus milena_array_mean(MilenaArray *out, const MilenaArray *source,
                               MilenaError *error);
MilenaStatus milena_array_min(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error);
MilenaStatus milena_array_max(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error);
MilenaStatus milena_array_variance(MilenaArray *out,
                                   const MilenaArray *source,
                                   MilenaError *error);
MilenaStatus milena_array_std(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error);
MilenaStatus milena_array_mean_axis(MilenaArray *out,
                                    const MilenaArray *source,
                                    int axis, bool keepdims,
                                    MilenaError *error);
MilenaStatus milena_array_min_axis(MilenaArray *out,
                                   const MilenaArray *source,
                                   int axis, bool keepdims,
                                   MilenaError *error);
MilenaStatus milena_array_max_axis(MilenaArray *out,
                                   const MilenaArray *source,
                                   int axis, bool keepdims,
                                   MilenaError *error);
MilenaStatus milena_array_variance_axis(MilenaArray *out,
                                        const MilenaArray *source,
                                        int axis, bool keepdims,
                                        MilenaError *error);
MilenaStatus milena_array_std_axis(MilenaArray *out,
                                   const MilenaArray *source,
                                   int axis, bool keepdims,
                                   MilenaError *error);
MilenaStatus milena_array_median(MilenaArray *out,
                                 const MilenaArray *source,
                                 MilenaError *error);
MilenaStatus milena_array_median_axis(MilenaArray *out,
                                      const MilenaArray *source,
                                      int axis, bool keepdims,
                                      MilenaError *error);
MilenaStatus milena_array_percentile(MilenaArray *out,
                                     const MilenaArray *source,
                                     double percentile, MilenaError *error);
MilenaStatus milena_array_percentile_axis(MilenaArray *out,
                                          const MilenaArray *source,
                                          double percentile, int axis,
                                          bool keepdims,
                                          MilenaError *error);

bool milena_array_is_contiguous(const MilenaArray *array);
/* Compatibility accessor: returns NULL for readonly or invalid arrays. */
void *milena_array_data(MilenaArray *array);
MilenaStatus milena_array_mut_data(MilenaArray *array, void **data,
                                   MilenaError *error);
const void *milena_array_const_data(const MilenaArray *array);

#ifdef __cplusplus
}
#endif

#endif
