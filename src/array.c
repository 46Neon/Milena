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
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (source->dtype != MILENA_DTYPE_FLOAT64) {
        array_error(error, MILENA_ERR_TYPE,
                    "greater_f64 requiere float64");
        return MILENA_ERR_TYPE;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_BOOL, source->ndim,
                            source->shape, error);
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
        double value = 0.0;
        size_t offset = linear_offset(source, index, coordinates);
        memcpy(&value, source->storage->data + offset, sizeof(value));
        bool result = value > threshold;
        memcpy(temporary.storage->data + index * sizeof(bool),
               &result, sizeof(result));
    }
    free(coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
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

static bool read_real_at(const MilenaArray *source, size_t offset,
                         long double *value) {
#define READ_REAL(type) do { type temporary; memcpy(&temporary, source->storage->data + offset, sizeof(temporary)); *value = (long double)temporary; } while (0)
    switch (source->dtype) {
        case MILENA_DTYPE_BOOL: { bool temporary; memcpy(&temporary, source->storage->data + offset, sizeof(temporary)); *value = temporary ? 1.0L : 0.0L; } break;
        case MILENA_DTYPE_INT8: READ_REAL(int8_t); break;
        case MILENA_DTYPE_INT16: READ_REAL(int16_t); break;
        case MILENA_DTYPE_INT32: READ_REAL(int32_t); break;
        case MILENA_DTYPE_INT64: READ_REAL(int64_t); break;
        case MILENA_DTYPE_UINT8: READ_REAL(uint8_t); break;
        case MILENA_DTYPE_UINT16: READ_REAL(uint16_t); break;
        case MILENA_DTYPE_UINT32: READ_REAL(uint32_t); break;
        case MILENA_DTYPE_UINT64: READ_REAL(uint64_t); break;
        case MILENA_DTYPE_FLOAT32: READ_REAL(float); break;
        case MILENA_DTYPE_FLOAT64: READ_REAL(double); break;
        default: return false;
    }
#undef READ_REAL
    return true;
}

static MilenaStatus write_real(void *destination, MilenaDType dtype,
                               long double value, MilenaError *error) {
#define WRITE_REAL(type, converted) do { type temporary = (converted); memcpy(destination, &temporary, sizeof(temporary)); } while (0)
    if (dtype != MILENA_DTYPE_BOOL && dtype <= MILENA_DTYPE_UINT64 &&
        !isfinite(value)) {
        array_error(error, MILENA_ERR_TYPE,
                    "No se puede convertir NaN o infinito a entero");
        return MILENA_ERR_TYPE;
    }
    switch (dtype) {
        case MILENA_DTYPE_BOOL: WRITE_REAL(bool, value != 0.0L); break;
        case MILENA_DTYPE_INT8: if (value < INT8_MIN || value > INT8_MAX) goto range; WRITE_REAL(int8_t, (int8_t)value); break;
        case MILENA_DTYPE_INT16: if (value < INT16_MIN || value > INT16_MAX) goto range; WRITE_REAL(int16_t, (int16_t)value); break;
        case MILENA_DTYPE_INT32: if (value < INT32_MIN || value > INT32_MAX) goto range; WRITE_REAL(int32_t, (int32_t)value); break;
        case MILENA_DTYPE_INT64: if (value < (long double)INT64_MIN || value > (long double)INT64_MAX) goto range; WRITE_REAL(int64_t, (int64_t)value); break;
        case MILENA_DTYPE_UINT8: if (value < 0.0L || value > UINT8_MAX) goto range; WRITE_REAL(uint8_t, (uint8_t)value); break;
        case MILENA_DTYPE_UINT16: if (value < 0.0L || value > UINT16_MAX) goto range; WRITE_REAL(uint16_t, (uint16_t)value); break;
        case MILENA_DTYPE_UINT32: if (value < 0.0L || value > UINT32_MAX) goto range; WRITE_REAL(uint32_t, (uint32_t)value); break;
        case MILENA_DTYPE_UINT64: if (value < 0.0L || value > (long double)UINT64_MAX) goto range; WRITE_REAL(uint64_t, (uint64_t)value); break;
        case MILENA_DTYPE_FLOAT32: WRITE_REAL(float, (float)value); break;
        case MILENA_DTYPE_FLOAT64: WRITE_REAL(double, (double)value); break;
        default:
            array_error(error, MILENA_ERR_UNSUPPORTED,
                        "Conversión complex no implementada");
            return MILENA_ERR_UNSUPPORTED;
    }
#undef WRITE_REAL
    return MILENA_OK;
range:
#undef WRITE_REAL
    array_error(error, MILENA_ERR_OVERFLOW,
                "El valor no cabe en el dtype solicitado");
    return MILENA_ERR_OVERFLOW;
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
    if (source->dtype >= MILENA_DTYPE_COMPLEX64 ||
        dtype >= MILENA_DTYPE_COMPLEX64) {
        array_error(error, MILENA_ERR_UNSUPPORTED,
                    "Conversión complex no implementada");
        return MILENA_ERR_UNSUPPORTED;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, dtype, source->ndim,
                            source->shape, error);
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
        long double value = 0.0L;
        size_t offset = linear_offset(source, index, coordinates);
        (void)read_real_at(source, offset, &value);
        status = write_real(temporary.storage->data +
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

static bool checked_binary_i64(int64_t left, int64_t right, char operation,
                               int64_t *result) {
    if (operation == '+') {
        if ((right > 0 && left > INT64_MAX - right) ||
            (right < 0 && left < INT64_MIN - right)) return false;
        *result = left + right; return true;
    }
    if (operation == '-') {
        if ((right < 0 && left > INT64_MAX + right) ||
            (right > 0 && left < INT64_MIN + right)) return false;
        *result = left - right; return true;
    }
    if (operation == '*') return checked_mul_i64(left, right, result);
    if (right == 0 || (left == INT64_MIN && right == -1)) return false;
    *result = left / right;
    return true;
}

static MilenaStatus binary_operation(MilenaArray *out,
                                     const MilenaArray *left,
                                     const MilenaArray *right,
                                     char operation, MilenaError *error) {
    const MilenaArray *inputs[] = {left, right};
    MilenaStatus status = prepare_output(out, inputs, 2, error);
    if (status != MILENA_OK) return status;
    status = validate_input(left, error);
    if (status == MILENA_OK) status = validate_input(right, error);
    if (status != MILENA_OK) return status;
    if (left->dtype != right->dtype || left->itemsize != right->itemsize) {
        array_error(error, MILENA_ERR_TYPE,
                    "Los arrays deben tener el mismo dtype");
        return MILENA_ERR_TYPE;
    }
    if (left->dtype != MILENA_DTYPE_FLOAT64 &&
        left->dtype != MILENA_DTYPE_INT64) {
        array_error(error, MILENA_ERR_UNSUPPORTED,
                    "Operación no implementada para este dtype");
        return MILENA_ERR_UNSUPPORTED;
    }
    size_t ndim = 0;
    size_t *shape = NULL;
    status = binary_broadcast_shape(left, right, &ndim, &shape, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, left->dtype, ndim, shape, error);
    if (status != MILENA_OK) { free(shape); return status; }
    size_t *coordinates = ndim == 0 ? NULL :
        (size_t *)calloc(ndim, sizeof(size_t));
    if (ndim != 0 && coordinates == NULL) {
        free(shape); milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas broadcast");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < temporary.size; ++index) {
        size_t left_offset = broadcast_offset(left, coordinates, ndim);
        size_t right_offset = broadcast_offset(right, coordinates, ndim);
        unsigned char *destination = temporary.storage->data +
                                     index * temporary.itemsize;
        if (left->dtype == MILENA_DTYPE_FLOAT64) {
            double a = 0.0, b = 0.0, result = 0.0;
            memcpy(&a, left->storage->data + left_offset, sizeof(a));
            memcpy(&b, right->storage->data + right_offset, sizeof(b));
            if (operation == '+') result = a + b;
            else if (operation == '-') result = a - b;
            else if (operation == '*') result = a * b;
            else {
                if (b == 0.0) {
                    free(shape); free(coordinates);
                    milena_array_release(&temporary);
                    array_error(error, MILENA_ERR_ARGUMENT,
                                "División por cero");
                    return MILENA_ERR_ARGUMENT;
                }
                result = a / b;
            }
            memcpy(destination, &result, sizeof(result));
        } else {
            int64_t a = 0, b = 0, result = 0;
            memcpy(&a, left->storage->data + left_offset, sizeof(a));
            memcpy(&b, right->storage->data + right_offset, sizeof(b));
            if (!checked_binary_i64(a, b, operation, &result)) {
                free(shape); free(coordinates);
                milena_array_release(&temporary);
                MilenaStatus code = operation == '/' && b == 0 ?
                    MILENA_ERR_ARGUMENT : MILENA_ERR_OVERFLOW;
                array_error(error, code, code == MILENA_ERR_ARGUMENT ?
                            "División por cero" :
                            "Operación int64 fuera de rango");
                return code;
            }
            memcpy(destination, &result, sizeof(result));
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
    return binary_operation(out, left, right, '+', error);
}
MilenaStatus milena_array_subtract(MilenaArray *out, const MilenaArray *left,
                                   const MilenaArray *right,
                                   MilenaError *error) {
    return binary_operation(out, left, right, '-', error);
}
MilenaStatus milena_array_multiply(MilenaArray *out, const MilenaArray *left,
                                   const MilenaArray *right,
                                   MilenaError *error) {
    return binary_operation(out, left, right, '*', error);
}
MilenaStatus milena_array_divide(MilenaArray *out, const MilenaArray *left,
                                 const MilenaArray *right,
                                 MilenaError *error) {
    return binary_operation(out, left, right, '/', error);
}

static MilenaStatus reduction_shape(const MilenaArray *source, int axis,
                                    bool keepdims, size_t *ndim,
                                    size_t **shape, MilenaError *error) {
    if (axis < -1 || (axis >= 0 && (size_t)axis >= source->ndim)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Eje de reducción fuera de rango");
        return MILENA_ERR_ARGUMENT;
    }
    if (axis < 0) {
        *ndim = keepdims ? source->ndim : 0u;
        *shape = *ndim == 0 ? NULL :
            (size_t *)malloc(*ndim * sizeof(size_t));
        if (*ndim != 0 && *shape == NULL) goto memory;
        for (size_t index = 0; index < *ndim; ++index) (*shape)[index] = 1;
        return MILENA_OK;
    }
    *ndim = keepdims ? source->ndim : source->ndim - 1u;
    *shape = *ndim == 0 ? NULL :
        (size_t *)malloc(*ndim * sizeof(size_t));
    if (*ndim != 0 && *shape == NULL) goto memory;
    size_t destination = 0;
    for (size_t source_axis = 0; source_axis < source->ndim; ++source_axis) {
        if (source_axis == (size_t)axis) {
            if (keepdims) (*shape)[destination++] = 1;
        } else (*shape)[destination++] = source->shape[source_axis];
    }
    return MILENA_OK;
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
        } else source_coordinates[source_axis] = output_coordinates[destination++];
    }
}

MilenaStatus milena_array_sum(MilenaArray *out, const MilenaArray *source,
                              int axis, bool keepdims, MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (source->dtype != MILENA_DTYPE_FLOAT64 &&
        source->dtype != MILENA_DTYPE_INT64) {
        array_error(error, MILENA_ERR_UNSUPPORTED,
                    "sum no implementado para este dtype");
        return MILENA_ERR_UNSUPPORTED;
    }
    size_t output_ndim = 0;
    size_t *output_shape = NULL;
    status = reduction_shape(source, axis, keepdims, &output_ndim,
                             &output_shape, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, source->dtype, output_ndim,
                            output_shape, error);
    if (status != MILENA_OK) { free(output_shape); return status; }
    size_t *source_coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    size_t *output_coordinates = output_ndim == 0 ? NULL :
        (size_t *)calloc(output_ndim, sizeof(size_t));
    if ((source->ndim != 0 && source_coordinates == NULL) ||
        (output_ndim != 0 && output_coordinates == NULL)) {
        free(source_coordinates); free(output_coordinates); free(output_shape);
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas de sum");
        return MILENA_ERR_MEMORY;
    }
    for (size_t output_index = 0; output_index < temporary.size; ++output_index) {
        if (output_ndim != 0)
            linear_coordinates(&temporary, output_index, output_coordinates);
        size_t count = axis < 0 ? source->size : source->shape[(size_t)axis];
        if (axis >= 0)
            map_reduction_coordinates(source, axis, keepdims,
                                      output_coordinates, source_coordinates);
        if (source->dtype == MILENA_DTYPE_FLOAT64) {
            double result = 0.0;
            for (size_t index = 0; index < count; ++index) {
                size_t offset;
                if (axis < 0) offset = linear_offset(source, index,
                                                     source_coordinates);
                else {
                    source_coordinates[(size_t)axis] = index;
                    offset = element_offset(source, source_coordinates);
                }
                double value = 0.0;
                memcpy(&value, source->storage->data + offset, sizeof(value));
                result += value;
            }
            memcpy(temporary.storage->data + output_index * sizeof(result),
                   &result, sizeof(result));
        } else {
            int64_t result = 0;
            for (size_t index = 0; index < count; ++index) {
                size_t offset;
                if (axis < 0) offset = linear_offset(source, index,
                                                     source_coordinates);
                else {
                    source_coordinates[(size_t)axis] = index;
                    offset = element_offset(source, source_coordinates);
                }
                int64_t value = 0;
                memcpy(&value, source->storage->data + offset, sizeof(value));
                if ((value > 0 && result > INT64_MAX - value) ||
                    (value < 0 && result < INT64_MIN - value)) {
                    free(source_coordinates); free(output_coordinates);
                    free(output_shape); milena_array_release(&temporary);
                    array_error(error, MILENA_ERR_OVERFLOW,
                                "sum int64 fuera de rango");
                    return MILENA_ERR_OVERFLOW;
                }
                result += value;
            }
            memcpy(temporary.storage->data + output_index * sizeof(result),
                   &result, sizeof(result));
        }
    }
    free(source_coordinates); free(output_coordinates); free(output_shape);
    commit_output(out, &temporary);
    return MILENA_OK;
}

static bool supported_stat_dtype(MilenaDType dtype) {
    return dtype == MILENA_DTYPE_INT64 || dtype == MILENA_DTYPE_FLOAT64;
}

static double read_stat_value(const MilenaArray *source, size_t offset) {
    if (source->dtype == MILENA_DTYPE_INT64) {
        int64_t value = 0;
        memcpy(&value, source->storage->data + offset, sizeof(value));
        return (double)value;
    }
    double value = 0.0;
    memcpy(&value, source->storage->data + offset, sizeof(value));
    return value;
}

static MilenaStatus stat_all(MilenaArray *out, const MilenaArray *source,
                             char statistic, MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (source->size == 0 || !supported_stat_dtype(source->dtype)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "La estadística requiere elementos int64 o float64");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_FLOAT64, 0, NULL, error);
    if (status != MILENA_OK) return status;
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (source->ndim != 0 && coordinates == NULL) {
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas estadísticas");
        return MILENA_ERR_MEMORY;
    }
    double mean = 0.0;
    if (statistic == 'm' || statistic == 'v' || statistic == 's') {
        for (size_t index = 0; index < source->size; ++index)
            mean += read_stat_value(source,
                                    linear_offset(source, index, coordinates));
        mean /= (double)source->size;
    }
    double result = mean;
    if (statistic == 'n' || statistic == 'x') {
        result = read_stat_value(source, linear_offset(source, 0, coordinates));
        for (size_t index = 1; index < source->size; ++index) {
            double value = read_stat_value(source,
                                           linear_offset(source, index,
                                                         coordinates));
            if ((statistic == 'n' && value < result) ||
                (statistic == 'x' && value > result)) result = value;
        }
    } else if (statistic == 'v' || statistic == 's') {
        result = 0.0;
        for (size_t index = 0; index < source->size; ++index) {
            double delta = read_stat_value(source,
                                           linear_offset(source, index,
                                                         coordinates)) - mean;
            result += delta * delta;
        }
        result /= (double)source->size;
        if (statistic == 's') result = sqrt(result);
    }
    free(coordinates);
    memcpy(temporary.storage->data, &result, sizeof(result));
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_mean(MilenaArray *out, const MilenaArray *source,
                               MilenaError *error) {
    return stat_all(out, source, 'm', error);
}
MilenaStatus milena_array_min(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error) {
    return stat_all(out, source, 'n', error);
}
MilenaStatus milena_array_max(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error) {
    return stat_all(out, source, 'x', error);
}
MilenaStatus milena_array_variance(MilenaArray *out,
                                   const MilenaArray *source,
                                   MilenaError *error) {
    return stat_all(out, source, 'v', error);
}
MilenaStatus milena_array_std(MilenaArray *out, const MilenaArray *source,
                              MilenaError *error) {
    return stat_all(out, source, 's', error);
}

static MilenaStatus stat_axis(MilenaArray *out, const MilenaArray *source,
                              int axis, bool keepdims, char statistic,
                              MilenaError *error) {
    if (axis < 0) return stat_all(out, source, statistic, error);
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if ((size_t)axis >= source->ndim || source->size == 0 ||
        !supported_stat_dtype(source->dtype)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Reducción estadística por eje inválida");
        return MILENA_ERR_ARGUMENT;
    }
    size_t output_ndim = 0;
    size_t *shape = NULL;
    status = reduction_shape(source, axis, keepdims, &output_ndim,
                             &shape, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_FLOAT64,
                            output_ndim, shape, error);
    if (status != MILENA_OK) { free(shape); return status; }
    size_t *source_coordinates =
        (size_t *)calloc(source->ndim, sizeof(size_t));
    size_t *output_coordinates = output_ndim == 0 ? NULL :
        (size_t *)calloc(output_ndim, sizeof(size_t));
    if (source_coordinates == NULL ||
        (output_ndim != 0 && output_coordinates == NULL)) {
        free(shape); free(source_coordinates); free(output_coordinates);
        milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudieron reservar coordenadas estadísticas");
        return MILENA_ERR_MEMORY;
    }
    size_t count = source->shape[(size_t)axis];
    for (size_t output_index = 0; output_index < temporary.size;
         ++output_index) {
        if (output_ndim != 0)
            linear_coordinates(&temporary, output_index, output_coordinates);
        map_reduction_coordinates(source, axis, keepdims,
                                  output_coordinates, source_coordinates);
        double mean = 0.0;
        double result = 0.0;
        for (size_t index = 0; index < count; ++index) {
            source_coordinates[(size_t)axis] = index;
            double value = read_stat_value(source,
                element_offset(source, source_coordinates));
            mean += value;
            if (index == 0 || (statistic == 'n' && value < result) ||
                (statistic == 'x' && value > result)) result = value;
        }
        mean /= (double)count;
        if (statistic == 'm') result = mean;
        else if (statistic == 'v' || statistic == 's') {
            result = 0.0;
            for (size_t index = 0; index < count; ++index) {
                source_coordinates[(size_t)axis] = index;
                double delta = read_stat_value(source,
                    element_offset(source, source_coordinates)) - mean;
                result += delta * delta;
            }
            result /= (double)count;
            if (statistic == 's') result = sqrt(result);
        }
        memcpy(temporary.storage->data + output_index * sizeof(result),
               &result, sizeof(result));
    }
    free(shape); free(source_coordinates); free(output_coordinates);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_mean_axis(MilenaArray *out,
                                    const MilenaArray *source, int axis,
                                    bool keepdims, MilenaError *error) {
    return stat_axis(out, source, axis, keepdims, 'm', error);
}
MilenaStatus milena_array_min_axis(MilenaArray *out,
                                   const MilenaArray *source, int axis,
                                   bool keepdims, MilenaError *error) {
    return stat_axis(out, source, axis, keepdims, 'n', error);
}
MilenaStatus milena_array_max_axis(MilenaArray *out,
                                   const MilenaArray *source, int axis,
                                   bool keepdims, MilenaError *error) {
    return stat_axis(out, source, axis, keepdims, 'x', error);
}
MilenaStatus milena_array_variance_axis(MilenaArray *out,
                                        const MilenaArray *source, int axis,
                                        bool keepdims, MilenaError *error) {
    return stat_axis(out, source, axis, keepdims, 'v', error);
}
MilenaStatus milena_array_std_axis(MilenaArray *out,
                                   const MilenaArray *source, int axis,
                                   bool keepdims, MilenaError *error) {
    return stat_axis(out, source, axis, keepdims, 's', error);
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
    size_t lower = (size_t)position;
    size_t upper = lower < count - 1u ? lower + 1u : lower;
    return values[lower] + (position - (double)lower) *
           (values[upper] - values[lower]);
}

MilenaStatus milena_array_percentile(MilenaArray *out,
                                     const MilenaArray *source,
                                     double percentile,
                                     MilenaError *error) {
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!isfinite(percentile) || percentile < 0.0 || percentile > 100.0 ||
        source->size == 0 || !supported_stat_dtype(source->dtype)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Percentil o array inválido");
        return MILENA_ERR_ARGUMENT;
    }
    if (source->size > SIZE_MAX / sizeof(double)) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "La copia para percentil desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    double *values = (double *)malloc(source->size * sizeof(double));
    size_t *coordinates = source->ndim == 0 ? NULL :
        (size_t *)calloc(source->ndim, sizeof(size_t));
    if (values == NULL || (source->ndim != 0 && coordinates == NULL)) {
        free(values); free(coordinates);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar copia para percentil");
        return MILENA_ERR_MEMORY;
    }
    for (size_t index = 0; index < source->size; ++index)
        values[index] = read_stat_value(source,
                            linear_offset(source, index, coordinates));
    double result = interpolated_percentile(values, source->size, percentile);
    free(values); free(coordinates);
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_FLOAT64, 0, NULL, error);
    if (status != MILENA_OK) return status;
    memcpy(temporary.storage->data, &result, sizeof(result));
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_median(MilenaArray *out,
                                 const MilenaArray *source,
                                 MilenaError *error) {
    return milena_array_percentile(out, source, 50.0, error);
}

static MilenaStatus percentile_axis_impl(MilenaArray *out,
                                         const MilenaArray *source,
                                         double percentile, int axis,
                                         bool keepdims,
                                         MilenaError *error) {
    if (axis < 0)
        return milena_array_percentile(out, source, percentile, error);
    const MilenaArray *inputs[] = {source};
    MilenaStatus status = prepare_output(out, inputs, 1, error);
    if (status != MILENA_OK) return status;
    status = validate_input(source, error);
    if (status != MILENA_OK) return status;
    if (!isfinite(percentile) || percentile < 0.0 || percentile > 100.0 ||
        (size_t)axis >= source->ndim || source->size == 0 ||
        !supported_stat_dtype(source->dtype)) {
        array_error(error, MILENA_ERR_ARGUMENT,
                    "Percentil por eje inválido");
        return MILENA_ERR_ARGUMENT;
    }
    size_t count = source->shape[(size_t)axis];
    if (count > SIZE_MAX / sizeof(double)) {
        array_error(error, MILENA_ERR_OVERFLOW,
                    "La copia para percentil desborda size_t");
        return MILENA_ERR_OVERFLOW;
    }
    size_t output_ndim = 0;
    size_t *shape = NULL;
    status = reduction_shape(source, axis, keepdims, &output_ndim,
                             &shape, error);
    if (status != MILENA_OK) return status;
    MilenaArray temporary = {0};
    status = allocate_owned(&temporary, MILENA_DTYPE_FLOAT64,
                            output_ndim, shape, error);
    if (status != MILENA_OK) { free(shape); return status; }
    size_t *source_coordinates =
        (size_t *)calloc(source->ndim, sizeof(size_t));
    size_t *output_coordinates = output_ndim == 0 ? NULL :
        (size_t *)calloc(output_ndim, sizeof(size_t));
    double *values = (double *)malloc(count * sizeof(double));
    if (source_coordinates == NULL || values == NULL ||
        (output_ndim != 0 && output_coordinates == NULL)) {
        free(shape); free(source_coordinates); free(output_coordinates);
        free(values); milena_array_release(&temporary);
        array_error(error, MILENA_ERR_MEMORY,
                    "No se pudo reservar copia para percentil por eje");
        return MILENA_ERR_MEMORY;
    }
    for (size_t output_index = 0; output_index < temporary.size;
         ++output_index) {
        if (output_ndim != 0)
            linear_coordinates(&temporary, output_index, output_coordinates);
        map_reduction_coordinates(source, axis, keepdims,
                                  output_coordinates, source_coordinates);
        for (size_t index = 0; index < count; ++index) {
            source_coordinates[(size_t)axis] = index;
            values[index] = read_stat_value(source,
                element_offset(source, source_coordinates));
        }
        double result = interpolated_percentile(values, count, percentile);
        memcpy(temporary.storage->data + output_index * sizeof(result),
               &result, sizeof(result));
    }
    free(shape); free(source_coordinates); free(output_coordinates); free(values);
    commit_output(out, &temporary);
    return MILENA_OK;
}

MilenaStatus milena_array_percentile_axis(MilenaArray *out,
                                          const MilenaArray *source,
                                          double percentile, int axis,
                                          bool keepdims,
                                          MilenaError *error) {
    return percentile_axis_impl(out, source, percentile, axis, keepdims, error);
}

MilenaStatus milena_array_median_axis(MilenaArray *out,
                                      const MilenaArray *source, int axis,
                                      bool keepdims, MilenaError *error) {
    return percentile_axis_impl(out, source, 50.0, axis, keepdims, error);
}
