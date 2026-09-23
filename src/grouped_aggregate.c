#include "grouped_aggregate.h"

#define GROUP_RECORD_FIXED (4u + MILENA_AGGREGATE_WIRE_SIZE)

typedef struct {
    unsigned char *candidate;
    size_t candidate_length;
    size_t max_key_bytes;
    const unsigned char *cursor;
    size_t cursor_length;
    bool has_cursor;
} CandidateScan;

typedef struct {
    const unsigned char *key;
    size_t key_length;
    size_t max_key_bytes;
    MilenaAggregateState aggregate;
    bool found;
} StateScan;

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
    if (grouped->group_count == 0) return MILENA_OK;
    size_t max_record;
    if (!milena_size_add(GROUP_RECORD_FIXED, grouped->max_key_bytes, &max_record)) {
        group_error(error, MILENA_ERR_OVERFLOW, "Tamaño de registro agrupado desbordado");
        return MILENA_ERR_OVERFLOW;
    }
    unsigned char *record = malloc(max_record);
    if (!record) {
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
    return status;
}

MilenaStatus milena_grouped_aggregate_open(
    const char *spill_path, size_t memory_budget_bytes, size_t max_key_bytes,
    size_t spill_quota_bytes, MilenaGroupedAggregate *grouped,
    MilenaError *error) {
    if (!spill_path || !grouped || max_key_bytes == 0 ||
        max_key_bytes > UINT32_MAX || memory_budget_bytes == 0) {
        group_error(error, MILENA_ERR_ARGUMENT, "Configuración inválida para agregación agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    FILE *existing = fopen(spill_path, "rb");
    if (existing) {
        fclose(existing);
        group_error(error, MILENA_ERR_ARGUMENT, "La ruta scratch debe ser nueva para esta agregación");
        return MILENA_ERR_ARGUMENT;
    }
    if (errno != ENOENT) {
        group_error(error, MILENA_ERR_IO, "No se pudo validar la ruta scratch agrupada");
        return MILENA_ERR_IO;
    }
    memset(grouped, 0, sizeof(*grouped));
    size_t max_record;
    size_t key_scratch;
    size_t scratch_budget;
    if (!milena_size_add(GROUP_RECORD_FIXED, max_key_bytes, &max_record) ||
        !milena_size_mul(2u, max_key_bytes, &key_scratch) ||
        !milena_size_add(max_record, key_scratch, &scratch_budget) ||
        !milena_size_add(scratch_budget, 1u, &scratch_budget) ||
        scratch_budget >= memory_budget_bytes) {
        group_error(error, MILENA_ERR_ARGUMENT, "Presupuesto insuficiente para buffers acotados de agregación agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    size_t map_budget = memory_budget_bytes - scratch_budget;
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
    grouped->max_key_bytes = max_key_bytes;
    grouped->group_capacity = capacity;
    grouped->table_capacity = table_capacity;
    MilenaStatus status = milena_spill_store_open(spill_path, spill_quota_bytes,
                                                   max_record, &grouped->spill, error);
    if (status != MILENA_OK) {
        milena_grouped_aggregate_close(grouped, NULL);
        return status;
    }
    grouped->spill_open = true;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_grouped_aggregate_add(
    MilenaGroupedAggregate *grouped, const void *key, size_t key_length,
    double value, MilenaError *error) {
    if (!grouped || !grouped->groups || grouped->finalized ||
        (!key && key_length != 0) || key_length > grouped->max_key_bytes) {
        group_error(error, MILENA_ERR_ARGUMENT, "Clave o estado inválido para agregar fila agrupada");
        return MILENA_ERR_ARGUMENT;
    }
    const unsigned char *bytes = key;
    bool found = false;
    size_t slot = find_group(grouped, bytes, key_length, &found);
    if (found) {
        MilenaStatus status = milena_aggregate_state_add(
            &grouped->groups[grouped->table[slot] - 1u].aggregate, value, error);
        return status;
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
    MilenaStatus status = milena_aggregate_state_add(&initial, value, error);
    if (status != MILENA_OK) return status;
    grouped->groups[index].key_length = key_length;
    grouped->groups[index].aggregate = initial;
    if (key_length) memcpy(entry_key(grouped, index), bytes, key_length);
    grouped->table[slot] = index + 1u;
    grouped->group_count++;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

static MilenaStatus candidate_visit(const void *payload, size_t length,
                                    size_t record_index, void *context,
                                    MilenaError *error) {
    (void)record_index;
    CandidateScan *scan = context;
    const unsigned char *record = payload;
    if (length < GROUP_RECORD_FIXED) {
        group_error(error, MILENA_ERR_DATA, "Registro de agregación agrupada truncado");
        return MILENA_ERR_DATA;
    }
    size_t key_length = read_u32(record);
    if (key_length > scan->max_key_bytes || length != 4 + key_length + MILENA_AGGREGATE_WIRE_SIZE) {
        group_error(error, MILENA_ERR_DATA, "Longitud de clave agrupada inválida");
        return MILENA_ERR_DATA;
    }
    MilenaAggregateState ignored;
    MilenaStatus status = milena_aggregate_state_decode(record + 4 + key_length,
        MILENA_AGGREGATE_WIRE_SIZE, &ignored, error);
    if (status != MILENA_OK) return status;
    const unsigned char *key = record + 4;
    if (scan->has_cursor && key_compare(key, key_length,
            scan->cursor, scan->cursor_length) <= 0) return MILENA_OK;
    /* The final byte is a sentinel, including for a valid empty key. */
    bool have_candidate = scan->candidate[scan->max_key_bytes] != 0;
    if (!have_candidate || key_compare(key, key_length, scan->candidate,
                                       scan->candidate_length) < 0) {
        if (key_length) memcpy(scan->candidate, key, key_length);
        scan->candidate_length = key_length;
        scan->candidate[scan->max_key_bytes] = 1;
    }
    return MILENA_OK;
}

static MilenaStatus state_visit(const void *payload, size_t length,
                                size_t record_index, void *context,
                                MilenaError *error) {
    (void)record_index;
    StateScan *scan = context;
    const unsigned char *record = payload;
    if (length < GROUP_RECORD_FIXED) {
        group_error(error, MILENA_ERR_DATA, "Registro de agregación agrupada truncado");
        return MILENA_ERR_DATA;
    }
    size_t key_length = read_u32(record);
    if (key_length > scan->max_key_bytes ||
        length != 4 + key_length + MILENA_AGGREGATE_WIRE_SIZE) {
        group_error(error, MILENA_ERR_DATA, "Longitud de agregado agrupado inválida");
        return MILENA_ERR_DATA;
    }
    if (key_compare(record + 4, key_length, scan->key, scan->key_length) != 0) return MILENA_OK;
    MilenaAggregateState partial;
    MilenaStatus status = milena_aggregate_state_decode(record + 4 + key_length,
        MILENA_AGGREGATE_WIRE_SIZE, &partial, error);
    if (status != MILENA_OK) return status;
    status = milena_aggregate_state_merge(&scan->aggregate, &partial, error);
    if (status == MILENA_OK) scan->found = true;
    return status;
}

MilenaStatus milena_grouped_aggregate_finalize(
    MilenaGroupedAggregate *grouped, MilenaGroupedAggregateVisitFn visitor,
    void *context, size_t *groups_emitted, MilenaError *error) {
    if (!grouped || !grouped->groups || grouped->finalized || !visitor) {
        group_error(error, MILENA_ERR_ARGUMENT, "Agregación agrupada no finalizable");
        return MILENA_ERR_ARGUMENT;
    }
    if (groups_emitted) *groups_emitted = 0;
    MilenaStatus status = flush_groups(grouped, error);
    if (status != MILENA_OK) return status;
    status = milena_spill_store_close(&grouped->spill, error);
    grouped->spill_open = false;
    if (status != MILENA_OK) return status;
    grouped->finalized = true;
    unsigned char *candidate = calloc(grouped->max_key_bytes + 1u, 1u);
    unsigned char *cursor = malloc(grouped->max_key_bytes ? grouped->max_key_bytes : 1u);
    if (!candidate || !cursor) {
        free(candidate); free(cursor);
        group_error(error, MILENA_ERR_MEMORY, "Sin memoria para finalizar claves agrupadas");
        return MILENA_ERR_MEMORY;
    }
    bool has_cursor = false;
    size_t cursor_length = 0;
    size_t emitted = 0;
    for (;;) {
        memset(candidate, 0, grouped->max_key_bytes + 1u);
        CandidateScan find = {candidate, 0, grouped->max_key_bytes,
                              cursor, cursor_length, has_cursor};
        status = milena_spill_store_visit(grouped->spill_path,
            grouped->spill.quota_bytes, grouped->spill.max_record_bytes,
            candidate_visit, &find, error);
        if (status != MILENA_OK) break;
        if (candidate[grouped->max_key_bytes] == 0) break;
        StateScan scan;
        memset(&scan, 0, sizeof(scan));
        scan.key = candidate;
        scan.key_length = find.candidate_length;
        scan.max_key_bytes = grouped->max_key_bytes;
        milena_aggregate_state_init(&scan.aggregate);
        status = milena_spill_store_visit(grouped->spill_path,
            grouped->spill.quota_bytes, grouped->spill.max_record_bytes,
            state_visit, &scan, error);
        if (status != MILENA_OK) break;
        if (!scan.found) {
            group_error(error, MILENA_ERR_DATA, "No se pudo reducir un grupo serializado");
            status = MILENA_ERR_DATA;
            break;
        }
        MilenaGroupedAggregateResult result;
        result.key_length = find.candidate_length;
        result.key = candidate;
        status = milena_aggregate_state_finalize(&scan.aggregate, &result.aggregate, error);
        if (status != MILENA_OK) break;
        status = visitor(&result, context, error);
        if (status != MILENA_OK) break;
        if (find.candidate_length) memcpy(cursor, candidate, find.candidate_length);
        cursor_length = find.candidate_length;
        has_cursor = true;
        if (emitted == SIZE_MAX) {
            group_error(error, MILENA_ERR_OVERFLOW, "Cantidad de grupos emitidos desbordada");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        emitted++;
    }
    free(candidate);
    free(cursor);
    if (groups_emitted) *groups_emitted = emitted;
    if (status == MILENA_OK && error) milena_error_clear(error);
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
