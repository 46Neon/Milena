#include "spill.h"
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#define unlink _unlink
#else
#include <unistd.h>
#endif
#include <sys/stat.h>

#define SPILL_MAGIC "MLSP"
#define SPILL_DEFAULT_BYTES (64u * 1024u * 1024u)

static uint64_t hash_bytes(uint64_t hash, const unsigned char *data, size_t size) {
    while (size-- != 0u) hash = (hash ^ *data++) * UINT64_C(1099511628211);
    return hash;
}

MilenaSpillPolicy milena_spill_policy_default(void) {
    MilenaSpillPolicy policy = {0, SPILL_DEFAULT_BYTES, 16};
    return policy;
}

static MilenaStatus spill_error(MilenaError *error, MilenaStatus status, const char *message) {
    milena_error_set(error, status, 0, 0, 0, message);
    return status;
}

MilenaStatus milena_spill_write(const MilenaSpillPolicy *requested, const char *prefix,
                                const char *const *keys, const double *values, size_t count,
                                MilenaSpillMetadata *metadata, MilenaError *error) {
    MilenaSpillPolicy defaults = milena_spill_policy_default();
    MilenaSpillPolicy policy = requested ? *requested : defaults;
    if (!policy.directory || !prefix || !keys || !values || !metadata || policy.max_bytes == 0u)
        return spill_error(error, MILENA_ERR_ARGUMENT, "Contrato de spill inválido");
    if (policy.partitions == 0u)
        return spill_error(error, MILENA_ERR_ARGUMENT, "El número de particiones debe ser positivo");
    memset(metadata, 0, sizeof(*metadata));
    int path_length = snprintf(metadata->path, sizeof(metadata->path), "%s/%s-%ld.bin",
                               policy.directory, prefix, (long)getpid());
    if (path_length < 0 || (size_t)path_length >= sizeof(metadata->path))
        return spill_error(error, MILENA_ERR_OVERFLOW, "Ruta de spill demasiado larga");
    FILE *file = fopen(metadata->path, "wb");
    if (!file) return spill_error(error, MILENA_ERR_IO, "No se pudo crear el spill");
    uint64_t checksum = UINT64_C(1469598103934665603);
    size_t bytes = sizeof(SPILL_MAGIC) - 1u;
    if (fwrite(SPILL_MAGIC, 1u, bytes, file) != bytes) {
        fclose(file); unlink(metadata->path);
        return spill_error(error, MILENA_ERR_IO, "No se pudo escribir cabecera de spill");
    }
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i]) { fclose(file); unlink(metadata->path); return spill_error(error, MILENA_ERR_ARGUMENT, "Clave de spill inválida"); }
        size_t key_size = strlen(keys[i]);
        if (key_size > UINT32_MAX || key_size > SIZE_MAX - sizeof(uint32_t) - sizeof(double)) {
            fclose(file); unlink(metadata->path); return spill_error(error, MILENA_ERR_OVERFLOW, "Clave de spill demasiado larga");
        }
        size_t record_size = sizeof(uint32_t) + key_size + sizeof(double);
        if (policy.max_bytes < sizeof(uint64_t) || record_size > policy.max_bytes - sizeof(uint64_t) ||
            bytes > policy.max_bytes - sizeof(uint64_t) - record_size) {
            fclose(file); unlink(metadata->path); return spill_error(error, MILENA_ERR_OVERFLOW, "El spill supera el límite configurado");
        }
        uint32_t encoded_size = (uint32_t)key_size;
        if (fwrite(&encoded_size, 1u, sizeof(encoded_size), file) != sizeof(encoded_size) ||
            fwrite(keys[i], 1u, key_size, file) != key_size ||
            fwrite(&values[i], 1u, sizeof(double), file) != sizeof(double)) {
            fclose(file); unlink(metadata->path); return spill_error(error, MILENA_ERR_IO, "Error escribiendo spill");
        }
        checksum = hash_bytes(checksum, (const unsigned char *)&encoded_size, sizeof(encoded_size));
        checksum = hash_bytes(checksum, (const unsigned char *)keys[i], key_size);
        checksum = hash_bytes(checksum, (const unsigned char *)&values[i], sizeof(double));
        bytes += record_size;
        metadata->records++;
    }
    if (fwrite(&checksum, 1u, sizeof(checksum), file) != sizeof(checksum) || fclose(file) != 0) {
        unlink(metadata->path); return spill_error(error, MILENA_ERR_IO, "Error cerrando spill");
    }
    metadata->bytes = bytes + sizeof(checksum);
    metadata->checksum = checksum;
    return MILENA_OK;
}

MilenaStatus milena_spill_read(const MilenaSpillPolicy *requested, const MilenaSpillMetadata *metadata,
                               char **keys, double *values, size_t capacity, size_t *count,
                               MilenaError *error) {
    MilenaSpillPolicy defaults = milena_spill_policy_default();
    MilenaSpillPolicy policy = requested ? *requested : defaults;
    if (!metadata || !keys || !values || !count || policy.max_bytes == 0u)
        return spill_error(error, MILENA_ERR_ARGUMENT, "Lectura de spill inválida");
    FILE *file = fopen(metadata->path, "rb");
    if (!file) return spill_error(error, MILENA_ERR_IO, "No se pudo abrir el spill");
    char magic[sizeof(SPILL_MAGIC) - 1u];
    if (fread(magic, 1u, sizeof(magic), file) != sizeof(magic) || memcmp(magic, SPILL_MAGIC, sizeof(magic)) != 0) {
        fclose(file); return spill_error(error, MILENA_ERR_DATA, "Cabecera de spill inválida");
    }
    uint64_t checksum = UINT64_C(1469598103934665603);
    size_t used = sizeof(magic), read_count = 0u;
    while (read_count < capacity) {
        uint32_t encoded_size;
        if (fread(&encoded_size, 1u, sizeof(encoded_size), file) != sizeof(encoded_size)) break;
        size_t key_size = (size_t)encoded_size;
        if (!keys[read_count] || key_size > SIZE_MAX - sizeof(encoded_size) - sizeof(double) ||
            used > policy.max_bytes || key_size + sizeof(encoded_size) + sizeof(double) > policy.max_bytes - used) {
            fclose(file); return spill_error(error, MILENA_ERR_OVERFLOW, "Registro de spill fuera de límites");
        }
        if (fread(keys[read_count], 1u, key_size, file) != key_size ||
            fread(&values[read_count], 1u, sizeof(double), file) != sizeof(double)) {
            fclose(file); return spill_error(error, MILENA_ERR_DATA, "Registro de spill truncado");
        }
        keys[read_count][key_size] = '\0';
        checksum = hash_bytes(checksum, (const unsigned char *)&encoded_size, sizeof(encoded_size));
        checksum = hash_bytes(checksum, (const unsigned char *)keys[read_count], key_size);
        checksum = hash_bytes(checksum, (const unsigned char *)&values[read_count], sizeof(double));
        used += sizeof(encoded_size) + key_size + sizeof(double);
        ++read_count;
    }
    uint64_t stored;
    if (fread(&stored, 1u, sizeof(stored), file) != sizeof(stored) || stored != checksum || stored != metadata->checksum) {
        fclose(file); return spill_error(error, MILENA_ERR_DATA, "Checksum de spill inválido");
    }
    fclose(file); *count = read_count; return MILENA_OK;
}

MilenaStatus milena_spill_remove(MilenaSpillMetadata *metadata, MilenaError *error) {
    if (!metadata || !metadata->path[0]) return spill_error(error, MILENA_ERR_ARGUMENT, "Metadata de spill inválida");
    if (unlink(metadata->path) != 0 && errno != ENOENT) return spill_error(error, MILENA_ERR_IO, "No se pudo limpiar spill");
    metadata->path[0] = '\0'; return MILENA_OK;
}
