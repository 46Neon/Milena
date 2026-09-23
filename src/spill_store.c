#include "spill_store.h"

#include <stdint.h>

static const unsigned char SPILL_MAGIC[4] = {'M', 'L', 'S', '1'};

static void spill_error(MilenaError *error, MilenaStatus status,
                        const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static uint32_t spill_checksum(const unsigned char *data, size_t length) {
    uint32_t value = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        value ^= data[i];
        value *= UINT32_C(16777619);
    }
    return value;
}

static void put_u32(unsigned char *out, uint32_t value) {
    for (size_t i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static void put_u64(unsigned char *out, uint64_t value) {
    for (size_t i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static uint32_t get_u32(const unsigned char *in) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++) value |= ((uint32_t)in[i]) << (i * 8);
    return value;
}

static uint64_t get_u64(const unsigned char *in) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value |= ((uint64_t)in[i]) << (i * 8);
    return value;
}

static bool read_exact(FILE *file, void *data, size_t length) {
    return length == 0 || fread(data, 1, length, file) == length;
}

static MilenaStatus scan_store(FILE *file, size_t quota, size_t max_record,
                               size_t *valid_bytes, size_t *records,
                               bool *bad_tail, MilenaError *error) {
    unsigned char header[MILENA_SPILL_HEADER_SIZE];
    size_t offset = 0;
    *valid_bytes = 0;
    *records = 0;
    *bad_tail = false;
    for (;;) {
        size_t header_start = offset;
        size_t header_bytes = fread(header, 1, sizeof(header), file);
        if (header_bytes == 0) {
            if (ferror(file)) {
                spill_error(error, MILENA_ERR_IO, "No se pudo leer el spill");
                return MILENA_ERR_IO;
            }
            break;
        }
        if (header_bytes != sizeof(header)) {
            *bad_tail = true;
            break;
        }
        if (memcmp(header, SPILL_MAGIC, 4) != 0 ||
            get_u32(header + 4) != MILENA_SPILL_VERSION) {
            *bad_tail = true;
            break;
        }
        uint64_t raw_length = get_u64(header + 8);
        if (raw_length > SIZE_MAX || (size_t)raw_length > max_record) {
            *bad_tail = true;
            break;
        }
        size_t length = (size_t)raw_length;
        size_t record_bytes = 0;
        if (!milena_size_add(MILENA_SPILL_HEADER_SIZE, length, &record_bytes) ||
            !milena_size_add(record_bytes, MILENA_SPILL_TRAILER_SIZE, &record_bytes) ||
            !milena_size_add(offset, record_bytes, &offset) || offset > quota) {
            *bad_tail = true;
            break;
        }
        unsigned char *payload = NULL;
        if (length > 0) {
            payload = (unsigned char *)malloc(length);
            if (!payload) {
                spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente al recuperar spill");
                return MILENA_ERR_MEMORY;
            }
        }
        unsigned char trailer[MILENA_SPILL_TRAILER_SIZE];
        bool complete = read_exact(file, payload, length) &&
                        read_exact(file, trailer, sizeof(trailer));
        if (!complete || spill_checksum(payload, length) != get_u32(trailer)) {
            free(payload);
            *bad_tail = true;
            break;
        }
        free(payload);
        *valid_bytes = offset;
        (*records)++;
        (void)header_start;
    }
    if (*valid_bytes > quota) {
        spill_error(error, MILENA_ERR_OVERFLOW, "El spill supera la cuota configurada");
        return MILENA_ERR_OVERFLOW;
    }
    return MILENA_OK;
}

MilenaStatus milena_spill_store_recover(const char *path, size_t quota_bytes,
                                        size_t max_record_bytes,
                                        MilenaSpillRecovery *recovery,
                                        MilenaError *error) {
    if (!path || !path[0] || !recovery || max_record_bytes == 0 ||
        quota_bytes < MILENA_SPILL_HEADER_SIZE + MILENA_SPILL_TRAILER_SIZE) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para recuperar spill");
        return MILENA_ERR_ARGUMENT;
    }
    memset(recovery, 0, sizeof(*recovery));
    FILE *input = fopen(path, "rb");
    if (!input) {
        if (errno == ENOENT) { if (error) milena_error_clear(error); return MILENA_OK; }
        spill_error(error, MILENA_ERR_IO, "No se pudo abrir el spill para recuperar");
        return MILENA_ERR_IO;
    }
    if (fseek(input, 0, SEEK_END) != 0) { fclose(input); spill_error(error, MILENA_ERR_IO, "No se pudo medir el spill"); return MILENA_ERR_IO; }
    long end = ftell(input);
    if (end < 0) { fclose(input); spill_error(error, MILENA_ERR_IO, "No se pudo medir el spill"); return MILENA_ERR_IO; }
    size_t total = (size_t)end;
    rewind(input);
    size_t valid = 0, records = 0;
    bool bad_tail = false;
    MilenaStatus status = scan_store(input, quota_bytes, max_record_bytes,
                                     &valid, &records, &bad_tail, error);
    fclose(input);
    if (status != MILENA_OK) return status;
    recovery->records_recovered = records;
    recovery->bytes_recovered = valid;
    recovery->bytes_discarded = total >= valid ? total - valid : 0;
    recovery->truncated_tail = bad_tail && recovery->bytes_discarded > 0;
    if (recovery->truncated_tail) {
        char *tmp_path = (char *)malloc(strlen(path) + 10);
        if (!tmp_path) { spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente al truncar spill"); return MILENA_ERR_MEMORY; }
        (void)snprintf(tmp_path, strlen(path) + 10, "%s.recover", path);
        input = fopen(path, "rb");
        FILE *output = fopen(tmp_path, "wb");
        if (!input || !output) { if (input) fclose(input); if (output) fclose(output); free(tmp_path); spill_error(error, MILENA_ERR_IO, "No se pudo crear spill recuperado"); return MILENA_ERR_IO; }
        unsigned char buffer[8192]; size_t left = valid;
        while (left > 0) { size_t want = left < sizeof(buffer) ? left : sizeof(buffer); if (fread(buffer, 1, want, input) != want || fwrite(buffer, 1, want, output) != want) { fclose(input); fclose(output); remove(tmp_path); free(tmp_path); spill_error(error, MILENA_ERR_IO, "No se pudo copiar el spill recuperado"); return MILENA_ERR_IO; } left -= want; }
        bool ok = fclose(input) == 0 && fclose(output) == 0 && remove(path) == 0 && rename(tmp_path, path) == 0;
        free(tmp_path);
        if (!ok) { spill_error(error, MILENA_ERR_IO, "No se pudo activar el spill recuperado"); return MILENA_ERR_IO; }
    }
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_store_open(const char *path, size_t quota_bytes,
                                     size_t max_record_bytes,
                                     MilenaSpillStore *store,
                                     MilenaError *error) {
    if (!path || !path[0] || !store || max_record_bytes == 0 ||
        quota_bytes < MILENA_SPILL_HEADER_SIZE + MILENA_SPILL_TRAILER_SIZE) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para abrir spill");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaSpillRecovery recovery;
    MilenaStatus status = milena_spill_store_recover(path, quota_bytes, max_record_bytes, &recovery, error);
    if (status != MILENA_OK) return status;
    FILE *file = fopen(path, "ab");
    if (!file) { spill_error(error, MILENA_ERR_IO, "No se pudo abrir el spill para escritura"); return MILENA_ERR_IO; }
    store->path = milena_strdup(path);
    if (!store->path) { fclose(file); spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente para abrir spill"); return MILENA_ERR_MEMORY; }
    store->file = file; store->quota_bytes = quota_bytes; store->max_record_bytes = max_record_bytes;
    store->bytes_used = recovery.bytes_recovered; store->record_count = recovery.records_recovered;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_store_append(MilenaSpillStore *store, const void *data,
                                       size_t length, MilenaError *error) {
    if (!store || !store->file || (length > 0 && !data) || length > store->max_record_bytes) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Registro inválido para spill"); return MILENA_ERR_ARGUMENT;
    }
    size_t total = 0;
    if (!milena_size_add(MILENA_SPILL_HEADER_SIZE, length, &total) ||
        !milena_size_add(total, MILENA_SPILL_TRAILER_SIZE, &total) ||
        total > store->quota_bytes - store->bytes_used) {
        spill_error(error, MILENA_ERR_OVERFLOW, "La cuota de spill fue agotada"); return MILENA_ERR_OVERFLOW;
    }
    unsigned char header[MILENA_SPILL_HEADER_SIZE], trailer[MILENA_SPILL_TRAILER_SIZE];
    memcpy(header, SPILL_MAGIC, 4); put_u32(header + 4, MILENA_SPILL_VERSION); put_u64(header + 8, (uint64_t)length);
    put_u32(trailer, spill_checksum((const unsigned char *)data, length));
    if (fwrite(header, 1, sizeof(header), store->file) != sizeof(header) ||
        (length > 0 && fwrite(data, 1, length, store->file) != length) ||
        fwrite(trailer, 1, sizeof(trailer), store->file) != sizeof(trailer) || fflush(store->file) != 0) {
        spill_error(error, MILENA_ERR_IO, "No se pudo escribir el registro spill"); return MILENA_ERR_IO;
    }
    store->bytes_used += total; store->record_count++;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_store_close(MilenaSpillStore *store, MilenaError *error) {
    if (!store || !store->file) { spill_error(error, MILENA_ERR_ARGUMENT, "Spill no abierto"); return MILENA_ERR_ARGUMENT; }
    MilenaStatus status = fclose(store->file) == 0 ? MILENA_OK : MILENA_ERR_IO;
    free(store->path); store->path = NULL; store->file = NULL;
    if (status != MILENA_OK) spill_error(error, status, "No se pudo cerrar el spill"); else if (error) milena_error_clear(error);
    return status;
}
