#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "grouped_aggregate.h"
#include "file_position.h"

#define GROUP_RECORD_FIXED (4u + MILENA_AGGREGATE_WIRE_SIZE)

static void group_error(MilenaError *error, MilenaStatus status,
                        const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static int key_compare(const unsigned char *a, size_t a_length,
                       const unsigned char *b, size_t b_length) {
    size_t common = a_length < b_length ? a_length : b_length;
    int cmp = common ? memcmp(a, b, common) : 0;
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return a_length < b_length ? -1 : (a_length > b_length ? 1 : 0);
}

static uint64_t key_hash(const unsigned char *key, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; ++i) {
        hash ^= key[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static unsigned char *entry_key(MilenaGroupedAggregate *grouped, size_t index) {
    return grouped->key_arena + index * grouped->max_key_bytes;
}

static size_t find_group(const MilenaGroupedAggregate *grouped,
                         const unsigned char *key, size_t key_length,
                         bool *found) {
    size_t mask = grouped->table_capacity - 1u;
    size_t slot = (size_t)key_hash(key, key_length) & mask;
    for (size_t probes = 0; probes < grouped->table_capacity; ++probes) {
        size_t value = grouped->table[slot];
        if (value == 0) { *found = false; return slot; }
        size_t index = value - 1u;
        const unsigned char *stored = grouped->key_arena + index * grouped->max_key_bytes;
        if (grouped->groups[index].key_length == key_length &&
            (key_length == 0 || memcmp(stored, key, key_length) == 0)) {
            *found = true;
            return slot;
        }
        slot = (slot + 1u) & mask;
    }
    *found = false;
    return grouped->table_capacity;
}

static void clear_groups(MilenaGroupedAggregate *grouped) {
    memset(grouped->table, 0, grouped->table_capacity * sizeof(*grouped->table));
    grouped->group_count = 0;
}

static bool table_size_for_groups(size_t groups, size_t *table_size) {
    if (groups == 0 || groups > SIZE_MAX / 2u) return false;
    size_t needed = groups * 2u;
    size_t size = 1u;
    while (size < needed) {
        if (size > SIZE_MAX / 2u) return false;
        size *= 2u;
    }
    *table_size = size;
    return true;
}

static MilenaStatus flush_groups(MilenaGroupedAggregate *grouped,
                                 MilenaError *error) {
    if (grouped->failed) return grouped->failure_status;
    if (grouped->group_count == 0) return MILENA_OK;
    size_t max_record;
    if (!milena_size_add(GROUP_RECORD_FIXED, grouped->max_key_bytes, &max_record)) {
        group_error(error, MILENA_ERR_OVERFLOW, "Tamaño de registro agrupado desbordado");
        return MILENA_ERR_OVERFLOW;
    }
    unsigned char *record = malloc(max_record);
    if (!record) {
        grouped->failed = true;
        grouped->failure_status = MILENA_ERR_MEMORY;
        group_error(error, MILENA_ERR_MEMORY, "Sin memoria para serializar grupos");
        return MILENA_ERR_MEMORY;
    }
    MilenaStatus status = MILENA_OK;
    for (size_t i = 0; i < grouped->group_count; ++i) {
        size_t key_length = grouped->groups[i].key_length;
        write_u32(record, (uint32_t)key_length);
        if (key_length) memcpy(record + 4, entry_key(grouped, i), key_length);
        size_t written = 0;
        status = milena_aggregate_state_encode(&grouped->groups[i].aggregate,
                                                record + 4 + key_length,
                                                MILENA_AGGREGATE_WIRE_SIZE,
                                                &written, error);
        if (status != MILENA_OK) break;
        status = milena_spill_store_append(&grouped->spill, record,
                                            4 + key_length + written, error);
        if (status != MILENA_OK) break;
    }
    free(record);
    if (status == MILENA_OK) clear_groups(grouped);
    else {
        /* A prefix may already have reached disk. The in-memory map still
         * contains every group, so retrying would duplicate that prefix. */
        grouped->failed = true;
        grouped->failure_status = status;
    }
    return status;
}

MilenaStatus milena_grouped_aggregate_open_with_max_runs(
    const char *spill_path, size_t memory_budget_bytes, size_t max_key_bytes,
    size_t spill_quota_bytes, size_t max_runs,
    MilenaGroupedAggregate *grouped, MilenaError *error) {
    if (!spill_path || !grouped || max_key_bytes == 0 ||
        max_key_bytes > UINT32_MAX || memory_budget_bytes == 0 ||
        max_runs == 0 || max_runs > MILENA_GROUPED_HARD_MAX_RUNS) {
        group_error(error, MILENA_ERR_ARGUMENT, "Configuración inválida para agregación agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    memset(grouped, 0, sizeof(*grouped));
    size_t max_record;
    if (!milena_size_add(GROUP_RECORD_FIXED, max_key_bytes, &max_record) ||
        max_record > SIZE_MAX / 2u || memory_budget_bytes < max_record * 4u) {
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para buffers acotados de agregación agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    /* Keep half of the budget available for external-sort batches/readers. */
    size_t map_budget = memory_budget_bytes / 2u;
    size_t entry_bytes;
    size_t index_overhead;
    if (!milena_size_add(sizeof(MilenaGroupedAggregateEntry), max_key_bytes, &entry_bytes) ||
        !milena_size_mul(2u, sizeof(size_t), &index_overhead) ||
        !milena_size_add(entry_bytes, index_overhead, &entry_bytes)) {
        group_error(error, MILENA_ERR_OVERFLOW, "Presupuesto de memoria agrupada desbordado");
        return MILENA_ERR_OVERFLOW;
    }
    size_t capacity = map_budget / entry_bytes;
    size_t table_capacity = 0;
    size_t group_bytes = 0, key_bytes = 0, table_bytes = 0, total_bytes = 0;
    while (capacity > 0) {
        if (!table_size_for_groups(capacity, &table_capacity) ||
            !milena_size_mul(capacity, sizeof(MilenaGroupedAggregateEntry), &group_bytes) ||
            !milena_size_mul(capacity, max_key_bytes, &key_bytes) ||
            !milena_size_mul(table_capacity, sizeof(size_t), &table_bytes) ||
            !milena_size_add(group_bytes, key_bytes, &total_bytes) ||
            !milena_size_add(total_bytes, table_bytes, &total_bytes)) {
            group_error(error, MILENA_ERR_OVERFLOW, "Presupuesto de memoria agrupada desbordado");
            return MILENA_ERR_OVERFLOW;
        }
        if (total_bytes <= map_budget) break;
        capacity--;
    }
    if (capacity == 0) {
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para un grupo");
        return MILENA_ERR_ARGUMENT;
    }
    grouped->groups = calloc(capacity, sizeof(*grouped->groups));
    grouped->key_arena = malloc(key_bytes);
    grouped->table = calloc(table_capacity, sizeof(*grouped->table));
    grouped->spill_path = milena_strdup(spill_path);
    if (!grouped->groups || !grouped->key_arena || !grouped->table || !grouped->spill_path) {
        milena_grouped_aggregate_close(grouped, NULL);
        group_error(error, MILENA_ERR_MEMORY, "Sin memoria para el mapa agrupado");
        return MILENA_ERR_MEMORY;
    }
    grouped->memory_budget_bytes = memory_budget_bytes;
    grouped->max_runs = max_runs;
    size_t map_used = group_bytes + key_bytes + table_bytes;
    size_t path_bytes = strlen(spill_path) + 1u;
    size_t temporary_path_bytes = 0, path_reserve = 0;
    if (!milena_size_add(path_bytes, 80u, &temporary_path_bytes) ||
        !milena_size_mul(4u, temporary_path_bytes, &path_reserve) ||
        map_used >= memory_budget_bytes ||
        path_bytes > memory_budget_bytes - map_used ||
        path_reserve > memory_budget_bytes - map_used - path_bytes) {
        milena_grouped_aggregate_close(grouped, NULL);
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para buffers de ordenamiento agrupado");
        return MILENA_ERR_ARGUMENT;
    }
    grouped->workspace_budget_bytes = memory_budget_bytes - map_used - path_bytes - path_reserve;
    if (grouped->workspace_budget_bytes < max_record * 2u) {
        milena_grouped_aggregate_close(grouped, NULL);
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para fusión externa agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    grouped->max_key_bytes = max_key_bytes;
    grouped->group_capacity = capacity;
    grouped->table_capacity = table_capacity;
    MilenaStatus status = milena_spill_store_create_exclusive(spill_path, spill_quota_bytes,
                                                   max_record, &grouped->spill, error);
    if (status != MILENA_OK) {
        milena_grouped_aggregate_close(grouped, NULL);
        return status;
    }
    grouped->spill_open = true;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_grouped_aggregate_open(
    const char *spill_path, size_t memory_budget_bytes, size_t max_key_bytes,
    size_t spill_quota_bytes, MilenaGroupedAggregate *grouped,
    MilenaError *error) {
    return milena_grouped_aggregate_open_with_max_runs(spill_path,
        memory_budget_bytes, max_key_bytes, spill_quota_bytes,
        MILENA_GROUPED_DEFAULT_MAX_RUNS, grouped, error);
}

static MilenaStatus grouped_aggregate_add_value(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    double float_value, int64_t integer_value, bool is_integer,
    MilenaError *error) {
    if (!grouped || !grouped->groups || grouped->finalized ||
        (!key && key_length != 0) || key_length > grouped->max_key_bytes) {
        group_error(error, MILENA_ERR_ARGUMENT, "Clave o estado inválido para agregar fila agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    if (grouped->failed) {
        group_error(error, grouped->failure_status,
                    "Agregación agrupada fallida; no se permiten más operaciones");
        return grouped->failure_status;
    }
    const unsigned char *bytes = key;
    bool found = false;
    size_t slot = find_group(grouped, bytes, key_length, &found);
    if (found) {
        MilenaAggregateState *state =
            &grouped->groups[grouped->table[slot] - 1u].aggregate;
        return is_integer ? milena_aggregate_state_add_int64(
            state, integer_value, error) : milena_aggregate_state_add(state, float_value, error);
    }
    if (grouped->group_count == grouped->group_capacity) {
        MilenaStatus status = flush_groups(grouped, error);
        if (status != MILENA_OK) return status;
        slot = find_group(grouped, bytes, key_length, &found);
    }
    if (slot >= grouped->table_capacity) {
        group_error(error, MILENA_ERR_INTERNAL, "Índice hash agrupado agotado");
        return MILENA_ERR_INTERNAL;
    }
    size_t index = grouped->group_count;
    MilenaAggregateState initial;
    milena_aggregate_state_init(&initial);
    MilenaStatus status = is_integer ?
        milena_aggregate_state_add_int64(&initial, integer_value, error) :
        milena_aggregate_state_add(&initial, float_value, error);
    if (status != MILENA_OK) return status;
    grouped->groups[index].key_length = key_length;
    grouped->groups[index].aggregate = initial;
    if (key_length) memcpy(entry_key(grouped, index), bytes, key_length);
    grouped->table[slot] = index + 1u;
    grouped->group_count++;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_grouped_aggregate_add(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    double value, MilenaError *error) {
    return grouped_aggregate_add_value(grouped, key, key_length, value, 0,
                                       false, error);
}

MilenaStatus milena_grouped_aggregate_add_int64(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    int64_t value, MilenaError *error) {
    return grouped_aggregate_add_value(grouped, key, key_length, 0.0, value,
                                       true, error);
}

static MilenaStatus grouped_aggregate_add_missing(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    bool invalid, MilenaError *error) {
    if (!grouped || !grouped->groups || grouped->finalized ||
        (!key && key_length != 0) || key_length > grouped->max_key_bytes) {
        group_error(error, MILENA_ERR_ARGUMENT,
                    "Clave inválida para fila agrupada sin valor");
        return MILENA_ERR_ARGUMENT;
    }
    if (grouped->failed) {
        group_error(error, grouped->failure_status,
                    "Agregación agrupada fallida; no se permiten más operaciones");
        return grouped->failure_status;
    }
    const unsigned char *bytes = key;
    bool found = false;
    size_t slot = find_group(grouped, bytes, key_length, &found);
    if (found) {
        MilenaAggregateState *state =
            &grouped->groups[grouped->table[slot] - 1u].aggregate;
        return invalid ? milena_aggregate_state_add_invalid(state, error) :
                         milena_aggregate_state_add_null(state, error);
    }
    if (grouped->group_count == grouped->group_capacity) {
        MilenaStatus status = flush_groups(grouped, error);
        if (status != MILENA_OK) return status;
        slot = find_group(grouped, bytes, key_length, &found);
        if (found) {
            MilenaAggregateState *state =
                &grouped->groups[grouped->table[slot] - 1u].aggregate;
            return invalid ? milena_aggregate_state_add_invalid(state, error) :
                             milena_aggregate_state_add_null(state, error);
        }
    }
    if (slot >= grouped->table_capacity) {
        group_error(error, MILENA_ERR_INTERNAL, "Índice hash agrupado agotado");
        return MILENA_ERR_INTERNAL;
    }
    size_t index = grouped->group_count;
    MilenaAggregateState initial;
    milena_aggregate_state_init(&initial);
    MilenaStatus status = invalid ?
        milena_aggregate_state_add_invalid(&initial, error) :
        milena_aggregate_state_add_null(&initial, error);
    if (status != MILENA_OK) return status;
    grouped->groups[index].key_length = key_length;
    grouped->groups[index].aggregate = initial;
    if (key_length) memcpy(entry_key(grouped, index), bytes, key_length);
    grouped->table[slot] = index + 1u;
    grouped->group_count++;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_grouped_aggregate_add_null(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    MilenaError *error) {
    return grouped_aggregate_add_missing(grouped, key, key_length, false, error);
}

MilenaStatus milena_grouped_aggregate_add_invalid(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    MilenaError *error) {
    return grouped_aggregate_add_missing(grouped, key, key_length, true, error);
}

static size_t record_length(const unsigned char *record) {
    return GROUP_RECORD_FIXED + (size_t)read_u32(record);
}

static int grouped_record_compare(const void *left, const void *right) {
    const unsigned char *a = left, *b = right;
    size_t a_length = read_u32(a), b_length = read_u32(b);
    return key_compare(a + 4, a_length, b + 4, b_length);
}

typedef struct {
    unsigned char *records;
    size_t stride;
    size_t capacity;
    size_t count;
    size_t max_key_bytes;
} GroupRunBatch;

/* A sorted run must contain at most one partial state per key. Map flushes can
 * serialize the same key in later chunks; qsort alone only orders those partials.
 * Coalescing here lets the bounded pairwise merge preserve that invariant and
 * prevents duplicate composite (group, metric) callbacks at finalization. */
static MilenaStatus coalesce_group_batch(GroupRunBatch *batch,
                                         MilenaError *error) {
    if (!batch || !batch->records || batch->stride < GROUP_RECORD_FIXED) {
        group_error(error, MILENA_ERR_ARGUMENT,
                    "Lote inválido para combinar claves agrupadas");
        return MILENA_ERR_ARGUMENT;
    }
    size_t unique_count = 0;
    for (size_t i = 0; i < batch->count; ++i) {
        unsigned char *source = batch->records + i * batch->stride;
        size_t source_length = record_length(source);
        if (source_length > batch->stride) {
            group_error(error, MILENA_ERR_DATA,
                        "Registro de lote agrupado fuera de límites");
            return MILENA_ERR_DATA;
        }
        if (unique_count == 0) {
            if (i != 0)
                memmove(batch->records, source, source_length);
            unique_count = 1;
            continue;
        }
        unsigned char *previous = batch->records +
            (unique_count - 1u) * batch->stride;
        int order = grouped_record_compare(previous, source);
        if (order > 0) {
            group_error(error, MILENA_ERR_INTERNAL,
                        "Lote agrupado dejó de estar ordenado");
            return MILENA_ERR_INTERNAL;
        }
        if (order == 0) {
            size_t key_length = read_u32(previous);
            MilenaAggregateState merged, incoming;
            MilenaStatus status = milena_aggregate_state_decode(
                previous + 4u + key_length, MILENA_AGGREGATE_WIRE_SIZE,
                &merged, error);
            if (status == MILENA_OK)
                status = milena_aggregate_state_decode(
                    source + 4u + key_length, MILENA_AGGREGATE_WIRE_SIZE,
                    &incoming, error);
            if (status == MILENA_OK)
                status = milena_aggregate_state_merge(&merged, &incoming, error);
            size_t written = 0;
            if (status == MILENA_OK)
                status = milena_aggregate_state_encode(&merged,
                    previous + 4u + key_length, MILENA_AGGREGATE_WIRE_SIZE,
                    &written, error);
            if (status != MILENA_OK) return status;
            continue;
        }
        if (unique_count != i)
            memmove(batch->records + unique_count * batch->stride,
                    source, source_length);
        unique_count++;
    }
    batch->count = unique_count;
    return MILENA_OK;
}

static bool path_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    (void)fclose(file);
    return true;
}

static char *sorted_run_path(const char *base, size_t pass, size_t run) {
    int n = snprintf(NULL, 0, "%s.group.p%zu.r%zu", base, pass, run);
    if (n < 0) return NULL;
    size_t capacity = (size_t)n + 1u;
    char *path = malloc(capacity);
    if (path) (void)snprintf(path, capacity, "%s.group.p%zu.r%zu", base, pass, run);
    return path;
}

static bool file_size(const char *path, size_t *bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = milena_file_seek64(file, 0, SEEK_END) == 0;
    int64_t end = ok ? milena_file_tell64(file) : -1;
    if (end < 0 || (uint64_t)end > SIZE_MAX) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok) *bytes = (size_t)end;
    return ok;
}

static void remove_run_set(const char *base, size_t pass, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        char *path = sorted_run_path(base, pass, i);
        if (path) { (void)remove(path); free(path); }
    }
}

static MilenaStatus write_group_run(const char *path, const GroupRunBatch *batch,
                                    size_t quota, size_t *bytes_written,
                                    MilenaError *error) {
    if (path_exists(path)) {
        group_error(error, MILENA_ERR_ARGUMENT, "Existe un run temporal agrupado previo");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaSpillStore output = {0};
    size_t max_record = GROUP_RECORD_FIXED + batch->max_key_bytes;
    MilenaStatus status = milena_spill_store_create_exclusive(path, quota, max_record, &output, error);
    bool output_created = status == MILENA_OK;
    for (size_t i = 0; status == MILENA_OK && i < batch->count; ++i) {
        const unsigned char *record = batch->records + i * batch->stride;
        status = milena_spill_store_append(&output, record, record_length(record), error);
    }
    if (output.file) {
        MilenaStatus close_status = milena_spill_store_close(&output, error);
        if (status == MILENA_OK) status = close_status;
    }
    if (status != MILENA_OK) { if (output_created) (void)remove(path); return status; }
    if (!file_size(path, bytes_written)) {
        if (output_created) (void)remove(path);
        group_error(error, MILENA_ERR_IO, "No se pudo medir run agrupado");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

static MilenaStatus read_group_record(MilenaSpillReader *reader,
                                      unsigned char *buffer, size_t capacity,
                                      bool *has_record, size_t max_key_bytes,
                                      MilenaError *error) {
    size_t length = 0;
    MilenaStatus status = milena_spill_reader_next(reader, buffer, capacity,
                                                    &length, has_record, error);
    if (status != MILENA_OK || !*has_record) return status;
    if (length < GROUP_RECORD_FIXED || read_u32(buffer) > max_key_bytes ||
        length != record_length(buffer)) {
        group_error(error, MILENA_ERR_DATA, "Registro ordenado de agregado agrupado inválido");
        return MILENA_ERR_DATA;
    }
    MilenaAggregateState decoded;
    return milena_aggregate_state_decode(buffer + 4 + read_u32(buffer),
        MILENA_AGGREGATE_WIRE_SIZE, &decoded, error);
}

static MilenaStatus merge_group_runs(const char *left_path, const char *right_path,
                                     const char *output_path, size_t max_key_bytes,
                                     size_t input_quota, size_t output_quota,
                                     size_t *bytes_written,
                                     unsigned char *left, unsigned char *right,
                                     size_t record_capacity, MilenaError *error) {
    if (path_exists(output_path)) {
        group_error(error, MILENA_ERR_ARGUMENT, "Existe una salida temporal agrupada previa");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaSpillReader a = {0}, b = {0};
    MilenaSpillStore output = {0};
    bool output_created = false;
    MilenaStatus status = milena_spill_reader_open(left_path, input_quota, record_capacity, &a, error);
    if (status == MILENA_OK) status = milena_spill_reader_open(right_path, input_quota, record_capacity, &b, error);
    if (status == MILENA_OK) {
        status = milena_spill_store_create_exclusive(output_path, output_quota,
            record_capacity, &output, error);
        output_created = status == MILENA_OK;
    }
    bool have_a = false, have_b = false;
    if (status == MILENA_OK) status = read_group_record(&a, left, record_capacity, &have_a, max_key_bytes, error);
    if (status == MILENA_OK) status = read_group_record(&b, right, record_capacity, &have_b, max_key_bytes, error);
    while (status == MILENA_OK && (have_a || have_b)) {
        unsigned char *chosen;
        bool advance_a = false, advance_b = false;
        if (!have_b || (have_a && grouped_record_compare(left, right) < 0)) {
            chosen = left; advance_a = true;
        } else if (!have_a || grouped_record_compare(left, right) > 0) {
            chosen = right; advance_b = true;
        } else {
            size_t key_length = read_u32(left);
            MilenaAggregateState merged, other;
            status = milena_aggregate_state_decode(left + 4 + key_length,
                MILENA_AGGREGATE_WIRE_SIZE, &merged, error);
            if (status == MILENA_OK) status = milena_aggregate_state_decode(
                right + 4 + key_length, MILENA_AGGREGATE_WIRE_SIZE, &other, error);
            if (status == MILENA_OK) status = milena_aggregate_state_merge(&merged, &other, error);
            size_t written = 0;
            if (status == MILENA_OK) status = milena_aggregate_state_encode(&merged,
                left + 4 + key_length, MILENA_AGGREGATE_WIRE_SIZE, &written, error);
            if (status != MILENA_OK) break;
            chosen = left; advance_a = advance_b = true;
        }
        status = milena_spill_store_append(&output, chosen, record_length(chosen), error);
        if (status != MILENA_OK) break;
        if (advance_a) status = read_group_record(&a, left, record_capacity,
                                                  &have_a, max_key_bytes, error);
        if (status == MILENA_OK && advance_b) status = read_group_record(&b, right,
                                                  record_capacity, &have_b, max_key_bytes, error);
    }
    if (a.file) { MilenaStatus close_status = milena_spill_reader_close(&a, error); if (status == MILENA_OK) status = close_status; }
    if (b.file) { MilenaStatus close_status = milena_spill_reader_close(&b, error); if (status == MILENA_OK) status = close_status; }
    if (output.file) { MilenaStatus close_status = milena_spill_store_close(&output, error); if (status == MILENA_OK) status = close_status; }
    if (status != MILENA_OK) { if (output_created) (void)remove(output_path); return status; }
    if (!file_size(output_path, bytes_written)) {
        if (output_created) (void)remove(output_path);
        group_error(error, MILENA_ERR_IO, "No se pudo medir la fusión agrupada");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

MilenaStatus milena_grouped_aggregate_finalize(
    MilenaGroupedAggregate *grouped, MilenaGroupedAggregateVisitFn visitor,
    void *context, size_t *groups_emitted, MilenaError *error) {
    if (!grouped || !grouped->groups || grouped->finalized || !visitor) {
        group_error(error, MILENA_ERR_ARGUMENT, "Agregación agrupada no finalizable");
        return MILENA_ERR_ARGUMENT;
    }
    if (grouped->failed) {
        group_error(error, grouped->failure_status,
                    "Agregación agrupada fallida; no se puede reintentar finalización");
        return grouped->failure_status;
    }
    if (groups_emitted) *groups_emitted = 0;
    MilenaStatus status = flush_groups(grouped, error);
    if (status != MILENA_OK) return status;
    status = milena_spill_store_close(&grouped->spill, error);
    grouped->spill_open = false;
    if (status != MILENA_OK) {
        grouped->failed = true;
        grouped->failure_status = status;
        return status;
    }
    grouped->finalized = true;

    size_t max_record = GROUP_RECORD_FIXED + grouped->max_key_bytes;
    size_t workspace = grouped->workspace_budget_bytes;
    size_t capacity = workspace / max_record;
    if (capacity == 0 || capacity > SIZE_MAX / max_record) {
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para runs agrupados");
        return MILENA_ERR_ARGUMENT;
    }
    size_t batch_bytes = capacity * max_record;
    unsigned char *batch_memory = malloc(batch_bytes);
    if (!batch_memory) {
        group_error(error, MILENA_ERR_MEMORY, "Sin memoria para ordenar runs agrupados");
        return MILENA_ERR_MEMORY;
    }
    size_t temporary_limit;
    if (!milena_size_mul(grouped->spill.quota_bytes, 2u, &temporary_limit)) {
        free(batch_memory);
        group_error(error, MILENA_ERR_OVERFLOW, "Cuota temporal agrupada desbordada");
        return MILENA_ERR_OVERFLOW;
    }
    GroupRunBatch batch = {batch_memory, max_record, capacity, 0, grouped->max_key_bytes};
    size_t slots = grouped->workspace_budget_bytes / max_record;
    if (slots < 3u) {
        free(batch_memory);
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para fusión externa agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    /* One slot is reserved for the streaming source reader; the remaining
     * slots form a bounded sort batch and are reused as the merge buffers. */
    capacity = slots - 1u;
    batch.capacity = capacity;
    size_t run_count = 0, run_bytes = 0;
    unsigned char *source_record = batch_memory + capacity * max_record;
    MilenaSpillReader source = {0};
    status = milena_spill_reader_open(grouped->spill_path,
        grouped->spill.quota_bytes, max_record, &source, error);
    bool has_record = false;
    while (status == MILENA_OK) {
        size_t length = 0;
        status = milena_spill_reader_next(&source, source_record, max_record,
                                           &length, &has_record, error);
        if (status != MILENA_OK || !has_record) break;
        if (length < GROUP_RECORD_FIXED || read_u32(source_record) > grouped->max_key_bytes ||
            length != record_length(source_record)) {
            group_error(error, MILENA_ERR_DATA, "Registro de spill agrupado inválido");
            status = MILENA_ERR_DATA;
            break;
        }
        MilenaAggregateState decoded;
        status = milena_aggregate_state_decode(source_record + 4 + read_u32(source_record),
            MILENA_AGGREGATE_WIRE_SIZE, &decoded, error);
        if (status != MILENA_OK) break;
        if (batch.count == batch.capacity) {
            if (run_count >= grouped->max_runs) {
                group_error(error, MILENA_ERR_OVERFLOW,
                            "Se excedió el máximo configurado de runs agrupados");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            qsort(batch.records, batch.count, batch.stride, grouped_record_compare);
            status = coalesce_group_batch(&batch, error);
            if (status != MILENA_OK) break;
            char *path = sorted_run_path(grouped->spill_path, 0, run_count);
            if (!path) { group_error(error, MILENA_ERR_MEMORY, "No se pudo crear la ruta de run agrupado"); status = MILENA_ERR_MEMORY; break; }
            size_t remaining = temporary_limit - run_bytes;
            size_t quota = grouped->spill.quota_bytes < remaining ? grouped->spill.quota_bytes : remaining;
            size_t written = 0;
            status = write_group_run(path, &batch, quota, &written, error);
            free(path);
            if (status != MILENA_OK) break;
            if (written > remaining || run_count == SIZE_MAX ||
                !milena_size_add(run_bytes, written, &run_bytes)) {
                group_error(error, MILENA_ERR_OVERFLOW, "Cuota temporal de runs agrupados agotada");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            run_count++;
            grouped->sorted_runs = run_count;
            batch.count = 0;
        }
        memcpy(batch.records + batch.count * batch.stride, source_record, length);
        batch.count++;
    }
    if (source.file) {
        MilenaStatus close_status = milena_spill_reader_close(&source, error);
        if (status == MILENA_OK) status = close_status;
    }
    if (status == MILENA_OK && batch.count != 0) {
        if (run_count >= grouped->max_runs) {
            group_error(error, MILENA_ERR_OVERFLOW,
                        "Se excedió el máximo configurado de runs agrupados");
            status = MILENA_ERR_OVERFLOW;
        }
        if (status == MILENA_OK) {
            qsort(batch.records, batch.count, batch.stride, grouped_record_compare);
            status = coalesce_group_batch(&batch, error);
        }
        if (status == MILENA_OK) {
            char *path = sorted_run_path(grouped->spill_path, 0, run_count);
            if (!path) {
                group_error(error, MILENA_ERR_MEMORY,
                            "No se pudo crear la ruta de run agrupado");
                status = MILENA_ERR_MEMORY;
            } else {
                size_t remaining = temporary_limit - run_bytes;
                size_t quota = grouped->spill.quota_bytes < remaining ?
                    grouped->spill.quota_bytes : remaining;
                size_t written = 0;
                status = write_group_run(path, &batch, quota, &written, error);
                free(path);
                if (status == MILENA_OK && (written > remaining ||
                    run_count == SIZE_MAX ||
                    !milena_size_add(run_bytes, written, &run_bytes))) {
                    group_error(error, MILENA_ERR_OVERFLOW,
                                "Cuota temporal de runs agrupados agotada");
                    status = MILENA_ERR_OVERFLOW;
                }
                if (status == MILENA_OK) {
                    run_count++;
                    grouped->sorted_runs = run_count;
                }
            }
        }
    }
    free(batch_memory);
    if (status != MILENA_OK) {
        if (error && error->code == MILENA_OK)
            group_error(error, status, "Error sin detalle al leer spill y crear runs ordenados");
        remove_run_set(grouped->spill_path, 0, run_count);
        return status;
    }
    if (run_count == 0) { if (error) milena_error_clear(error); return MILENA_OK; }

    size_t pass = 0;
    while (run_count > 1u && status == MILENA_OK) {
        if (pass == SIZE_MAX) { group_error(error, MILENA_ERR_OVERFLOW, "Demasiadas pasadas de fusión agrupada"); status = MILENA_ERR_OVERFLOW; break; }
        size_t next_count = run_count / 2u + run_count % 2u;
        size_t next_bytes = run_bytes;
        size_t created = 0;
        size_t record_capacity = max_record;
        unsigned char *merge_buffers = malloc(record_capacity * 2u);
        if (!merge_buffers) { group_error(error, MILENA_ERR_MEMORY, "Sin memoria para fusionar runs agrupados"); status = MILENA_ERR_MEMORY; break; }
        for (size_t i = 0; i < run_count; i += 2u) {
            char *left = sorted_run_path(grouped->spill_path, pass, i);
            char *right = i + 1u < run_count ? sorted_run_path(grouped->spill_path, pass, i + 1u) : NULL;
            char *out = sorted_run_path(grouped->spill_path, pass + 1u, i / 2u);
            if (!left || !out || (i + 1u < run_count && !right)) {
                free(left); free(right); free(out);
                group_error(error, MILENA_ERR_MEMORY, "No se pudo generar ruta de fusión agrupada"); status = MILENA_ERR_MEMORY; break;
            }
            if (!right) {
                if (path_exists(out) || rename(left, out) != 0) {
                    group_error(error, MILENA_ERR_IO, "No se pudo trasladar run agrupado impar"); status = MILENA_ERR_IO;
                }
                free(left); free(out);
                if (status != MILENA_OK) break;
                created++;
                continue;
            }
            size_t left_bytes = 0, right_bytes = 0, output_bytes = 0;
            if (!file_size(left, &left_bytes) || !file_size(right, &right_bytes)) {
                group_error(error, MILENA_ERR_IO, "No se pudo medir run de entrada agrupado"); status = MILENA_ERR_IO;
            } else {
                size_t remaining = temporary_limit - next_bytes;
                size_t output_quota = grouped->spill.quota_bytes < remaining ? grouped->spill.quota_bytes : remaining;
                status = merge_group_runs(left, right, out, grouped->max_key_bytes,
                    grouped->spill.quota_bytes, output_quota, &output_bytes,
                    merge_buffers, merge_buffers + record_capacity, record_capacity, error);
            }
            if (status == MILENA_OK && (output_bytes > temporary_limit - next_bytes ||
                !milena_size_add(next_bytes, output_bytes, &next_bytes))) {
                group_error(error, MILENA_ERR_OVERFLOW, "Cuota temporal de fusión agrupada agotada"); status = MILENA_ERR_OVERFLOW;
            }
            if (status == MILENA_OK) {
                (void)remove(left); (void)remove(right);
                next_bytes -= left_bytes + right_bytes;
                created++;
            }
            free(left); free(right); free(out);
            if (status != MILENA_OK) break;
        }
        free(merge_buffers);
        if (status != MILENA_OK) { remove_run_set(grouped->spill_path, pass + 1u, created); break; }
        run_count = next_count;
        run_bytes = next_bytes;
        pass++;
    }
    if (status != MILENA_OK) {
        if (error && error->code == MILENA_OK)
            group_error(error, status, "Error sin detalle al fusionar runs agrupados");
        remove_run_set(grouped->spill_path, pass, run_count);
        return status;
    }

    char *final_path = sorted_run_path(grouped->spill_path, pass, 0);
    if (!final_path) {
        remove_run_set(grouped->spill_path, pass, 1u);
        group_error(error, MILENA_ERR_MEMORY, "No se pudo abrir la salida agrupada");
        return MILENA_ERR_MEMORY;
    }
    unsigned char *record = malloc(max_record);
    if (!record) { free(final_path); group_error(error, MILENA_ERR_MEMORY, "Sin memoria para leer salida agrupada"); remove_run_set(grouped->spill_path, pass, 1u); return MILENA_ERR_MEMORY; }
    MilenaSpillReader final_reader = {0};
    status = milena_spill_reader_open(final_path, grouped->spill.quota_bytes,
                                      max_record, &final_reader, error);
    size_t emitted = 0;
    while (status == MILENA_OK) {
        size_t length = 0;
        status = milena_spill_reader_next(&final_reader, record, max_record,
                                           &length, &has_record, error);
        if (status != MILENA_OK || !has_record) break;
        if (length < GROUP_RECORD_FIXED || read_u32(record) > grouped->max_key_bytes ||
            length != record_length(record)) {
            group_error(error, MILENA_ERR_DATA, "Registro final agrupado inválido"); status = MILENA_ERR_DATA; break;
        }
        size_t key_length = read_u32(record);
        MilenaAggregateState state;
        MilenaAggregateResult result;
        status = milena_aggregate_state_decode(record + 4 + key_length,
            MILENA_AGGREGATE_WIRE_SIZE, &state, error);
        if (status == MILENA_OK) status = milena_aggregate_state_finalize(&state, &result, error);
        if (status != MILENA_OK) break;
        MilenaGroupedAggregateResult output = {key_length, record + 4, result};
        status = visitor(&output, context, error);
        if (status != MILENA_OK) break;
        if (emitted == SIZE_MAX) { group_error(error, MILENA_ERR_OVERFLOW, "Cantidad de grupos emitidos desbordada"); status = MILENA_ERR_OVERFLOW; break; }
        emitted++;
    }
    if (final_reader.file) {
        /* Cleanup must not erase a read/decode/visitor diagnostic. */
        MilenaStatus close_status = milena_spill_reader_close(&final_reader,
            status == MILENA_OK ? error : NULL);
        if (status == MILENA_OK) status = close_status;
    }
    free(record);
    free(final_path);
    remove_run_set(grouped->spill_path, pass, 1u);
    if (groups_emitted) *groups_emitted = emitted;
    if (status == MILENA_OK && error) milena_error_clear(error);
    else if (status != MILENA_OK && error && error->code == MILENA_OK)
        group_error(error, status, "Error sin detalle al emitir resultados ordenados");
    return status;
}

MilenaStatus milena_grouped_aggregate_close(MilenaGroupedAggregate *grouped,
                                            MilenaError *error) {
    if (!grouped) return MILENA_OK;
    MilenaStatus status = MILENA_OK;
    if (grouped->spill_open) {
        status = milena_spill_store_close(&grouped->spill, error);
        grouped->spill_open = false;
    }
    free(grouped->groups);
    free(grouped->key_arena);
    free(grouped->table);
    free(grouped->spill_path);
    grouped->groups = NULL;
    grouped->key_arena = NULL;
    grouped->table = NULL;
    grouped->spill_path = NULL;
    return status;
}
