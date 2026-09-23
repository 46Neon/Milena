#include "external_merge.h"

#include <float.h>

static void merge_error(MilenaError *error, MilenaStatus status,
                        const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static uint64_t get_u64_le(const unsigned char *in) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value |= ((uint64_t)in[i]) << (8u * i);
    return value;
}
static void put_u64_le(unsigned char *out, uint64_t value) {
    for (size_t i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (8u * i));
}
static double decode_double(const unsigned char *bytes) {
    uint64_t bits = get_u64_le(bytes); double value = 0.0;
    memcpy(&value, &bits, sizeof(value)); return value;
}
static void encode_double(double value, unsigned char *bytes) {
    uint64_t bits = 0; memcpy(&bits, &value, sizeof(bits)); put_u64_le(bytes, bits);
}

typedef struct { double value; size_t run; bool present; } HeapNode;
static bool node_before(const HeapNode *a, const HeapNode *b) {
    if (a->value < b->value) return true;
    if (a->value > b->value) return false;
    return a->run < b->run;
}
static void heap_push(HeapNode *heap, size_t *length, HeapNode node) {
    size_t i = (*length)++;
    while (i > 0) { size_t parent = (i - 1) / 2; if (!node_before(&node, &heap[parent])) break; heap[i] = heap[parent]; i = parent; }
    heap[i] = node;
}
static HeapNode heap_pop(HeapNode *heap, size_t *length) {
    HeapNode top = heap[0], tail = heap[--(*length)];
    if (*length > 0) {
        size_t i = 0;
        for (;;) { size_t left = i * 2 + 1, right = left + 1; if (left >= *length) break;
            size_t child = right < *length && node_before(&heap[right], &heap[left]) ? right : left;
            if (!node_before(&heap[child], &tail)) break; heap[i] = heap[child]; i = child; }
        heap[i] = tail;
    }
    return top;
}

MilenaStatus milena_external_merge_double_runs(
    const char *const *input_paths, size_t input_count,
    const char *output_path, size_t max_fan_in,
    size_t per_file_quota_bytes, MilenaError *error) {
    if (!input_paths || input_count == 0 || !output_path || !output_path[0] ||
        max_fan_in < 2 || input_count > max_fan_in ||
        sizeof(double) != sizeof(uint64_t) || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024) {
        merge_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para fusión externa");
        return MILENA_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < input_count; i++) {
        if (!input_paths[i] || !input_paths[i][0] || strcmp(input_paths[i], output_path) == 0) {
            merge_error(error, MILENA_ERR_ARGUMENT, "Ruta de run inválida o igual a la salida"); return MILENA_ERR_ARGUMENT;
        }
    }
    size_t readers_bytes = 0, nodes_bytes = 0, previous_bytes = 0, flags_bytes = 0;
    if (!milena_size_mul(input_count, sizeof(MilenaSpillReader), &readers_bytes) ||
        !milena_size_mul(input_count, sizeof(HeapNode), &nodes_bytes) ||
        !milena_size_mul(input_count, sizeof(double), &previous_bytes) ||
        !milena_size_mul(input_count, sizeof(bool), &flags_bytes)) {
        merge_error(error, MILENA_ERR_OVERFLOW, "Demasiados runs para fusionar"); return MILENA_ERR_OVERFLOW;
    }
    MilenaSpillReader *readers = (MilenaSpillReader *)calloc(1, readers_bytes);
    HeapNode *heap = (HeapNode *)calloc(1, nodes_bytes);
    double *previous = (double *)calloc(1, previous_bytes);
    bool *have_previous = (bool *)calloc(1, flags_bytes);
    if (!readers || !heap || !previous || !have_previous) { free(readers); free(heap); free(previous); free(have_previous); merge_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente para fusionar runs"); return MILENA_ERR_MEMORY; }

    MilenaStatus status = MILENA_OK; size_t opened = 0, heap_size = 0;
    for (size_t i = 0; i < input_count; i++) {
        status = milena_spill_reader_open(input_paths[i], per_file_quota_bytes,
                                          sizeof(double), &readers[i], error);
        if (status != MILENA_OK) goto cleanup;
        opened++;
        unsigned char bytes[sizeof(double)]; size_t length = 0; bool has_record = false;
        status = milena_spill_reader_next(&readers[i], bytes, sizeof(bytes), &length, &has_record, error);
        if (status != MILENA_OK) goto cleanup;
        if (has_record) {
            if (length != sizeof(double)) { status = MILENA_ERR_DATA; merge_error(error, status, "Run contiene un registro de tamaño incorrecto"); goto cleanup; }
            double value = decode_double(bytes);
            if (!isfinite(value)) { status = MILENA_ERR_DATA; merge_error(error, status, "Run contiene un valor no finito"); goto cleanup; }
            heap_push(heap, &heap_size, (HeapNode){value, i, true});
        }
    }

    size_t path_length = strlen(output_path), temp_capacity = 0;
    if (!milena_size_add(path_length, sizeof(".partial"), &temp_capacity)) { status = MILENA_ERR_OVERFLOW; merge_error(error,status,"Ruta de salida demasiado larga"); goto cleanup; }
    char *temp_path = (char *)malloc(temp_capacity);
    if (!temp_path) { status = MILENA_ERR_MEMORY; merge_error(error,status,"Memoria insuficiente para ruta temporal"); goto cleanup; }
    (void)snprintf(temp_path, temp_capacity, "%s.partial", output_path);
    FILE *existing = fopen(output_path, "rb");
    if (existing) { fclose(existing); free(temp_path); status = MILENA_ERR_ARGUMENT; merge_error(error,status,"La salida ya existe; no se sobrescribirá"); goto cleanup; }
    existing = fopen(temp_path, "rb");
    if (existing) { fclose(existing); free(temp_path); status = MILENA_ERR_ARGUMENT; merge_error(error,status,"Existe una salida temporal previa"); goto cleanup; }

    MilenaSpillStore output = {0};
    status = milena_spill_store_open(temp_path, per_file_quota_bytes, sizeof(double), &output, error);
    if (status != MILENA_OK) { (void)remove(temp_path); free(temp_path); goto cleanup; }
    while (heap_size > 0) {
        HeapNode node = heap_pop(heap, &heap_size);
        if (have_previous[node.run] && node.value < previous[node.run]) {
            status = MILENA_ERR_DATA; merge_error(error,status,"Run de entrada no está ordenado"); break;
        }
        previous[node.run] = node.value; have_previous[node.run] = true;
        unsigned char bytes[sizeof(double)]; encode_double(node.value, bytes);
        status = milena_spill_store_append(&output, bytes, sizeof(bytes), error);
        if (status != MILENA_OK) break;
        unsigned char next_bytes[sizeof(double)]; size_t length = 0; bool has_record = false;
        status = milena_spill_reader_next(&readers[node.run], next_bytes, sizeof(next_bytes), &length, &has_record, error);
        if (status != MILENA_OK) break;
        if (has_record) {
            if (length != sizeof(double)) { status = MILENA_ERR_DATA; merge_error(error,status,"Run contiene un registro de tamaño incorrecto"); break; }
            double next = decode_double(next_bytes);
            if (!isfinite(next)) { status = MILENA_ERR_DATA; merge_error(error,status,"Run contiene un valor no finito"); break; }
            heap_push(heap, &heap_size, (HeapNode){next, node.run, true});
        }
    }
    MilenaStatus close_status = milena_spill_store_close(&output, error);
    if (status == MILENA_OK && close_status != MILENA_OK) status = close_status;
    if (status == MILENA_OK) {
        if (rename(temp_path, output_path) != 0) { status = MILENA_ERR_IO; merge_error(error,status,"No se pudo activar el resultado de la fusión"); }
    }
    if (status != MILENA_OK) (void)remove(temp_path);
    free(temp_path);

cleanup:
    for (size_t i = 0; i < opened; i++) (void)milena_spill_reader_close(&readers[i], NULL);
    free(readers); free(heap); free(previous); free(have_previous);
    if (status == MILENA_OK && error) milena_error_clear(error);
    return status;
}
