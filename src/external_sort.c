#include "external_sort.h"

#include <float.h>

static void sort_error(MilenaError *error, MilenaStatus status, const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}
static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}
static void encode_double_le(double value, unsigned char out[8]) {
    uint64_t bits = 0; memcpy(&bits, &value, sizeof(bits));
    for (size_t i = 0; i < 8; i++) out[i] = (unsigned char)(bits >> (8u * i));
}
static bool exists(const char *path) {
    FILE *file = fopen(path, "rb"); if (!file) return false;
    (void)fclose(file); return true;
}
static char *run_path(const char *prefix, size_t pass, size_t run) {
    int needed = snprintf(NULL, 0, "%s.p%zu.r%zu.spill", prefix, pass, run);
    if (needed < 0) return NULL;
    size_t capacity = (size_t)needed + 1;
    char *path = (char *)malloc(capacity);
    if (path) (void)snprintf(path, capacity, "%s.p%zu.r%zu.spill", prefix, pass, run);
    return path;
}
static bool disk_size(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb"); if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long end = ok ? ftell(file) : -1;
    if (end < 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok) *size = (size_t)end;
    return ok;
}
static void remove_runs(const char *prefix, size_t pass, size_t count) {
    for (size_t i = 0; i < count; i++) { char *path = run_path(prefix, pass, i); if (path) { (void)remove(path); free(path); } }
}
static MilenaStatus write_run(const char *path, const double *values, size_t count,
                              size_t quota, size_t *bytes_written,
                              MilenaError *error) {
    if (exists(path)) { sort_error(error, MILENA_ERR_ARGUMENT, "Existe un run temporal previo"); return MILENA_ERR_ARGUMENT; }
    MilenaSpillStore store = {0};
    MilenaStatus status = milena_spill_store_open(path, quota, sizeof(double), &store, error);
    if (status != MILENA_OK) return status;
    for (size_t i = 0; i < count; i++) {
        unsigned char record[sizeof(double)]; encode_double_le(values[i], record);
        status = milena_spill_store_append(&store, record, sizeof(record), error);
        if (status != MILENA_OK) break;
    }
    MilenaStatus close_status = milena_spill_store_close(&store, error);
    if (status == MILENA_OK && close_status != MILENA_OK) status = close_status;
    if (status != MILENA_OK) { (void)remove(path); return status; }
    if (!disk_size(path, bytes_written)) { (void)remove(path); sort_error(error, MILENA_ERR_IO, "No se pudo medir run temporal"); return MILENA_ERR_IO; }
    return MILENA_OK;
}
static size_t bounded_quota(size_t file_quota, size_t remaining) {
    return file_quota < remaining ? file_quota : remaining;
}

MilenaStatus milena_external_sort_doubles(
    MilenaDoubleSourceFn source, void *source_context,
    const char *run_prefix, const char *output_path,
    size_t run_capacity, size_t max_fan_in,
    size_t per_file_quota_bytes, size_t total_scratch_quota_bytes,
    MilenaError *error) {
    if (!source || !run_prefix || !run_prefix[0] || !output_path || !output_path[0] ||
        strcmp(run_prefix, output_path) == 0 || run_capacity == 0 || max_fan_in < 2 ||
        per_file_quota_bytes < MILENA_SPILL_HEADER_SIZE + sizeof(double) + MILENA_SPILL_TRAILER_SIZE ||
        total_scratch_quota_bytes < MILENA_SPILL_HEADER_SIZE + sizeof(double) + MILENA_SPILL_TRAILER_SIZE ||
        sizeof(double) != sizeof(uint64_t) ||
        DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024) {
        sort_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para ordenamiento externo");
        return MILENA_ERR_ARGUMENT;
    }
    if (exists(output_path)) { sort_error(error, MILENA_ERR_ARGUMENT, "La salida ya existe; no se sobrescribirá"); return MILENA_ERR_ARGUMENT; }
    size_t value_bytes = 0;
    if (!milena_size_mul(run_capacity, sizeof(double), &value_bytes)) {
        sort_error(error, MILENA_ERR_OVERFLOW, "Tamaño de lote de ordenamiento excedido"); return MILENA_ERR_OVERFLOW;
    }
    double *values = (double *)malloc(value_bytes);
    if (!values) { sort_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente para lote de ordenamiento"); return MILENA_ERR_MEMORY; }
    size_t run_count = 0, scratch_used = 0, chunk_count = 0, chunk_bytes = 0;
    MilenaStatus status = MILENA_OK; bool eof = false;
    while (!eof) {
        chunk_count = 0;
        while (chunk_count < run_capacity) {
            double value = 0.0; bool has_value = false;
            status = source(source_context, &value, &has_value, error);
            if (status != MILENA_OK) goto failed_initial;
            if (!has_value) { eof = true; break; }
            if (!isfinite(value)) { status = MILENA_ERR_DATA; sort_error(error, status, "Fuente contiene valor no finito"); goto failed_initial; }
            values[chunk_count++] = value;
        }
        if (chunk_count == 0) break;
        qsort(values, chunk_count, sizeof(*values), compare_double);
        char *path = run_path(run_prefix, 0, run_count);
        if (!path) { status = MILENA_ERR_MEMORY; sort_error(error, status, "No se pudo generar ruta de run"); goto failed_initial; }
        size_t remaining = total_scratch_quota_bytes - scratch_used;
        size_t quota = bounded_quota(per_file_quota_bytes, remaining);
        if (quota < MILENA_SPILL_HEADER_SIZE + sizeof(double) + MILENA_SPILL_TRAILER_SIZE) {
            free(path); status = MILENA_ERR_OVERFLOW; sort_error(error, status, "Cuota total de scratch agotada"); goto failed_initial;
        }
        status = write_run(path, values, chunk_count, quota, &chunk_bytes, error);
        free(path);
        if (status != MILENA_OK) goto failed_initial;
        if (chunk_bytes > remaining || run_count == SIZE_MAX) { status = MILENA_ERR_OVERFLOW; sort_error(error, status, "Cuota total de scratch agotada"); goto failed_initial; }
        scratch_used += chunk_bytes; run_count++;
    }
    free(values); values = NULL;
    if (run_count == 0) {
        size_t out_length = strlen(output_path), capacity = 0;
        if (!milena_size_add(out_length, sizeof(".partial"), &capacity)) { sort_error(error, MILENA_ERR_OVERFLOW, "Ruta de salida demasiado larga"); return MILENA_ERR_OVERFLOW; }
        char *temp = (char *)malloc(capacity);
        if (!temp) { sort_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente para salida vacía"); return MILENA_ERR_MEMORY; }
        (void)snprintf(temp, capacity, "%s.partial", output_path);
        if (exists(temp)) { free(temp); sort_error(error, MILENA_ERR_ARGUMENT, "Existe un temporal de salida previo"); return MILENA_ERR_ARGUMENT; }
        MilenaSpillStore store = {0}; status = milena_spill_store_open(temp, per_file_quota_bytes, sizeof(double), &store, error);
        if (status == MILENA_OK) status = milena_spill_store_close(&store, error);
        if (status == MILENA_OK && rename(temp, output_path) != 0) { status = MILENA_ERR_IO; sort_error(error, status, "No se pudo activar salida vacía"); }
        if (status != MILENA_OK) (void)remove(temp); free(temp);
        return status;
    }

    size_t pass = 0, next_created = 0;
    while (run_count > max_fan_in) {
        if (pass == SIZE_MAX) { status = MILENA_ERR_OVERFLOW; sort_error(error, status, "Demasiadas pasadas de ordenamiento"); goto failed_pass; }
        size_t next_pass = pass + 1;
        size_t next_count = run_count / max_fan_in + (run_count % max_fan_in != 0 ? 1u : 0u);
        size_t next_bytes = 0;
        next_created = 0;
        for (size_t group = 0; group < next_count; group++) {
            size_t start = group * max_fan_in;
            size_t group_count = run_count - start < max_fan_in ? run_count - start : max_fan_in;
            const char **inputs = (const char **)calloc(group_count, sizeof(*inputs));
            char **owned = (char **)calloc(group_count, sizeof(*owned));
            if (!inputs || !owned) { free(inputs); free(owned); status = MILENA_ERR_MEMORY; sort_error(error, status, "Memoria insuficiente para fusión de runs"); goto failed_next; }
            for (size_t i = 0; i < group_count; i++) { owned[i] = run_path(run_prefix, pass, start + i); if (!owned[i]) { status = MILENA_ERR_MEMORY; break; } inputs[i] = owned[i]; }
            char *out = run_path(run_prefix, next_pass, group);
            if (status != MILENA_OK || !out) { if (status == MILENA_OK) { status = MILENA_ERR_MEMORY; sort_error(error,status,"No se pudo generar ruta de run"); } for(size_t i=0;i<group_count;i++)free(owned[i]); free(owned); free(inputs); free(out); goto failed_next; }
            size_t remaining = total_scratch_quota_bytes - scratch_used;
            size_t quota = bounded_quota(per_file_quota_bytes, remaining);
            if (quota < MILENA_SPILL_HEADER_SIZE + sizeof(double) + MILENA_SPILL_TRAILER_SIZE) {
                for (size_t i = 0; i < group_count; i++) free(owned[i]); free(owned); free(inputs); free(out);
                status = MILENA_ERR_OVERFLOW; sort_error(error, status, "Cuota total de scratch agotada"); goto failed_next;
            }
            status = milena_external_merge_double_runs(inputs, group_count, out, max_fan_in, quota, error);
            for (size_t i = 0; i < group_count; i++) free(owned[i]); free(owned); free(inputs);
            if (status != MILENA_OK) { free(out); goto failed_next; }
            next_created++;
            size_t out_bytes = 0, accumulated_next = 0;
            if (!disk_size(out, &out_bytes) || out_bytes > total_scratch_quota_bytes - scratch_used ||
                !milena_size_add(next_bytes, out_bytes, &accumulated_next)) { free(out); status = MILENA_ERR_OVERFLOW; sort_error(error,status,"La fusión excede la cuota total de scratch"); goto failed_next; }
            scratch_used += out_bytes; next_bytes = accumulated_next; free(out);
        }
        remove_runs(run_prefix, pass, run_count); scratch_used = next_bytes;
        run_count = next_count; pass = next_pass;
    }

    {
        const char **inputs = (const char **)calloc(run_count, sizeof(*inputs));
        char **owned = (char **)calloc(run_count, sizeof(*owned));
        if (!inputs || !owned) { free(inputs); free(owned); status = MILENA_ERR_MEMORY; sort_error(error,status,"Memoria insuficiente para fusión final"); goto failed_pass; }
        for (size_t i = 0; i < run_count; i++) { owned[i] = run_path(run_prefix, pass, i); if (!owned[i]) { status = MILENA_ERR_MEMORY; break; } inputs[i] = owned[i]; }
        if (status == MILENA_OK) {
            size_t remaining = total_scratch_quota_bytes - scratch_used;
            size_t quota = bounded_quota(per_file_quota_bytes, remaining);
            status = milena_external_merge_double_runs(inputs, run_count, output_path, max_fan_in, quota, error);
        } else sort_error(error,status,"No se pudo generar ruta de run final");
        for (size_t i = 0; i < run_count; i++) free(owned[i]); free(owned); free(inputs);
    }
    if (status == MILENA_OK) remove_runs(run_prefix, pass, run_count);
    else goto failed_pass;
    if (error) milena_error_clear(error);
    return MILENA_OK;

failed_initial:
    free(values);
    remove_runs(run_prefix, 0, run_count);
    return status;
failed_next:
    remove_runs(run_prefix, pass + 1, next_created);
failed_pass:
    remove_runs(run_prefix, pass, run_count);
    return status;
}
