#include "array.h"

#include <complex.h>
#include <stdatomic.h>
#include <stdint.h>

#define STORAGE_INTERNAL 1u
#define ARRAY_PUBLIC_FLAGS (MILENA_ARRAY_OWN_DATA | MILENA_ARRAY_READONLY)

struct MilenaArrayStorage {
    unsigned char *data;
    size_t nbytes;
    atomic_size_t references;
    MilenaArrayBufferDeleter deleter;
    void *deleter_context;
    unsigned flags;
};

static void array_error(MilenaError *error, MilenaStatus code,
                        const char *message) {
    if (error != NULL) milena_error_set(error, code, 0, 0, 0, message);
}

static void internal_buffer_deleter(void *data, void *context) {
    (void)context;
    free(data);
}

const char *milena_dtype_name(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_BOOL: return "bool";
        case MILENA_DTYPE_INT8: return "int8";
        case MILENA_DTYPE_INT16: return "int16";
        case MILENA_DTYPE_INT32: return "int32";
        case MILENA_DTYPE_INT64: return "int64";
        case MILENA_DTYPE_UINT8: return "uint8";
        case MILENA_DTYPE_UINT16: return "uint16";
        case MILENA_DTYPE_UINT32: return "uint32";
        case MILENA_DTYPE_UINT64: return "uint64";
        case MILENA_DTYPE_FLOAT32: return "float32";
        case MILENA_DTYPE_FLOAT64: return "float64";
        case MILENA_DTYPE_COMPLEX64: return "complex64";
        case MILENA_DTYPE_COMPLEX128: return "complex128";
        default: return "unknown";
    }
}

size_t milena_dtype_size(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_BOOL: return sizeof(bool);
        case MILENA_DTYPE_INT8: return sizeof(int8_t);
        case MILENA_DTYPE_INT16: return sizeof(int16_t);
        case MILENA_DTYPE_INT32: return sizeof(int32_t);
        case MILENA_DTYPE_INT64: return sizeof(int64_t);
        case MILENA_DTYPE_UINT8: return sizeof(uint8_t);
        case MILENA_DTYPE_UINT16: return sizeof(uint16_t);
        case MILENA_DTYPE_UINT32: return sizeof(uint32_t);
        case MILENA_DTYPE_UINT64: return sizeof(uint64_t);
        case MILENA_DTYPE_FLOAT32: return sizeof(float);
        case MILENA_DTYPE_FLOAT64: return sizeof(double);
        case MILENA_DTYPE_COMPLEX64: return sizeof(float _Complex);
        case MILENA_DTYPE_COMPLEX128: return sizeof(double _Complex);
        default: return 0;
    }
}

static size_t dtype_alignment(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_BOOL: return _Alignof(bool);
        case MILENA_DTYPE_INT8: return _Alignof(int8_t);
        case MILENA_DTYPE_INT16: return _Alignof(int16_t);
        case MILENA_DTYPE_INT32: return _Alignof(int32_t);
        case MILENA_DTYPE_INT64: return _Alignof(int64_t);
        case MILENA_DTYPE_UINT8: return _Alignof(uint8_t);
        case MILENA_DTYPE_UINT16: return _Alignof(uint16_t);
        case MILENA_DTYPE_UINT32: return _Alignof(uint32_t);
        case MILENA_DTYPE_UINT64: return _Alignof(uint64_t);
        case MILENA_DTYPE_FLOAT32: return _Alignof(float);
        case MILENA_DTYPE_FLOAT64: return _Alignof(double);
        case MILENA_DTYPE_COMPLEX64: return _Alignof(float _Complex);
        case MILENA_DTYPE_COMPLEX128: return _Alignof(double _Complex);
        default: return 0;
    }
}

static bool descriptor_is_zero(const MilenaArray *array) {
    return array != NULL && array->storage == NULL &&
           array->dtype == MILENA_DTYPE_BOOL && array->ndim == 0 &&
           array->shape == NULL && array->strides == NULL &&
           array->itemsize == 0 && array->size == 0 &&
           array->byte_offset == 0 && array->flags == 0;
}

void milena_array_init(MilenaArray *array) {
    if (array != NULL) memset(array, 0, sizeof(*array));
}

void milena_array_swap(MilenaArray *left, MilenaArray *right) {
    if (left == NULL || right == NULL || left == right) return;
    MilenaArray temporary = *left;
    *left = *right;
    *right = temporary;
}

static MilenaStatus shape_size(size_t ndim, const size_t *shape,
                               size_t *result, MilenaError *error) {
    if (result == NULL || (ndim != 0 && shape == NULL)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "La forma requiere metadata válida");
        return MILENA_ERR_ARGUMENT;
    }
    size_t size = 1;
    for (size_t axis = 0; axis < ndim; ++axis) {
        if (!milena_size_mul(size, shape[axis], &size)) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "El producto de la forma desborda size_t");
            return MILENA_ERR_OVERFLOW;
        }
    }
    *result = size;
    return MILENA_OK;
}

static size_t stride_magnitude(ptrdiff_t stride) {
    if (stride >= 0) return (size_t)stride;
    return (size_t)(-(stride + 1)) + 1u;
}

static MilenaStatus reachable_bounds(const MilenaArray *array,
                                     size_t *lowest, size_t *highest,
                                     MilenaError *error) {
    size_t negative = 0;
    size_t positive = 0;
    for (size_t axis = 0; axis < array->ndim; ++axis) {
        if (array->shape[axis] <= 1) continue;
        size_t extent = 0;
        size_t magnitude = stride_magnitude(array->strides[axis]);
        if (!milena_size_mul(array->shape[axis] - 1u, magnitude, &extent)) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "El alcance de los strides desborda size_t");
            return MILENA_ERR_OVERFLOW;
        }
        size_t *accumulator = array->strides[axis] < 0 ? &negative : &positive;
        if (!milena_size_add(*accumulator, extent, accumulator)) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "El alcance combinado de strides desborda size_t");
            return MILENA_ERR_OVERFLOW;
        }
    }
    if (negative > array->byte_offset) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Los strides negativos alcanzan antes del buffer");
        return MILENA_ERR_ARGUMENT;
    }
    if (positive > SIZE_MAX - array->byte_offset) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "El offset máximo del array desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    *lowest = array->byte_offset - negative;
    *highest = array->byte_offset + positive;
    return MILENA_OK;
}

MilenaStatus milena_array_validate(const MilenaArray *array,
                                   MilenaError *error) {
    if (array == NULL || array->storage == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Descriptor de array no inicializado");
        return MILENA_ERR_ARGUMENT;
    }
    size_t expected_itemsize = milena_dtype_size(array->dtype);
    size_t alignment = dtype_alignment(array->dtype);
    if (expected_itemsize == 0 || array->itemsize != expected_itemsize) {
        array_error(error, MILENA_ERR_TYPE,
                    "dtype e itemsize del descriptor no son coherentes");
        return MILENA_ERR_TYPE;
    }
    if ((array->flags & ~ARRAY_PUBLIC_FLAGS) != 0u) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "El descriptor contiene flags públicos desconocidos");
        return MILENA_ERR_ARGUMENT;
    }
    if (array->ndim == 0) {
        if (array->shape != NULL || array->strides != NULL) {
            array_error(error, MILENA_ERR_ARGUMENT,
                        "Un escalar no debe tener metadata por ejes");
            return MILENA_ERR_ARGUMENT;
        }
    } else if (array->shape == NULL || array->strides == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Falta shape o strides en el descriptor");
        return MILENA_ERR_ARGUMENT;
    }
    size_t expected_size = 0;
    MilenaStatus status = shape_size(array->ndim, array->shape,
                                     &expected_size, error);
    if (status != MILENA_OK) return status;
    if (array->size != expected_size) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "size no coincide con el producto de shape");
        return MILENA_ERR_ARGUMENT;
    }
    bool has_zero_stride = false;
    for (size_t axis = 0; axis < array->ndim; ++axis) {
        if (array->strides[axis] == 0) has_zero_stride = true;
        if (array->shape[axis] > 1 && alignment != 0 &&
            stride_magnitude(array->strides[axis]) % alignment != 0) {
            array_error(error, MILENA_ERR_ARGUMENT,
                        "Un stride produce elementos desalineados");
            return MILENA_ERR_ARGUMENT;
        }
    }
    if (has_zero_stride && (array->flags & MILENA_ARRAY_READONLY) == 0u) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Todo array con stride cero debe ser readonly");
        return MILENA_ERR_ARGUMENT;
    }
    if (array->byte_offset > array->storage->nbytes) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "byte_offset está fuera del almacenamiento");
        return MILENA_ERR_ARGUMENT;
    }
    if (array->storage->nbytes != 0 && array->storage->data == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "El almacenamiento no tiene buffer");
        return MILENA_ERR_ARGUMENT;
    }
    if (array->size == 0) return MILENA_OK;
    if (array->storage->data == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Un array no vacío requiere buffer");
        return MILENA_ERR_ARGUMENT;
    }
    size_t lowest = 0;
    size_t highest = 0;
    status = reachable_bounds(array, &lowest, &highest, error);
    if (status != MILENA_OK) return status;
    if (highest > array->storage->nbytes ||
        array->itemsize > array->storage->nbytes - highest) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Los elementos alcanzables salen del buffer");
        return MILENA_ERR_ARGUMENT;
    }
    uintptr_t address = (uintptr_t)array->storage->data;
    if (address > UINTPTR_MAX - lowest ||
        (address + lowest) % alignment != 0u) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "El primer elemento alcanzable está desalineado");
        return MILENA_ERR_ARGUMENT;
    }
    return MILENA_OK;
}

static MilenaStatus storage_retain(MilenaArrayStorage *storage,
                                   MilenaError *error) {
    size_t current = atomic_load_explicit(&storage->references,
                                          memory_order_relaxed);
    for (;;) {
        if (current == SIZE_MAX) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "El contador de referencias del storage se saturó");
            return MILENA_ERR_OVERFLOW;
        }
        if (atomic_compare_exchange_weak_explicit(&storage->references,
                                                  &current, current + 1u,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            return MILENA_OK;
        }
    }
}

static void storage_release(MilenaArrayStorage *storage) {
    if (storage == NULL) return;
    size_t previous = atomic_fetch_sub_explicit(&storage->references, 1u,
                                                 memory_order_acq_rel);
    if (previous == 1u) {
        if (storage->deleter != NULL)
            storage->deleter(storage->data, storage->deleter_context);
        free(storage);
    }
}

void milena_array_retain(MilenaArray *array) {
    if (array != NULL && array->storage != NULL)
        (void)storage_retain(array->storage, NULL);
}

void milena_array_release(MilenaArray *array) {
    if (array == NULL) return;
    if (array->storage != NULL) storage_release(array->storage);
    free(array->shape);
    free(array->strides);
    memset(array, 0, sizeof(*array));
}

static MilenaStatus make_contiguous_strides(size_t ndim, size_t itemsize,
                                            const size_t *shape,
                                            ptrdiff_t *strides,
                                            MilenaError *error) {
    if (ndim == 0) return MILENA_OK;
    if (itemsize > (size_t)PTRDIFF_MAX) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "itemsize no cabe en ptrdiff_t");
        return MILENA_ERR_OVERFLOW;
    }
    size_t expected = itemsize;
    for (size_t axis = ndim; axis > 0; --axis) {
        size_t current = axis - 1u;
        if (expected > (size_t)PTRDIFF_MAX) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "Los strides contiguos desbordan ptrdiff_t");
            return MILENA_ERR_OVERFLOW;
        }
        strides[current] = (ptrdiff_t)expected;
        /* Empty axes do not need a zero stride.  Keeping the preceding
         * non-zero stride preserves the global zero-stride=>readonly rule. */
        if (shape[current] != 0 &&
            !milena_size_mul(expected, shape[current], &expected)) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "Los strides contiguos desbordan size_t");
            return MILENA_ERR_OVERFLOW;
        }
    }
    return MILENA_OK;
}

static MilenaStatus allocate_metadata(MilenaArray *array, size_t ndim,
                                      const size_t *shape,
                                      const ptrdiff_t *strides,
                                      MilenaError *error) {
    if (ndim == 0) return MILENA_OK;
    if (ndim > SIZE_MAX / sizeof(size_t) ||
        ndim > SIZE_MAX / sizeof(ptrdiff_t)) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "La metadata del array desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    array->shape = (size_t *)malloc(ndim * sizeof(size_t));
    array->strides = (ptrdiff_t *)malloc(ndim * sizeof(ptrdiff_t));
    if (array->shape == NULL || array->strides == NULL) {
        free(array->shape);
        free(array->strides);
        array->shape = NULL;
        array->strides = NULL;
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar metadata del array");
        return MILENA_ERR_MEMORY;
    }
    memcpy(array->shape, shape, ndim * sizeof(size_t));
    if (strides != NULL)
        memcpy(array->strides, strides, ndim * sizeof(ptrdiff_t));
    return MILENA_OK;
}

static MilenaStatus allocate_owned(MilenaArray *array, MilenaDType dtype,
                                   size_t ndim, const size_t *shape,
                                   MilenaError *error) {
    milena_array_init(array);
    size_t itemsize = milena_dtype_size(dtype);
    if (itemsize == 0 || (ndim != 0 && shape == NULL)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Argumentos inválidos al crear array");
        return MILENA_ERR_ARGUMENT;
    }
    size_t size = 0;
    MilenaStatus status = shape_size(ndim, shape, &size, error);
    if (status != MILENA_OK) return status;
    size_t nbytes = 0;
    if (!milena_size_mul(size, itemsize, &nbytes)) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "El buffer del array desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaArrayStorage *storage =
        (MilenaArrayStorage *)calloc(1, sizeof(*storage));
    if (storage == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar storage del array");
        return MILENA_ERR_MEMORY;
    }
    atomic_init(&storage->references, 1u);
    storage->nbytes = nbytes;
    storage->deleter = internal_buffer_deleter;
    storage->flags = STORAGE_INTERNAL;
    if (nbytes != 0) {
        storage->data = (unsigned char *)calloc(1, nbytes);
        if (storage->data == NULL) {
            free(storage);
            array_error(error, MILENA_ERR_MEMORY,
                        "No se pudo reservar buffer del array");
            return MILENA_ERR_MEMORY;
        }
    }
    array->storage = storage;
    array->dtype = dtype;
    array->ndim = ndim;
    array->itemsize = itemsize;
    array->size = size;
    array->flags = MILENA_ARRAY_OWN_DATA;
    status = allocate_metadata(array, ndim, shape, NULL, error);
    if (status == MILENA_OK)
        status = make_contiguous_strides(ndim, itemsize, shape,
                                         array->strides, error);
    if (status != MILENA_OK) {
        milena_array_release(array);
        return status;
    }
    return MILENA_OK;
}

static MilenaStatus prepare_output(MilenaArray *out,
                                   const MilenaArray *const *inputs,
                                   size_t input_count, MilenaError *error) {
    if (out == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "El descriptor de salida es NULL");
        return MILENA_ERR_ARGUMENT;
    }
    for (size_t index = 0; index < input_count; ++index) {
        if (out == inputs[index]) {
            array_error(error, MILENA_ERR_ARGUMENT,
                        "La salida no puede aliasar un descriptor de entrada");
            return MILENA_ERR_ARGUMENT;
        }
    }
    if (descriptor_is_zero(out)) return MILENA_OK;
    MilenaStatus status = milena_array_validate(out, error);
    if (status != MILENA_OK) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "La salida previa no es un descriptor liberable válido");
        return MILENA_ERR_ARGUMENT;
    }
    for (size_t index = 0; index < input_count; ++index) {
        if (inputs[index] != NULL && out->storage == inputs[index]->storage) {
            array_error(error, MILENA_ERR_ARGUMENT,
                        "La salida previa comparte storage con una entrada");
            return MILENA_ERR_ARGUMENT;
        }
    }
    return MILENA_OK;
}

static void commit_output(MilenaArray *out, MilenaArray *temporary) {
    milena_array_release(out);
    *out = *temporary;
    milena_array_init(temporary);
}

MilenaStatus milena_array_zeros(MilenaArray *out, MilenaDType dtype,
                                size_t ndim, const size_t *shape,
                                MilenaError *error) {
    MilenaStatus status = prepare_output(out, NULL, 0, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, dtype, ndim, shape, error);
    if (status == MILENA_OK) commit_output(out, &temporary);
    return status;
}

static MilenaStatus from_values(MilenaArray *out, MilenaDType dtype,
                                size_t ndim, const size_t *shape,
                                const void *values, MilenaError *error) {
    MilenaStatus status = prepare_output(out, NULL, 0, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, dtype, ndim, shape, error);
    if (status != MILENA_OK) return status;
    if (temporary.size != 0 && values == NULL) {
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Faltan valores para inicializar el array");
        return MILENA_ERR_ARGUMENT;
    }
    if (temporary.size != 0)
        memcpy(temporary.storage->data, values, temporary.storage->nbytes);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_from_f64(MilenaArray *out, size_t ndim,
                                   const size_t *shape, const double *values,
                                   MilenaError *error) {
    return from_values(out, MILENA_DTYPE_FLOAT64, ndim, shape, values, error);
}

MilenaStatus milena_array_from_i64(MilenaArray *out, size_t ndim,
                                   const size_t *shape, const int64_t *values,
                                   MilenaError *error) {
    return from_values(out, MILENA_DTYPE_INT64, ndim, shape, values, error);
}

MilenaStatus milena_array_from_buffer(MilenaArray *out, MilenaDType dtype,
                                      size_t ndim, const size_t *shape,
                                      const ptrdiff_t *strides, void *data,
                                      size_t nbytes, size_t byte_offset,
                                      unsigned flags,
                                      MilenaArrayBufferDeleter deleter,
                                      void *deleter_context,
                                      MilenaError *error) {
    MilenaStatus status = prepare_output(out, NULL, 0, error);
    if (status != MILENA_OK) return status;
    if ((flags & ~MILENA_ARRAY_READONLY) != 0u ||
        milena_dtype_size(dtype) == 0 || (ndim != 0 && shape == NULL) ||
        (nbytes != 0 && data == NULL)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Argumentos inválidos para buffer externo");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaArray temporary = {0};
    size_t size = 0;
    status = shape_size(ndim, shape, &size, error);
    if (status != MILENA_OK) return status;
    MilenaArrayStorage *storage =
        (MilenaArrayStorage *)calloc(1, sizeof(*storage));
    if (storage == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar storage externo");
        return MILENA_ERR_MEMORY;
    }
    atomic_init(&storage->references, 1u);
    storage->data = (unsigned char *)data;
    storage->nbytes = nbytes;
    storage->deleter = deleter;
    storage->deleter_context = deleter_context;
    temporary.storage = storage;
    temporary.dtype = dtype;
    temporary.ndim = ndim;
    temporary.itemsize = milena_dtype_size(dtype);
    temporary.size = size;
    temporary.byte_offset = byte_offset;
    temporary.flags = flags;
    status = allocate_metadata(&temporary, ndim, shape, strides, error);
    if (status == MILENA_OK && ndim != 0 && strides == NULL)
        status = make_contiguous_strides(ndim, temporary.itemsize, shape,
                                         temporary.strides, error);
    if (status == MILENA_OK) status = milena_array_validate(&temporary, error);
    if (status != MILENA_OK) {
        storage->deleter = NULL; /* ownership transfers only on success */
        milena_array_release(&temporary);
        return status;
    }
    commit_output(out, &temporary);
    return MILENA_OK;
}

static size_t element_offset(const MilenaArray *array,
                             const size_t *coordinates) {
    size_t offset = array->byte_offset;
    for (size_t axis = 0; axis < array->ndim; ++axis) {
        size_t delta = coordinates[axis] * stride_magnitude(array->strides[axis]);
        if (array->strides[axis] < 0) offset -= delta;
        else offset += delta;
    }
    return offset;
}

static void linear_coordinates(const MilenaArray *array, size_t index,
                               size_t *coordinates) {
    for (size_t axis = array->ndim; axis > 0; --axis) {
        size_t current = axis - 1u;
        size_t dimension = array->shape[current];
        coordinates[current] = dimension == 0 ? 0 : index % dimension;
        if (dimension != 0) index /= dimension;
    }
}

static size_t linear_offset(const MilenaArray *array, size_t index,
                            size_t *coordinates) {
    linear_coordinates(array, index, coordinates);
    return element_offset(array, coordinates);
}

static bool same_shape(const MilenaArray *left, const MilenaArray *right) {
    if (left->ndim != right->ndim) return false;
    for (size_t axis = 0; axis < left->ndim; ++axis)
        if (left->shape[axis] != right->shape[axis]) return false;
    return true;
}

static MilenaStatus validate_input(const MilenaArray *array,
                                   MilenaError *error) {
    return milena_array_validate(array, error);
}

MilenaStatus milena_array_share(MilenaArray *out, const MilenaArray *source,
                                MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    temporary.storage = source->storage;
    temporary.dtype = source->dtype;
    temporary.ndim = source->ndim;
    temporary.itemsize = source->itemsize;
    temporary.size = source->size;
    temporary.byte_offset = source->byte_offset;
    temporary.flags = source->flags & ~MILENA_ARRAY_OWN_DATA;
    status = allocate_metadata(&temporary, source->ndim, source->shape,
                               source->strides, error);
    if (status != MILENA_OK) return status;
    status = storage_retain(temporary.storage, error);
    if (status != MILENA_OK) {
        temporary.storage = NULL;
        milena_array_release(&temporary);
        return status;
    }
    commit_output(out, &temporary);
    return MILENA_OK;
}

bool milena_array_is_contiguous(const MilenaArray *array) {
    if (milena_array_validate(array, NULL) != MILENA_OK) return false;
    if (array->size == 0 || array->ndim == 0) return true;
    size_t expected = array->itemsize;
    for (size_t axis = array->ndim; axis > 0; --axis) {
        size_t current = axis - 1u;
        if (array->shape[current] > 1) {
            if (expected > (size_t)PTRDIFF_MAX ||
                array->strides[current] != (ptrdiff_t)expected) return false;
        }
        if (!milena_size_mul(expected, array->shape[current], &expected))
            return false;
    }
    return true;
}

const void *milena_array_const_data(const MilenaArray *array) {
    if (milena_array_validate(array, NULL) != MILENA_OK) return NULL;
    if (array->storage->data == NULL) return NULL;
    return array->storage->data + array->byte_offset;
}

MilenaStatus milena_array_mut_data(MilenaArray *array, void **data,
                                   MilenaError *error) {
    if (data == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Falta la salida para el puntero mutable");
        return MILENA_ERR_ARGUMENT;
    }
    *data = NULL;
    MilenaStatus status = milena_array_validate(array, error);
    if (status != MILENA_OK) return status;
    if ((array->flags & MILENA_ARRAY_READONLY) != 0u) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "El array es readonly");
        return MILENA_ERR_ARGUMENT;
    }
    if (array->storage->data != NULL)
        *data = array->storage->data + array->byte_offset;
    return MILENA_OK;
}

void *milena_array_data(MilenaArray *array) {
    void *data = NULL;
    if (milena_array_mut_data(array, &data, NULL) != MILENA_OK) return NULL;
    return data;
}

static MilenaStatus create_view(MilenaArray *out, const MilenaArray *source,
                                size_t ndim, const size_t *shape,
                                const ptrdiff_t *strides, size_t size,
                                size_t byte_offset, unsigned extra_flags,
                                MilenaError *error) {
    MilenaArray temporary = {0};
    temporary.storage = source->storage;
    temporary.dtype = source->dtype;
    temporary.ndim = ndim;
    temporary.itemsize = source->itemsize;
    temporary.size = size;
    temporary.byte_offset = byte_offset;
    temporary.flags = (source->flags & ~MILENA_ARRAY_OWN_DATA) | extra_flags;
    MilenaStatus status = allocate_metadata(&temporary, ndim, shape, strides,
                                            error);
    if (status != MILENA_OK) return status;
    status = milena_array_validate(&temporary, error);
    if (status != MILENA_OK) {
        temporary.storage = NULL;
        milena_array_release(&temporary);
        return status;
    }
    status = storage_retain(temporary.storage, error);
    if (status != MILENA_OK) {
        temporary.storage = NULL;
        milena_array_release(&temporary);
        return status;
    }
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_reshape_view(MilenaArray *out,
                                       const MilenaArray *source,
                                       size_t ndim, const size_t *shape,
                                       MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!milena_array_is_contiguous(source)) {
        array_error(error, MILENA_ERR_UNSUPPORTED,
                    "reshape view requiere un array C-contiguo");
        return MILENA_ERR_UNSUPPORTED;
    }
    size_t size = 0;
    status = shape_size(ndim, shape, &size, error);
    if (status != MILENA_OK) return status;
    if (size != source->size) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "reshape no conserva el número de elementos");
        return MILENA_ERR_ARGUMENT;
    }
    ptrdiff_t *strides = ndim == 0 ? NULL :
        (ptrdiff_t *)malloc(ndim * sizeof(ptrdiff_t));
    if (ndim != 0 && strides == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar strides de reshape");
        return MILENA_ERR_MEMORY;
    }
    status = make_contiguous_strides(ndim, source->itemsize, shape,
                                     strides, error);
    if (status == MILENA_OK)
        status = create_view(out, source, ndim, shape, strides, size,
                             source->byte_offset, 0u, error);
    free(strides);
    return status;
}

static bool checked_stride_multiply(ptrdiff_t left, ptrdiff_t right,
                                    ptrdiff_t *result) {
    if (left == 0 || right == 0) {
        *result = 0;
        return true;
    }
    bool negative = (left < 0) != (right < 0);
    size_t a = stride_magnitude(left);
    size_t b = stride_magnitude(right);
    size_t magnitude = 0;
    if (!milena_size_mul(a, b, &magnitude)) return false;
    if (!negative) {
        if (magnitude > (size_t)PTRDIFF_MAX) return false;
        *result = (ptrdiff_t)magnitude;
    } else {
        size_t minimum_magnitude = (size_t)PTRDIFF_MAX + 1u;
        if (magnitude > minimum_magnitude) return false;
        if (magnitude == minimum_magnitude) *result = PTRDIFF_MIN;
        else *result = -(ptrdiff_t)magnitude;
    }
    return true;
}

static void normalize_slice(ptrdiff_t length, const MilenaSlice *slice,
                            ptrdiff_t *start, ptrdiff_t *stop,
                            ptrdiff_t *step, size_t *count) {
    *step = slice->step;
    if (*step > 0) {
        ptrdiff_t first = slice->has_start ? slice->start : 0;
        ptrdiff_t last = slice->has_stop ? slice->stop : length;
        if (slice->has_start && first < 0) first += length;
        if (slice->has_stop && last < 0) last += length;
        if (first < 0) first = 0;
        if (first > length) first = length;
        if (last < 0) last = 0;
        if (last > length) last = length;
        *start = first;
        *stop = last;
        *count = first < last ?
            (size_t)(1 + (last - 1 - first) / *step) : 0u;
    } else {
        ptrdiff_t first = slice->has_start ? slice->start : length - 1;
        ptrdiff_t last = slice->has_stop ? slice->stop : -1;
        if (slice->has_start && first < 0) first += length;
        if (slice->has_stop && last < 0) last += length;
        if (first < -1) first = -1;
        if (first >= length) first = length - 1;
        if (last < -1) last = -1;
        if (last >= length) last = length - 1;
        *start = first;
        *stop = last;
        ptrdiff_t magnitude = -*step;
        *count = first > last ?
            (size_t)(1 + (first - 1 - last) / magnitude) : 0u;
    }
}

MilenaStatus milena_array_slice_view_ex(MilenaArray *out,
                                        const MilenaArray *source,
                                        size_t axis, const MilenaSlice *slice,
                                        MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (slice == NULL || axis >= source->ndim || slice->step == 0) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Slice, eje o step inválido");
        return MILENA_ERR_ARGUMENT;
    }
    if (source->shape[axis] > (size_t)PTRDIFF_MAX ||
        slice->step == PTRDIFF_MIN) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "El slice no cabe en índices ptrdiff_t");
        return MILENA_ERR_OVERFLOW;
    }
    size_t *shape = (size_t *)malloc(source->ndim * sizeof(size_t));
    ptrdiff_t *strides =
        (ptrdiff_t *)malloc(source->ndim * sizeof(ptrdiff_t));
    if (shape == NULL || strides == NULL) {
        free(shape);
        free(strides);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar metadata del slice");
        return MILENA_ERR_MEMORY;
    }
    memcpy(shape, source->shape, source->ndim * sizeof(size_t));
    memcpy(strides, source->strides,
           source->ndim * sizeof(ptrdiff_t));
    ptrdiff_t start = 0;
    ptrdiff_t stop = 0;
    ptrdiff_t step = 0;
    size_t selected = 0;
    normalize_slice((ptrdiff_t)source->shape[axis], slice,
                    &start, &stop, &step, &selected);
    (void)stop;
    shape[axis] = selected;
    if (!checked_stride_multiply(strides[axis], step, &strides[axis])) {
        free(shape);
        free(strides);
        array_error(error, MILENA_ERR_OVERFLOW,
                    "El stride compuesto del slice desborda ptrdiff_t");
        return MILENA_ERR_OVERFLOW;
    }
    size_t offset = source->byte_offset;
    if (selected != 0) {
        size_t delta = (size_t)start * stride_magnitude(source->strides[axis]);
        if (source->strides[axis] < 0) offset -= delta;
        else offset += delta;
    }
    size_t size = 0;
    status = shape_size(source->ndim, shape, &size, error);
    if (status == MILENA_OK)
        status = create_view(out, source, source->ndim, shape, strides,
                             size, offset, 0u, error);
    free(shape);
    free(strides);
    return status;
}

MilenaStatus milena_array_slice_view(MilenaArray *out,
                                     const MilenaArray *source,
                                     size_t axis, size_t start, size_t stop,
                                     size_t step, MilenaError *error) {
    if (source == NULL || axis >= source->ndim || step == 0 || start > stop ||
        stop > source->shape[axis] || start > (size_t)PTRDIFF_MAX ||
        stop > (size_t)PTRDIFF_MAX || step > (size_t)PTRDIFF_MAX) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Límites inválidos para slice legacy");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaSlice slice = {true, (ptrdiff_t)start, true, (ptrdiff_t)stop,
                         (ptrdiff_t)step};
    return milena_array_slice_view_ex(out, source, axis, &slice, error);
}

MilenaStatus milena_array_transpose_view(MilenaArray *out,
                                         const MilenaArray *source,
                                         const size_t *axes,
                                         MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    size_t *shape = source->ndim == 0 ? NULL :
        (size_t *)malloc(source->ndim * sizeof(size_t));
    ptrdiff_t *strides = source->ndim == 0 ? NULL :
        (ptrdiff_t *)malloc(source->ndim * sizeof(ptrdiff_t));
    bool *used = source->ndim == 0 ? NULL :
        (bool *)calloc(source->ndim, sizeof(bool));
    if (source->ndim != 0 &&
        (shape == NULL || strides == NULL || used == NULL)) {
        free(shape); free(strides); free(used);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar metadata de transpose");
        return MILENA_ERR_MEMORY;
    }
    for (size_t axis = 0; axis < source->ndim; ++axis) {
        size_t selected = axes == NULL ? source->ndim - 1u - axis : axes[axis];
        if (selected >= source->ndim || used[selected]) {
            free(shape); free(strides); free(used);
            array_error(error, MILENA_ERR_ARGUMENT,
                        "Los ejes no forman una permutación");
            return MILENA_ERR_ARGUMENT;
        }
        used[selected] = true;
        shape[axis] = source->shape[selected];
        strides[axis] = source->strides[selected];
    }
    free(used);
    status = create_view(out, source, source->ndim, shape, strides,
                         source->size, source->byte_offset, 0u, error);
    free(shape);
    free(strides);
    return status;
}

MilenaStatus milena_array_broadcast_view(MilenaArray *out,
                                         const MilenaArray *source,
                                         size_t ndim, const size_t *shape,
                                         MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (ndim < source->ndim || (ndim != 0 && shape == NULL)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Forma objetivo inválida para broadcast view");
        return MILENA_ERR_ARGUMENT;
    }
    ptrdiff_t *strides = ndim == 0 ? NULL :
        (ptrdiff_t *)calloc(ndim, sizeof(ptrdiff_t));
    if (ndim != 0 && strides == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar metadata de broadcast");
        return MILENA_ERR_MEMORY;
    }
    size_t missing = ndim - source->ndim;
    for (size_t axis = 0; axis < ndim; ++axis) {
        if (axis < missing) {
            strides[axis] = 0;
            continue;
        }
        size_t source_axis = axis - missing;
        size_t source_dim = source->shape[source_axis];
        if (source_dim != shape[axis] && source_dim != 1u) {
            free(strides);
            array_error(error, MILENA_ERR_ARGUMENT,
                        "La forma objetivo no es broadcast-compatible");
            return MILENA_ERR_ARGUMENT;
        }
        strides[axis] = source_dim == shape[axis] ?
            source->strides[source_axis] : 0;
    }
    size_t size = 0;
    status = shape_size(ndim, shape, &size, error);
    if (status == MILENA_OK)
        status = create_view(out, source, ndim, shape, strides, size,
                             source->byte_offset, MILENA_ARRAY_READONLY,
                             error);
    free(strides);
    return status;
}

MilenaStatus milena_array_reshape_copy(MilenaArray *out,
                                       const MilenaArray *source,
                                       size_t ndim, const size_t *shape,
                                       MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    size_t size = 0;
    status = shape_size(ndim, shape, &size, error);
    if (status != MILENA_OK) return status;
    if (size != source->size) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "reshape copy no conserva el número de elementos");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, source->dtype, ndim, shape, error);
    if (status != MILENA_OK) return status;
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (source->ndim != 0 && coordinates == NULL) {
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < source->size; ++index) {
        size_t offset = linear_offset(source, index, coordinates);
        memcpy(temporary.storage->data + index * temporary.itemsize,
               source->storage->data + offset, temporary.itemsize);
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_greater_f64(MilenaArray *out,
                                      const MilenaArray *source,
                                      double threshold,
                                      MilenaError *error) {
    MilenaStatus status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (source->dtype >= MILENA_DTYPE_COMPLEX64) {
        array_error(error, MILENA_ERR_UNSUPPORTED,
                    "complex64/complex128 son dtypes solo de almacenamiento");
        return MILENA_ERR_UNSUPPORTED;
    }
    if (source->dtype != MILENA_DTYPE_FLOAT64) {
        array_error(error, MILENA_ERR_TYPE,
                    "greater_f64 requiere float64");
        return MILENA_ERR_TYPE;
    }
    MilenaArray scalar = {0};
    status = milena_array_from_f64(&scalar, 0, NULL, &threshold, error);
    if (status == MILENA_OK)
        status = milena_array_greater(out, source, &scalar, error);
    milena_array_release(&scalar);
    return status;
}

MilenaStatus milena_array_boolean_mask(MilenaArray *out,
                                       const MilenaArray *source,
                                       const MilenaArray *mask,
                                       MilenaError *error) {
    const MilenaArray *inputs[] = {source, mask};
    MilenaStatus status = prepare_output(out, inputs, 2, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status == MILENA_OK) status = validate_input(mask, error);
    if (status != MILENA_OK) return status;
    if (mask->dtype != MILENA_DTYPE_BOOL || !same_shape(source, mask)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "La máscara debe ser bool y tener la misma forma");
        return MILENA_ERR_ARGUMENT;
    }
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (source->ndim != 0 && coordinates == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas");
        return MILENA_ERR_MEMORY;
    }
    size_t selected = 0;
    for (size_t index = 0; index < mask->size; ++index) {
        bool take = false;
        size_t offset = linear_offset(mask, index, coordinates);
        memcpy(&take, mask->storage->data + offset, sizeof(take));
        if (take) ++selected;
    }
    size_t output_shape[] = {selected};
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, source->dtype, 1, output_shape, error);
    if (status != MILENA_OK) { free(coordinates); return status; }
    size_t destination = 0;
    for (size_t index = 0; index < source->size; ++index) {
        bool take = false;
        size_t mask_offset = linear_offset(mask, index, coordinates);
        memcpy(&take, mask->storage->data + mask_offset, sizeof(take));
        if (take) {
            size_t source_offset = linear_offset(source, index, coordinates);
            memcpy(temporary.storage->data + destination * source->itemsize,
                   source->storage->data + source_offset, source->itemsize);
            ++destination;
        }
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_nonzero(MilenaArray *out, const MilenaArray *mask,
                                  MilenaError *error) {
    const MilenaArray *inputs[] = {mask};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(mask, error);
    if (status != MILENA_OK) return status;
    if (mask->dtype != MILENA_DTYPE_BOOL) {
        array_error(error, MILENA_ERR_TYPE,
                    "nonzero requiere una máscara bool");
        return MILENA_ERR_TYPE;
    }
    size_t *coordinates = mask->ndim == 0 ? NULL :
        (size_t *)calloc(mask->ndim, sizeof(size_t));
    if (mask->ndim != 0 && coordinates == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas");
        return MILENA_ERR_MEMORY;
    }
    size_t selected = 0;
    for (size_t index = 0; index < mask->size; ++index) {
        bool take = false;
        size_t offset = linear_offset(mask, index, coordinates);
        memcpy(&take, mask->storage->data + offset, sizeof(take));
        if (take) ++selected;
    }
    if (mask->size != 0 && mask->size - 1u > (size_t)INT64_MAX) {
        free(coordinates);
        array_error(error, MILENA_ERR_OVERFLOW,
                    "Un índice nonzero no cabe en int64");
        return MILENA_ERR_OVERFLOW;
    }
    size_t output_shape[] = {selected};
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_INT64, 1,
                            output_shape, error);
    if (status != MILENA_OK) { free(coordinates); return status; }
    size_t destination = 0;
    for (size_t index = 0; index < mask->size; ++index) {
        bool take = false;
        size_t offset = linear_offset(mask, index, coordinates);
        memcpy(&take, mask->storage->data + offset, sizeof(take));
        if (take) {
            int64_t value = (int64_t)index;
            memcpy(temporary.storage->data + destination * sizeof(value),
                   &value, sizeof(value));
            ++destination;
        }
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_where(MilenaArray *out,
                                const MilenaArray *condition,
                                const MilenaArray *when_true,
                                const MilenaArray *when_false,
                                MilenaError *error) {
    const MilenaArray *inputs[] = {condition, when_true, when_false};
    MilenaStatus status = prepare_output(out, inputs, 3, error);
    if (status != MILENA_OK) return status;
    status = validate_input(condition, error);
    if (status == MILENA_OK) status = validate_input(when_true, error);
    if (status == MILENA_OK) status = validate_input(when_false, error);
    if (status != MILENA_OK) return status;
    if (condition->dtype != MILENA_DTYPE_BOOL) {
        array_error(error, MILENA_ERR_TYPE,
                    "where requiere una condición bool");
        return MILENA_ERR_TYPE;
    }
    if (!same_shape(condition, when_true) ||
        !same_shape(when_true, when_false)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "where requiere formas idénticas");
        return MILENA_ERR_ARGUMENT;
    }
    if (when_true->dtype != when_false->dtype ||
        when_true->itemsize != when_false->itemsize) {
        array_error(error, MILENA_ERR_TYPE,
                    "Las ramas de where requieren dtype e itemsize iguales");
        return MILENA_ERR_TYPE;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, when_true->dtype, when_true->ndim,
                            when_true->shape, error);
    if (status != MILENA_OK) return status;
    size_t *coordinates = when_true->ndim == 0 ? NULL :
        (size_t *)calloc(when_true->ndim, sizeof(size_t));
    if (when_true->ndim != 0 && coordinates == NULL) {
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas de where");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < when_true->size; ++index) {
        bool take_true = false;
        size_t condition_offset = linear_offset(condition, index, coordinates);
        size_t true_offset = linear_offset(when_true, index, coordinates);
        size_t false_offset = linear_offset(when_false, index, coordinates);
        memcpy(&take_true, condition->storage->data + condition_offset,
               sizeof(take_true));
        const unsigned char *selected = take_true ?
            when_true->storage->data + true_offset :
            when_false->storage->data + false_offset;
        memcpy(temporary.storage->data + index * temporary.itemsize,
               selected, temporary.itemsize);
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

/* -------------------------------------------------------------------------
 * Central real-dtype dispatcher and numerical runtime (Worker 3).
 * Complex dtypes remain valid storage dtypes, but every numerical operation
 * below rejects them uniformly with MILENA_ERR_UNSUPPORTED.
 * ------------------------------------------------------------------------- */

typedef enum {
    NUMERIC_BOOL,
    NUMERIC_SIGNED,
    NUMERIC_UNSIGNED,
    NUMERIC_FLOAT
} NumericKind;

typedef struct {
    NumericKind kind;
    union {
        bool boolean;
        int64_t signed_value;
        uint64_t unsigned_value;
        long double float_value;
    } as;
} NumericValue;

typedef void (*NumericLoader)(const unsigned char *, NumericValue *);

typedef struct {
    NumericKind kind;
    unsigned bits;
    NumericLoader load;
} DTypeKernel;

#define DEFINE_SIGNED_LOADER(name, type)                                      \
    static void name(const unsigned char *data, NumericValue *out) {          \
        type value = 0;                                                        \
        memcpy(&value, data, sizeof(value));                                   \
        out->kind = NUMERIC_SIGNED;                                            \
        out->as.signed_value = (int64_t)value;                                 \
    }
#define DEFINE_UNSIGNED_LOADER(name, type)                                    \
    static void name(const unsigned char *data, NumericValue *out) {          \
        type value = 0;                                                        \
        memcpy(&value, data, sizeof(value));                                   \
        out->kind = NUMERIC_UNSIGNED;                                          \
        out->as.unsigned_value = (uint64_t)value;                              \
    }

static void load_bool_value(const unsigned char *data, NumericValue *out) {
    bool value = false;
    memcpy(&value, data, sizeof(value));
    out->kind = NUMERIC_BOOL;
    out->as.boolean = value;
}
DEFINE_SIGNED_LOADER(load_i8_value, int8_t)
DEFINE_SIGNED_LOADER(load_i16_value, int16_t)
DEFINE_SIGNED_LOADER(load_i32_value, int32_t)
DEFINE_SIGNED_LOADER(load_i64_value, int64_t)
DEFINE_UNSIGNED_LOADER(load_u8_value, uint8_t)
DEFINE_UNSIGNED_LOADER(load_u16_value, uint16_t)
DEFINE_UNSIGNED_LOADER(load_u32_value, uint32_t)
DEFINE_UNSIGNED_LOADER(load_u64_value, uint64_t)

static void load_f32_value(const unsigned char *data, NumericValue *out) {
    float value = 0.0f;
    memcpy(&value, data, sizeof(value));
    out->kind = NUMERIC_FLOAT;
    out->as.float_value = (long double)value;
}

static void load_f64_value(const unsigned char *data, NumericValue *out) {
    double value = 0.0;
    memcpy(&value, data, sizeof(value));
    out->kind = NUMERIC_FLOAT;
    out->as.float_value = (long double)value;
}

static const DTypeKernel dtype_kernels[] = {
    {NUMERIC_BOOL, 1u, load_bool_value},
    {NUMERIC_SIGNED, 8u, load_i8_value},
    {NUMERIC_SIGNED, 16u, load_i16_value},
    {NUMERIC_SIGNED, 32u, load_i32_value},
    {NUMERIC_SIGNED, 64u, load_i64_value},
    {NUMERIC_UNSIGNED, 8u, load_u8_value},
    {NUMERIC_UNSIGNED, 16u, load_u16_value},
    {NUMERIC_UNSIGNED, 32u, load_u32_value},
    {NUMERIC_UNSIGNED, 64u, load_u64_value},
    {NUMERIC_FLOAT, 32u, load_f32_value},
    {NUMERIC_FLOAT, 64u, load_f64_value}
};

#undef DEFINE_SIGNED_LOADER
#undef DEFINE_UNSIGNED_LOADER

static bool dtype_is_real(MilenaDType dtype) {
    return dtype >= MILENA_DTYPE_BOOL && dtype <= MILENA_DTYPE_FLOAT64;
}

static const DTypeKernel *dtype_kernel(MilenaDType dtype) {
    if (!dtype_is_real(dtype)) return NULL;
    return &dtype_kernels[(size_t)dtype];
}

static NumericValue load_numeric(const MilenaArray *array, size_t offset) {
    NumericValue value;
    const DTypeKernel *kernel = dtype_kernel(array->dtype);
    memset(&value, 0, sizeof(value));
    if (kernel != NULL) kernel->load(array->storage->data + offset, &value);
    return value;
}

static bool numeric_is_zero(NumericValue value) {
    if (value.kind == NUMERIC_FLOAT) return value.as.float_value == 0.0L;
    if (value.kind == NUMERIC_SIGNED) return value.as.signed_value == 0;
    if (value.kind == NUMERIC_UNSIGNED) return value.as.unsigned_value == 0u;
    return !value.as.boolean;
}

static long double numeric_as_long_double(NumericValue value) {
    if (value.kind == NUMERIC_FLOAT) return value.as.float_value;
    if (value.kind == NUMERIC_SIGNED) return (long double)value.as.signed_value;
    if (value.kind == NUMERIC_UNSIGNED)
        return (long double)value.as.unsigned_value;
    return value.as.boolean ? 1.0L : 0.0L;
}

static bool numeric_as_i64(NumericValue value, int64_t *out) {
    if (value.kind == NUMERIC_SIGNED) {
        *out = value.as.signed_value;
        return true;
    }
    if (value.kind == NUMERIC_UNSIGNED) {
        if (value.as.unsigned_value > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)value.as.unsigned_value;
        return true;
    }
    if (value.kind == NUMERIC_BOOL) {
        *out = value.as.boolean ? 1 : 0;
        return true;
    }
    return false;
}

static bool numeric_as_u64(NumericValue value, uint64_t *out) {
    if (value.kind == NUMERIC_UNSIGNED) {
        *out = value.as.unsigned_value;
        return true;
    }
    if (value.kind == NUMERIC_SIGNED) {
        if (value.as.signed_value < 0) return false;
        *out = (uint64_t)value.as.signed_value;
        return true;
    }
    if (value.kind == NUMERIC_BOOL) {
        *out = value.as.boolean ? 1u : 0u;
        return true;
    }
    return false;
}

static MilenaStatus unsupported_complex(MilenaError *error) {
    array_error(error, MILENA_ERR_UNSUPPORTED,
                "complex64/complex128 son dtypes solo de almacenamiento");
    return MILENA_ERR_UNSUPPORTED;
}

static uint64_t unsigned_max_for_dtype(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_UINT8: return UINT8_MAX;
        case MILENA_DTYPE_UINT16: return UINT16_MAX;
        case MILENA_DTYPE_UINT32: return UINT32_MAX;
        case MILENA_DTYPE_UINT64: return UINT64_MAX;
        default: return 1u;
    }
}

static int64_t signed_min_for_dtype(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_INT8: return INT8_MIN;
        case MILENA_DTYPE_INT16: return INT16_MIN;
        case MILENA_DTYPE_INT32: return INT32_MIN;
        default: return INT64_MIN;
    }
}

static int64_t signed_max_for_dtype(MilenaDType dtype) {
    switch (dtype) {
        case MILENA_DTYPE_INT8: return INT8_MAX;
        case MILENA_DTYPE_INT16: return INT16_MAX;
        case MILENA_DTYPE_INT32: return INT32_MAX;
        default: return INT64_MAX;
    }
}

static MilenaStatus store_numeric(unsigned char *destination,
                                  MilenaDType dtype, NumericValue value,
                                  MilenaError *error) {
    if (!dtype_is_real(dtype)) return unsupported_complex(error);
    if (dtype == MILENA_DTYPE_BOOL) {
        bool converted = value.kind == NUMERIC_FLOAT ?
            value.as.float_value != 0.0L : !numeric_is_zero(value);
        memcpy(destination, &converted, sizeof(converted));
        return MILENA_OK;
    }
    const DTypeKernel *target = dtype_kernel(dtype);
    if (target != NULL && target->kind == NUMERIC_SIGNED) {
        int64_t converted = 0;
        if (value.kind == NUMERIC_FLOAT) {
            long double number = value.as.float_value;
            const long double lower = -ldexpl(1.0L, 63);
            const long double upper = ldexpl(1.0L, 63);
            if (!isfinite(number)) {
                array_error(error, MILENA_ERR_TYPE,
                            "NaN o infinito no se puede convertir a entero");
                return MILENA_ERR_TYPE;
            }
            number = truncl(number);
            if (number < lower || number >= upper) goto range;
            converted = (int64_t)number;
        } else if (!numeric_as_i64(value, &converted)) {
            goto range;
        }
        if (converted < signed_min_for_dtype(dtype) ||
            converted > signed_max_for_dtype(dtype)) goto range;
        switch (dtype) {
            case MILENA_DTYPE_INT8: { int8_t x = (int8_t)converted; memcpy(destination, &x, sizeof(x)); break; }
            case MILENA_DTYPE_INT16: { int16_t x = (int16_t)converted; memcpy(destination, &x, sizeof(x)); break; }
            case MILENA_DTYPE_INT32: { int32_t x = (int32_t)converted; memcpy(destination, &x, sizeof(x)); break; }
            default: memcpy(destination, &converted, sizeof(converted)); break;
        }
        return MILENA_OK;
    }
    if (target != NULL && target->kind == NUMERIC_UNSIGNED) {
        uint64_t converted = 0;
        if (value.kind == NUMERIC_FLOAT) {
            long double number = value.as.float_value;
            const long double upper = ldexpl(1.0L, 64);
            if (!isfinite(number)) {
                array_error(error, MILENA_ERR_TYPE,
                            "NaN o infinito no se puede convertir a entero");
                return MILENA_ERR_TYPE;
            }
            number = truncl(number);
            if (number < 0.0L || number >= upper) goto range;
            converted = (uint64_t)number;
        } else if (!numeric_as_u64(value, &converted)) {
            goto range;
        }
        if (converted > unsigned_max_for_dtype(dtype)) goto range;
        switch (dtype) {
            case MILENA_DTYPE_UINT8: { uint8_t x = (uint8_t)converted; memcpy(destination, &x, sizeof(x)); break; }
            case MILENA_DTYPE_UINT16: { uint16_t x = (uint16_t)converted; memcpy(destination, &x, sizeof(x)); break; }
            case MILENA_DTYPE_UINT32: { uint32_t x = (uint32_t)converted; memcpy(destination, &x, sizeof(x)); break; }
            default: memcpy(destination, &converted, sizeof(converted)); break;
        }
        return MILENA_OK;
    }
    {
        long double number = numeric_as_long_double(value);
        if (dtype == MILENA_DTYPE_FLOAT32) {
            if (isfinite(number) && fabsl(number) > (long double)FLT_MAX)
                goto range;
            float converted = (float)number;
            memcpy(destination, &converted, sizeof(converted));
        } else {
            if (isfinite(number) && fabsl(number) > (long double)DBL_MAX)
                goto range;
            double converted = (double)number;
            memcpy(destination, &converted, sizeof(converted));
        }
        return MILENA_OK;
    }
range:
    array_error(error, MILENA_ERR_OVERFLOW,
                "El valor no es representable en el dtype de destino");
    return MILENA_ERR_OVERFLOW;
}

MilenaStatus milena_dtype_promote(MilenaDType left, MilenaDType right,
                                  MilenaDType *result, MilenaError *error) {
    if (result == NULL) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Falta la salida de promoción de dtype");
        return MILENA_ERR_ARGUMENT;
    }
    if (milena_dtype_size(left) == 0 || milena_dtype_size(right) == 0) {
        array_error(error, MILENA_ERR_TYPE, "dtype inválido en promoción");
        return MILENA_ERR_TYPE;
    }
    if (!dtype_is_real(left) || !dtype_is_real(right))
        return unsupported_complex(error);
    const DTypeKernel *a = dtype_kernel(left);
    const DTypeKernel *b = dtype_kernel(right);
    if (a->kind == NUMERIC_FLOAT || b->kind == NUMERIC_FLOAT) {
        if (left == MILENA_DTYPE_FLOAT64 || right == MILENA_DTYPE_FLOAT64) {
            *result = MILENA_DTYPE_FLOAT64;
        } else if ((a->kind != NUMERIC_FLOAT && a->bits > 16u) ||
                   (b->kind != NUMERIC_FLOAT && b->bits > 16u)) {
            *result = MILENA_DTYPE_FLOAT64;
        } else {
            *result = MILENA_DTYPE_FLOAT32;
        }
        return MILENA_OK;
    }
    NumericKind ak = a->kind == NUMERIC_BOOL ? NUMERIC_UNSIGNED : a->kind;
    NumericKind bk = b->kind == NUMERIC_BOOL ? NUMERIC_UNSIGNED : b->kind;
    unsigned abits = a->kind == NUMERIC_BOOL ? 8u : a->bits;
    unsigned bbits = b->kind == NUMERIC_BOOL ? 8u : b->bits;
    unsigned bits = abits > bbits ? abits : bbits;
    if (ak == bk) {
        static const MilenaDType signed_types[] = {
            MILENA_DTYPE_INT8, MILENA_DTYPE_INT16,
            MILENA_DTYPE_INT32, MILENA_DTYPE_INT64
        };
        static const MilenaDType unsigned_types[] = {
            MILENA_DTYPE_UINT8, MILENA_DTYPE_UINT16,
            MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64
        };
        size_t index = bits <= 8u ? 0u : bits <= 16u ? 1u :
                       bits <= 32u ? 2u : 3u;
        *result = ak == NUMERIC_SIGNED ? signed_types[index] :
                                        unsigned_types[index];
        return MILENA_OK;
    }
    unsigned signed_bits = ak == NUMERIC_SIGNED ? abits : bbits;
    unsigned unsigned_bits = ak == NUMERIC_UNSIGNED ? abits : bbits;
    if (signed_bits > unsigned_bits) {
        *result = signed_bits <= 8u ? MILENA_DTYPE_INT8 :
                  signed_bits <= 16u ? MILENA_DTYPE_INT16 :
                  signed_bits <= 32u ? MILENA_DTYPE_INT32 :
                                       MILENA_DTYPE_INT64;
    } else if (unsigned_bits < 64u) {
        *result = unsigned_bits < 16u ? MILENA_DTYPE_INT16 :
                  unsigned_bits < 32u ? MILENA_DTYPE_INT32 :
                                        MILENA_DTYPE_INT64;
    } else {
        *result = MILENA_DTYPE_FLOAT64;
    }
    return MILENA_OK;
}

MilenaStatus milena_array_cast(MilenaArray *out, const MilenaArray *source,
                               MilenaDType dtype, MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (milena_dtype_size(dtype) == 0) {
        array_error(error, MILENA_ERR_TYPE, "dtype de destino inválido");
        return MILENA_ERR_TYPE;
    }
    if (!dtype_is_real(source->dtype) || !dtype_is_real(dtype))
        return unsupported_complex(error);
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, dtype, source->ndim,
                            source->shape, error);
    if (status != MILENA_OK) return status;
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (source->ndim != 0 && coordinates == NULL) {
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas de cast");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < source->size; ++index) {
        size_t offset = linear_offset(source, index, coordinates);
        NumericValue value = load_numeric(source, offset);
        status = store_numeric(temporary.storage->data +
                                   index * temporary.itemsize,
                               dtype, value, error);
        if (status != MILENA_OK) {
            free(coordinates);
            milena_array_release(&temporary);
            return status;
        }
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

static size_t shape_dimension(const MilenaArray *array, size_t output_axis,
                              size_t output_ndim) {
    size_t missing = output_ndim - array->ndim;
    return output_axis < missing ? 1u : array->shape[output_axis - missing];
}

static MilenaStatus binary_broadcast_shape(const MilenaArray *left,
                                           const MilenaArray *right,
                                           size_t *ndim, size_t **shape,
                                           MilenaError *error) {
    *ndim = left->ndim > right->ndim ? left->ndim : right->ndim;
    if (*ndim > SIZE_MAX / sizeof(size_t)) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "La metadata broadcast desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    *shape = *ndim == 0 ? NULL :
        (size_t *)malloc(*ndim * sizeof(size_t));
    if (*ndim != 0 && *shape == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar forma broadcast");
        return MILENA_ERR_MEMORY;
    }
    for (size_t axis = 0; axis < *ndim; ++axis) {
        size_t a = shape_dimension(left, axis, *ndim);
        size_t b = shape_dimension(right, axis, *ndim);
        if (a != b && a != 1u && b != 1u) {
            free(*shape);
            *shape = NULL;
            array_error(error, MILENA_ERR_ARGUMENT,
                        "Las formas no son compatibles para broadcasting");
            return MILENA_ERR_ARGUMENT;
        }
        (*shape)[axis] = a == 1u ? b : a;
    }
    return MILENA_OK;
}

static size_t broadcast_offset(const MilenaArray *array,
                               const size_t *coordinates,
                               size_t output_ndim) {
    size_t offset = array->byte_offset;
    size_t missing = output_ndim - array->ndim;
    for (size_t axis = 0; axis < array->ndim; ++axis) {
        size_t coordinate = array->shape[axis] == 1u ?
            0u : coordinates[axis + missing];
        size_t delta = coordinate * stride_magnitude(array->strides[axis]);
        if (array->strides[axis] < 0) offset -= delta;
        else offset += delta;
    }
    return offset;
}

static void increment_coordinates(size_t *coordinates, size_t ndim,
                                  const size_t *shape) {
    for (size_t axis = ndim; axis > 0; --axis) {
        size_t current = axis - 1u;
        ++coordinates[current];
        if (coordinates[current] < shape[current]) return;
        coordinates[current] = 0;
    }
}

static bool checked_add_i64(int64_t left, int64_t right, int64_t *result) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) return false;
    *result = left + right;
    return true;
}

static bool checked_sub_i64(int64_t left, int64_t right, int64_t *result) {
    if ((right < 0 && left > INT64_MAX + right) ||
        (right > 0 && left < INT64_MIN + right)) return false;
    *result = left - right;
    return true;
}

static bool checked_mul_i64(int64_t left, int64_t right, int64_t *result) {
    if (left == 0 || right == 0) { *result = 0; return true; }
    if ((left == -1 && right == INT64_MIN) ||
        (right == -1 && left == INT64_MIN)) return false;
    if (left > 0) {
        if ((right > 0 && left > INT64_MAX / right) ||
            (right < 0 && right < INT64_MIN / left)) return false;
    } else if ((right > 0 && left < INT64_MIN / right) ||
               (right < 0 && left < INT64_MAX / right)) return false;
    *result = left * right;
    return true;
}

static bool checked_add_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool checked_mul_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (right != 0u && left > UINT64_MAX / right) return false;
    *result = left * right;
    return true;
}

typedef enum {
    BINARY_ADD,
    BINARY_SUBTRACT,
    BINARY_MULTIPLY,
    BINARY_DIVIDE,
    BINARY_EQUAL,
    BINARY_LESS,
    BINARY_GREATER
} BinaryOperation;

static MilenaStatus calculate_binary(NumericValue left, NumericValue right,
                                     MilenaDType dtype,
                                     BinaryOperation operation,
                                     NumericValue *result,
                                     MilenaError *error) {
    const DTypeKernel *kernel = dtype_kernel(dtype);
    if (kernel->kind == NUMERIC_FLOAT) {
        long double a = numeric_as_long_double(left);
        long double b = numeric_as_long_double(right);
        if (operation == BINARY_DIVIDE && b == 0.0L) {
            array_error(error, MILENA_ERR_ARGUMENT, "División por cero");
            return MILENA_ERR_ARGUMENT;
        }
        result->kind = NUMERIC_FLOAT;
        if (operation == BINARY_ADD) result->as.float_value = a + b;
        else if (operation == BINARY_SUBTRACT) result->as.float_value = a - b;
        else if (operation == BINARY_MULTIPLY) result->as.float_value = a * b;
        else result->as.float_value = a / b;
        if (isfinite(a) && isfinite(b) &&
            !isfinite(result->as.float_value)) {
            array_error(error, MILENA_ERR_OVERFLOW,
                        "La operación flotante desbordó su rango");
            return MILENA_ERR_OVERFLOW;
        }
        return MILENA_OK;
    }
    if (kernel->kind == NUMERIC_SIGNED) {
        int64_t a = 0;
        int64_t b = 0;
        int64_t converted = 0;
        if (!numeric_as_i64(left, &a) || !numeric_as_i64(right, &b))
            goto overflow;
        bool valid = false;
        if (operation == BINARY_ADD) valid = checked_add_i64(a, b, &converted);
        else if (operation == BINARY_SUBTRACT)
            valid = checked_sub_i64(a, b, &converted);
        else if (operation == BINARY_MULTIPLY)
            valid = checked_mul_i64(a, b, &converted);
        else if (b == 0) {
            array_error(error, MILENA_ERR_ARGUMENT, "División por cero");
            return MILENA_ERR_ARGUMENT;
        } else if (a == INT64_MIN && b == -1) {
            valid = false;
        } else {
            converted = a / b;
            valid = true;
        }
        if (!valid) goto overflow;
        result->kind = NUMERIC_SIGNED;
        result->as.signed_value = converted;
        return MILENA_OK;
    }
    {
        uint64_t a = 0;
        uint64_t b = 0;
        uint64_t converted = 0;
        if (!numeric_as_u64(left, &a) || !numeric_as_u64(right, &b))
            goto overflow;
        bool valid = false;
        if (operation == BINARY_ADD) valid = checked_add_u64(a, b, &converted);
        else if (operation == BINARY_SUBTRACT) {
            valid = a >= b;
            if (valid) converted = a - b;
        } else if (operation == BINARY_MULTIPLY)
            valid = checked_mul_u64(a, b, &converted);
        else if (b == 0u) {
            array_error(error, MILENA_ERR_ARGUMENT, "División por cero");
            return MILENA_ERR_ARGUMENT;
        } else {
            converted = a / b;
            valid = true;
        }
        if (!valid) goto overflow;
        result->kind = NUMERIC_UNSIGNED;
        result->as.unsigned_value = converted;
        return MILENA_OK;
    }
overflow:
    array_error(error, MILENA_ERR_OVERFLOW,
                "La operación entera está fuera de rango");
    return MILENA_ERR_OVERFLOW;
}

static int compare_numeric(NumericValue left, NumericValue right,
                           MilenaDType promoted, bool *unordered) {
    *unordered = false;
    const DTypeKernel *kernel = dtype_kernel(promoted);
    if (kernel->kind == NUMERIC_FLOAT) {
        long double a = numeric_as_long_double(left);
        long double b = numeric_as_long_double(right);
        if (isnan(a) || isnan(b)) {
            *unordered = true;
            return 0;
        }
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (kernel->kind == NUMERIC_SIGNED) {
        int64_t a = 0;
        int64_t b = 0;
        (void)numeric_as_i64(left, &a);
        (void)numeric_as_i64(right, &b);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    {
        uint64_t a = 0;
        uint64_t b = 0;
        (void)numeric_as_u64(left, &a);
        (void)numeric_as_u64(right, &b);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
}

static MilenaStatus binary_operation(MilenaArray *out,
                                     const MilenaArray *left,
                                     const MilenaArray *right,
                                     BinaryOperation operation,
                                     MilenaError *error) {
    const MilenaArray *inputs[] = {left, right};
    MilenaStatus status = prepare_output(out, inputs, 2, error);
    if (status != MILENA_OK) return status;
    status = validate_input(left, error);
    if (status == MILENA_OK) status = validate_input(right, error);
    if (status != MILENA_OK) return status;
    MilenaDType result_dtype = MILENA_DTYPE_BOOL;
    status = milena_dtype_promote(left->dtype, right->dtype,
                                  &result_dtype, error);
    if (status != MILENA_OK) return status;
    if (operation == BINARY_DIVIDE &&
        dtype_kernel(result_dtype)->kind != NUMERIC_FLOAT)
        result_dtype = MILENA_DTYPE_FLOAT64;
    bool comparison = operation >= BINARY_EQUAL;
    size_t ndim = 0;
    size_t *shape = NULL;
    status = binary_broadcast_shape(left, right, &ndim, &shape, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary,
                            comparison ? MILENA_DTYPE_BOOL : result_dtype,
                            ndim, shape, error);
    if (status != MILENA_OK) { free(shape); return status; }
    size_t *coordinates = ndim == 0 ? NULL :
        (size_t *)calloc(ndim, sizeof(size_t));
    if (ndim != 0 && coordinates == NULL) {
        free(shape);
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas broadcast");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < temporary.size; ++index) {
        NumericValue a = load_numeric(left,
            broadcast_offset(left, coordinates, ndim));
        NumericValue b = load_numeric(right,
            broadcast_offset(right, coordinates, ndim));
        unsigned char *destination = temporary.storage->data +
                                     index * temporary.itemsize;
        if (comparison) {
            bool unordered = false;
            int order = compare_numeric(a, b, result_dtype, &unordered);
            bool answer = false;
            if (operation == BINARY_EQUAL) answer = !unordered && order == 0;
            else if (operation == BINARY_LESS)
                answer = !unordered && order < 0;
            else answer = !unordered && order > 0;
            memcpy(destination, &answer, sizeof(answer));
        } else {
            NumericValue result;
            status = calculate_binary(a, b, result_dtype, operation,
                                      &result, error);
            if (status == MILENA_OK)
                status = store_numeric(destination, result_dtype,
                                       result, error);
            if (status == MILENA_ERR_OVERFLOW &&
                dtype_kernel(result_dtype)->kind != NUMERIC_FLOAT)
                array_error(error, MILENA_ERR_OVERFLOW,
                            "La operación entera está fuera de rango");
            if (status != MILENA_OK) {
                free(shape);
                free(coordinates);
                milena_array_release(&temporary);
                return status;
            }
        }
        if (ndim != 0) increment_coordinates(coordinates, ndim, shape);
    }
    free(shape);
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_add(MilenaArray *out, const MilenaArray *left,
                              const MilenaArray *right, MilenaError *error) {
    return binary_operation(out, left, right, BINARY_ADD, error);
}
MilenaStatus milena_array_subtract(MilenaArray *out, const MilenaArray *left,
                                   const MilenaArray *right,
                                   MilenaError *error) {
    return binary_operation(out, left, right, BINARY_SUBTRACT, error);
}
MilenaStatus milena_array_multiply(MilenaArray *out, const MilenaArray *left,
                                   const MilenaArray *right,
                                   MilenaError *error) {
    return binary_operation(out, left, right, BINARY_MULTIPLY, error);
}
MilenaStatus milena_array_divide(MilenaArray *out, const MilenaArray *left,
                                 const MilenaArray *right,
                                 MilenaError *error) {
    return binary_operation(out, left, right, BINARY_DIVIDE, error);
}
MilenaStatus milena_array_equal(MilenaArray *out, const MilenaArray *left,
                                const MilenaArray *right,
                                MilenaError *error) {
    return binary_operation(out, left, right, BINARY_EQUAL, error);
}
MilenaStatus milena_array_less(MilenaArray *out, const MilenaArray *left,
                               const MilenaArray *right,
                               MilenaError *error) {
    return binary_operation(out, left, right, BINARY_LESS, error);
}
MilenaStatus milena_array_greater(MilenaArray *out, const MilenaArray *left,
                                  const MilenaArray *right,
                                  MilenaError *error) {
    return binary_operation(out, left, right, BINARY_GREATER, error);
}

static MilenaStatus unary_predicate(MilenaArray *out,
                                    const MilenaArray *source,
                                    bool finite_test,
                                    MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!dtype_is_real(source->dtype)) return unsupported_complex(error);
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_BOOL, source->ndim,
                            source->shape, error);
    if (status != MILENA_OK) return status;
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (source->ndim != 0 && coordinates == NULL) {
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas del predicado");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < source->size; ++index) {
        NumericValue value = load_numeric(source,
            linear_offset(source, index, coordinates));
        bool answer = finite_test;
        if (value.kind == NUMERIC_FLOAT) {
            answer = finite_test ? isfinite(value.as.float_value) :
                                   isnan(value.as.float_value);
        } else if (!finite_test) {
            answer = false;
        }
        memcpy(temporary.storage->data + index * sizeof(answer),
               &answer, sizeof(answer));
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_isnan(MilenaArray *out, const MilenaArray *source,
                                MilenaError *error) {
    return unary_predicate(out, source, false, error);
}

MilenaStatus milena_array_isfinite(MilenaArray *out,
                                   const MilenaArray *source,
                                   MilenaError *error) {
    return unary_predicate(out, source, true, error);
}

static MilenaStatus normalize_reduction_axis(const MilenaArray *source,
                                             int axis, int *normalized,
                                             MilenaError *error) {
    if (axis == -1) {
        *normalized = -1; /* ABI histórico: -1 significa reducción global. */
        return MILENA_OK;
    }
    if (axis >= 0) {
        if ((size_t)axis >= source->ndim) goto invalid;
        *normalized = axis;
        return MILENA_OK;
    }
    /* Conserva -1 global; -2, -3, ... cuentan desde el final. */
    size_t magnitude = (size_t)(-(axis + 1)) + 1u;
    if (magnitude > source->ndim) goto invalid;
    size_t converted = source->ndim - magnitude;
    if (converted > (size_t)INT_MAX) goto invalid;
    *normalized = (int)converted;
    return MILENA_OK;
invalid:
    array_error(error, MILENA_ERR_ARGUMENT,
                "Eje de reducción fuera de rango");
    return MILENA_ERR_ARGUMENT;
}

static MilenaStatus reduction_shape(const MilenaArray *source, int axis,
                                    bool keepdims, size_t *ndim,
                                    size_t **shape, MilenaError *error) {
    if (axis < -1 || (axis >= 0 && (size_t)axis >= source->ndim)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Eje de reducción fuera de rango");
        return MILENA_ERR_ARGUMENT;
    }
    if (axis == -1) {
        *ndim = keepdims ? source->ndim : 0u;
        if (*ndim > SIZE_MAX / sizeof(size_t)) goto overflow;
        *shape = *ndim == 0 ? NULL :
            (size_t *)malloc(*ndim * sizeof(size_t));
        if (*ndim != 0 && *shape == NULL) goto memory;
        for (size_t index = 0; index < *ndim; ++index) (*shape)[index] = 1u;
        return MILENA_OK;
    }
    *ndim = keepdims ? source->ndim : source->ndim - 1u;
    if (*ndim > SIZE_MAX / sizeof(size_t)) goto overflow;
    *shape = *ndim == 0 ? NULL :
        (size_t *)malloc(*ndim * sizeof(size_t));
    if (*ndim != 0 && *shape == NULL) goto memory;
    size_t destination = 0;
    for (size_t source_axis = 0; source_axis < source->ndim; ++source_axis) {
        if (source_axis == (size_t)axis) {
            if (keepdims) (*shape)[destination++] = 1u;
        } else {
            (*shape)[destination++] = source->shape[source_axis];
        }
    }
    return MILENA_OK;
overflow:
    array_error(error, MILENA_ERR_OVERFLOW,
                "La metadata de reducción desborda size_t");
    return MILENA_ERR_OVERFLOW;
memory:
    array_error(error, MILENA_ERR_MEMORY,
                "No se pudo reservar forma de reducción");
    return MILENA_ERR_MEMORY;
}

static void map_reduction_coordinates(const MilenaArray *source,
                                      int axis, bool keepdims,
                                      const size_t *output_coordinates,
                                      size_t *source_coordinates) {
    size_t destination = 0;
    for (size_t source_axis = 0; source_axis < source->ndim; ++source_axis) {
        if (source_axis == (size_t)axis) {
            source_coordinates[source_axis] = 0;
            if (keepdims) ++destination;
        } else {
            source_coordinates[source_axis] =
                output_coordinates[destination++];
        }
    }
}

static size_t reduction_offset(const MilenaArray *source, int axis,
                               size_t reduction_index,
                               size_t *source_coordinates) {
    if (axis == -1)
        return linear_offset(source, reduction_index, source_coordinates);
    source_coordinates[(size_t)axis] = reduction_index;
    return element_offset(source, source_coordinates);
}

static void compensated_add(long double value, long double *sum,
                            long double *correction) {
    if (!isfinite(value) || !isfinite(*sum)) {
        *sum += value;
        *correction = 0.0L;
        return;
    }
    long double next = *sum + value;
    if (fabsl(*sum) >= fabsl(value))
        *correction += (*sum - next) + value;
    else
        *correction += (value - next) + *sum;
    *sum = next;
}

static MilenaDType accumulation_dtype(MilenaDType dtype) {
    const DTypeKernel *kernel = dtype_kernel(dtype);
    if (kernel->kind == NUMERIC_FLOAT) return MILENA_DTYPE_FLOAT64;
    if (kernel->kind == NUMERIC_SIGNED) return MILENA_DTYPE_INT64;
    return MILENA_DTYPE_UINT64;
}

static MilenaStatus integer_reduce(MilenaArray *temporary, size_t output_index,
                                   const MilenaArray *source, int axis,
                                   size_t count, size_t *coordinates,
                                   bool product, MilenaError *error) {
    MilenaDType result_dtype = temporary->dtype;
    if (result_dtype == MILENA_DTYPE_INT64) {
        int64_t accumulator = product ? 1 : 0;
        for (size_t index = 0; index < count; ++index) {
            NumericValue value = load_numeric(source,
                reduction_offset(source, axis, index, coordinates));
            int64_t item = 0;
            (void)numeric_as_i64(value, &item);
            bool valid = product ? checked_mul_i64(accumulator, item,
                                                    &accumulator) :
                                   checked_add_i64(accumulator, item,
                                                    &accumulator);
            if (!valid) goto overflow;
        }
        memcpy(temporary->storage->data + output_index * sizeof(accumulator),
               &accumulator, sizeof(accumulator));
    } else {
        uint64_t accumulator = product ? 1u : 0u;
        for (size_t index = 0; index < count; ++index) {
            NumericValue value = load_numeric(source,
                reduction_offset(source, axis, index, coordinates));
            uint64_t item = 0;
            (void)numeric_as_u64(value, &item);
            bool valid = product ? checked_mul_u64(accumulator, item,
                                                    &accumulator) :
                                   checked_add_u64(accumulator, item,
                                                    &accumulator);
            if (!valid) goto overflow;
        }
        memcpy(temporary->storage->data + output_index * sizeof(accumulator),
               &accumulator, sizeof(accumulator));
    }
    return MILENA_OK;
overflow:
    array_error(error, MILENA_ERR_OVERFLOW,
                product ? "prod entero fuera de rango" :
                          "sum entero fuera de rango");
    return MILENA_ERR_OVERFLOW;
}

static MilenaStatus additive_reduction(MilenaArray *out,
                                       const MilenaArray *source,
                                       int axis, bool keepdims,
                                       bool product, MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!dtype_is_real(source->dtype)) return unsupported_complex(error);
    int normalized_axis = -1;
    status = normalize_reduction_axis(source, axis, &normalized_axis, error);
    if (status != MILENA_OK) return status;
    axis = normalized_axis;
    size_t output_ndim = 0;
    size_t *output_shape = NULL;
    status = reduction_shape(source, axis, keepdims, &output_ndim,
                             &output_shape, error);
    if (status != MILENA_OK) return status;
    MilenaDType result_dtype = accumulation_dtype(source->dtype);
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, result_dtype, output_ndim,
                            output_shape, error);
    if (status != MILENA_OK) { free(output_shape); return status; }
    size_t *source_coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    size_t *output_coordinates = output_ndim == 0 ? NULL :
        (size_t *)calloc(output_ndim, sizeof(size_t));
    if ((source->ndim != 0 && source_coordinates == NULL) ||
        (output_ndim != 0 && output_coordinates == NULL)) {
        free(source_coordinates);
        free(output_coordinates);
        free(output_shape);
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas de reducción");
        return MILENA_ERR_MEMORY;
    }
    size_t count = axis == -1 ? source->size :
                   source->shape[(size_t)axis];
    for (size_t output_index = 0; output_index < temporary.size;
         ++output_index) {
        if (output_ndim != 0)
            linear_coordinates(&temporary, output_index, output_coordinates);
        if (axis >= 0)
            map_reduction_coordinates(source, axis, keepdims,
                                      output_coordinates,
                                      source_coordinates);
        if (result_dtype == MILENA_DTYPE_FLOAT64) {
            long double accumulator = product ? 1.0L : 0.0L;
            long double correction = 0.0L;
            bool saw_nonfinite = false;
            for (size_t index = 0; index < count; ++index) {
                long double value = numeric_as_long_double(load_numeric(source,
                    reduction_offset(source, axis, index,
                                     source_coordinates)));
                if (!isfinite(value)) saw_nonfinite = true;
                if (product) accumulator *= value;
                else compensated_add(value, &accumulator, &correction);
            }
            NumericValue result;
            result.kind = NUMERIC_FLOAT;
            result.as.float_value = product ? accumulator :
                                              accumulator + correction;
            if (!saw_nonfinite && !isfinite(result.as.float_value)) {
                array_error(error, MILENA_ERR_OVERFLOW,
                            product ? "prod flotante fuera de rango" :
                                      "sum flotante fuera de rango");
                status = MILENA_ERR_OVERFLOW;
            } else {
                status = store_numeric(temporary.storage->data +
                                           output_index * sizeof(double),
                                       MILENA_DTYPE_FLOAT64, result, error);
            }
        } else {
            status = integer_reduce(&temporary, output_index, source, axis,
                                    count, source_coordinates, product,
                                    error);
        }
        if (status != MILENA_OK) {
            free(source_coordinates);
            free(output_coordinates);
            free(output_shape);
            milena_array_release(&temporary);
            return status;
        }
    }
    free(source_coordinates);
    free(output_coordinates);
    free(output_shape);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_sum(MilenaArray *out, const MilenaArray *source,
                              int axis, bool keepdims, MilenaError *error) {
    return additive_reduction(out, source, axis, keepdims, false, error);
}

MilenaStatus milena_array_prod(MilenaArray *out, const MilenaArray *source,
                               int axis, bool keepdims, MilenaError *error) {
    return additive_reduction(out, source, axis, keepdims, true, error);
}

typedef enum {
    STAT_MEAN,
    STAT_MIN,
    STAT_MAX,
    STAT_VARIANCE,
    STAT_STD
} Statistic;

static MilenaStatus stat_reduce(MilenaArray *out,
                                const MilenaArray *source,
                                int axis, bool keepdims,
                                Statistic statistic,
                                MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!dtype_is_real(source->dtype)) return unsupported_complex(error);
    int normalized_axis = -1;
    status = normalize_reduction_axis(source, axis, &normalized_axis, error);
    if (status != MILENA_OK) return status;
    axis = normalized_axis;
    size_t output_ndim = 0;
    size_t *shape = NULL;
    status = reduction_shape(source, axis, keepdims, &output_ndim,
                             &shape, error);
    if (status != MILENA_OK) return status;
    size_t count = axis == -1 ? source->size :
                   source->shape[(size_t)axis];
    if (count == 0) {
        free(shape);
        array_error(error, MILENA_ERR_ARGUMENT,
                    "La reducción estadística de un conjunto vacío no existe");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_FLOAT64,
                            output_ndim, shape, error);
    if (status != MILENA_OK) { free(shape); return status; }
    size_t *source_coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    size_t *output_coordinates = output_ndim == 0 ? NULL :
        (size_t *)calloc(output_ndim, sizeof(size_t));
    if ((source->ndim != 0 && source_coordinates == NULL) ||
        (output_ndim != 0 && output_coordinates == NULL)) {
        free(shape);
        free(source_coordinates);
        free(output_coordinates);
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas estadísticas");
        return MILENA_ERR_MEMORY;
    }
    for (size_t output_index = 0; output_index < temporary.size;
         ++output_index) {
        if (output_ndim != 0)
            linear_coordinates(&temporary, output_index, output_coordinates);
        if (axis >= 0)
            map_reduction_coordinates(source, axis, keepdims,
                                      output_coordinates,
                                      source_coordinates);
        long double result = 0.0L;
        if (statistic == STAT_MIN || statistic == STAT_MAX) {
            result = numeric_as_long_double(load_numeric(source,
                reduction_offset(source, axis, 0, source_coordinates)));
            for (size_t index = 1; index < count; ++index) {
                long double value = numeric_as_long_double(load_numeric(source,
                    reduction_offset(source, axis, index,
                                     source_coordinates)));
                if (isnan(result) || isnan(value)) {
                    result = NAN;
                    break;
                }
                if ((statistic == STAT_MIN && value < result) ||
                    (statistic == STAT_MAX && value > result)) result = value;
            }
        } else if (statistic == STAT_MEAN) {
            long double sum = 0.0L;
            long double correction = 0.0L;
            for (size_t index = 0; index < count; ++index) {
                long double value = numeric_as_long_double(load_numeric(source,
                    reduction_offset(source, axis, index,
                                     source_coordinates)));
                compensated_add(value, &sum, &correction);
            }
            result = (sum + correction) / (long double)count;
        } else {
            long double mean = 0.0L;
            long double m2 = 0.0L;
            for (size_t index = 0; index < count; ++index) {
                long double value = numeric_as_long_double(load_numeric(source,
                    reduction_offset(source, axis, index,
                                     source_coordinates)));
                long double n = (long double)(index + 1u);
                long double delta = value - mean;
                mean += delta / n;
                m2 += delta * (value - mean);
            }
            result = m2 / (long double)count;
            if (result < 0.0L && result > -LDBL_EPSILON) result = 0.0L;
            if (statistic == STAT_STD) result = sqrtl(result);
        }
        NumericValue converted;
        converted.kind = NUMERIC_FLOAT;
        converted.as.float_value = result;
        status = store_numeric(temporary.storage->data +
                                   output_index * sizeof(double),
                               MILENA_DTYPE_FLOAT64, converted, error);
        if (status != MILENA_OK) {
            free(shape);
            free(source_coordinates);
            free(output_coordinates);
            milena_array_release(&temporary);
            return status;
        }
    }
    free(shape);
    free(source_coordinates);
    free(output_coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_mean(MilenaArray *out, const MilenaArray *source,
                               MilenaError *error) {
    return stat_reduce(out, source, -1, false, STAT_MEAN, error);
}
MilenaStatus milena_array_min(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error) {
    return stat_reduce(out, source, -1, false, STAT_MIN, error);
}
MilenaStatus milena_array_max(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error) {
    return stat_reduce(out, source, -1, false, STAT_MAX, error);
}
MilenaStatus milena_array_variance(MilenaArray *out,
                                   const MilenaArray *source,
                                   MilenaError *error) {
    return stat_reduce(out, source, -1, false, STAT_VARIANCE, error);
}
MilenaStatus milena_array_std(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error) {
    return stat_reduce(out, source, -1, false, STAT_STD, error);
}
MilenaStatus milena_array_mean_axis(MilenaArray *out,
                                    const MilenaArray *source, int axis,
                                    bool keepdims, MilenaError *error) {
    return stat_reduce(out, source, axis, keepdims, STAT_MEAN, error);
}
MilenaStatus milena_array_min_axis(MilenaArray *out,
                                   const MilenaArray *source, int axis,
                                   bool keepdims, MilenaError *error) {
    return stat_reduce(out, source, axis, keepdims, STAT_MIN, error);
}
MilenaStatus milena_array_max_axis(MilenaArray *out,
                                   const MilenaArray *source, int axis,
                                   bool keepdims, MilenaError *error) {
    return stat_reduce(out, source, axis, keepdims, STAT_MAX, error);
}
MilenaStatus milena_array_variance_axis(MilenaArray *out,
                                        const MilenaArray *source, int axis,
                                        bool keepdims, MilenaError *error) {
    return stat_reduce(out, source, axis, keepdims, STAT_VARIANCE, error);
}
MilenaStatus milena_array_std_axis(MilenaArray *out,
                                   const MilenaArray *source, int axis,
                                   bool keepdims, MilenaError *error) {
    return stat_reduce(out, source, axis, keepdims, STAT_STD, error);
}

static int compare_doubles(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double interpolated_percentile(double *values, size_t count,
                                      double percentile) {
    qsort(values, count, sizeof(double), compare_doubles);
    double position = percentile * (double)(count - 1u) / 100.0;
    size_t lower = (size_t)floor(position);
    size_t upper = lower < count - 1u ? lower + 1u : lower;
    double fraction = position - (double)lower;
    if (upper == lower || fraction == 0.0) return values[lower];
    return values[lower] + fraction * (values[upper] - values[lower]);
}

static MilenaStatus percentile_reduce(MilenaArray *out,
                                      const MilenaArray *source,
                                      double percentile, int axis,
                                      bool keepdims,
                                      MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!dtype_is_real(source->dtype)) return unsupported_complex(error);
    if (!isfinite(percentile) || percentile < 0.0 || percentile > 100.0) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "El percentil debe estar en [0, 100]");
        return MILENA_ERR_ARGUMENT;
    }
    int normalized_axis = -1;
    status = normalize_reduction_axis(source, axis, &normalized_axis, error);
    if (status != MILENA_OK) return status;
    axis = normalized_axis;
    size_t output_ndim = 0;
    size_t *shape = NULL;
    status = reduction_shape(source, axis, keepdims, &output_ndim,
                             &shape, error);
    if (status != MILENA_OK) return status;
    size_t count = axis == -1 ? source->size :
                   source->shape[(size_t)axis];
    if (count == 0) {
        free(shape);
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Percentil indefinido para una reducción vacía");
        return MILENA_ERR_ARGUMENT;
    }
    if (count > SIZE_MAX / sizeof(double)) {
        free(shape);
        array_error(error, MILENA_ERR_OVERFLOW,
                    "La copia para percentil desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_FLOAT64,
                            output_ndim, shape, error);
    if (status != MILENA_OK) { free(shape); return status; }
    size_t *source_coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    size_t *output_coordinates = output_ndim == 0 ? NULL :
        (size_t *)calloc(output_ndim, sizeof(size_t));
    double *values = (double *)malloc(count * sizeof(double));
    if ((source->ndim != 0 && source_coordinates == NULL) ||
        (output_ndim != 0 && output_coordinates == NULL) || values == NULL) {
        free(shape);
        free(source_coordinates);
        free(output_coordinates);
        free(values);
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar copia para percentil");
        return MILENA_ERR_MEMORY;
    }
    for (size_t output_index = 0; output_index < temporary.size;
         ++output_index) {
        if (output_ndim != 0)
            linear_coordinates(&temporary, output_index, output_coordinates);
        if (axis >= 0)
            map_reduction_coordinates(source, axis, keepdims,
                                      output_coordinates,
                                      source_coordinates);
        bool has_nan = false;
        for (size_t index = 0; index < count; ++index) {
            long double value = numeric_as_long_double(load_numeric(source,
                reduction_offset(source, axis, index,
                                 source_coordinates)));
            values[index] = (double)value;
            if (isnan(value)) has_nan = true;
        }
        double result = has_nan ? NAN :
            interpolated_percentile(values, count, percentile);
        memcpy(temporary.storage->data + output_index * sizeof(result),
               &result, sizeof(result));
    }
    free(shape);
    free(source_coordinates);
    free(output_coordinates);
    free(values);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_percentile(MilenaArray *out,
                                     const MilenaArray *source,
                                     double percentile,
                                     MilenaError *error) {
    return percentile_reduce(out, source, percentile, -1, false, error);
}
MilenaStatus milena_array_median(MilenaArray *out,
                                 const MilenaArray *source,
                                 MilenaError *error) {
    return percentile_reduce(out, source, 50.0, -1, false, error);
}
MilenaStatus milena_array_percentile_axis(MilenaArray *out,
                                          const MilenaArray *source,
                                          double percentile, int axis,
                                          bool keepdims,
                                          MilenaError *error) {
    return percentile_reduce(out, source, percentile, axis, keepdims, error);
}
MilenaStatus milena_array_median_axis(MilenaArray *out,
                                      const MilenaArray *source, int axis,
                                      bool keepdims, MilenaError *error) {
    return percentile_reduce(out, source, 50.0, axis, keepdims, error);
}

static MilenaStatus arg_extreme(MilenaArray *out,
                                const MilenaArray *source, bool maximum,
                                MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!dtype_is_real(source->dtype)) return unsupported_complex(error);
    if (source->size == 0) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "argmin/argmax no existe para un array vacío");
        return MILENA_ERR_ARGUMENT;
    }
    if (source->size - 1u > (size_t)INT64_MAX) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "El índice extremo no cabe en int64");
        return MILENA_ERR_OVERFLOW;
    }
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (source->ndim != 0 && coordinates == NULL) {
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas de argmin/argmax");
        return MILENA_ERR_MEMORY;
    }
    NumericValue best = load_numeric(source,
        linear_offset(source, 0, coordinates));
    if (best.kind == NUMERIC_FLOAT && isnan(best.as.float_value)) {
        free(coordinates);
        array_error(error, MILENA_ERR_ARGUMENT,
                    "argmin/argmax rechaza NaN");
        return MILENA_ERR_ARGUMENT;
    }
    size_t best_index = 0;
    for (size_t index = 1; index < source->size; ++index) {
        NumericValue candidate = load_numeric(source,
            linear_offset(source, index, coordinates));
        if (candidate.kind == NUMERIC_FLOAT &&
            isnan(candidate.as.float_value)) {
            free(coordinates);
            array_error(error, MILENA_ERR_ARGUMENT,
                        "argmin/argmax rechaza NaN");
            return MILENA_ERR_ARGUMENT;
        }
        bool unordered = false;
        int order = compare_numeric(candidate, best, source->dtype,
                                    &unordered);
        if ((maximum && order > 0) || (!maximum && order < 0)) {
            best = candidate;
            best_index = index;
        }
    }
    free(coordinates);
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_INT64, 0, NULL, error);
    if (status != MILENA_OK) return status;
    int64_t converted = (int64_t)best_index;
    memcpy(temporary.storage->data, &converted, sizeof(converted));
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_argmin(MilenaArray *out,
                                 const MilenaArray *source,
                                 MilenaError *error) {
    return arg_extreme(out, source, false, error);
}
MilenaStatus milena_array_argmax(MilenaArray *out,
                                 const MilenaArray *source,
                                 MilenaError *error) {
    return arg_extreme(out, source, true, error);
}
