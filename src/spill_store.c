#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "spill_store.h"

#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static const unsigned char SPILL_MAGIC[4] = {'M', 'L', 'S', '1'};
static unsigned long spill_temp_counter = 0;

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

/* Scan and validate a complete prefix. A valid file larger than the caller's
 * quota is rejected, never mistaken for a corrupt tail and truncated. */
static MilenaStatus scan_store(FILE *file, size_t quota, size_t max_record,
                              size_t *valid_bytes, size_t *records,
                              bool *bad_tail, MilenaError *error) {
    unsigned char header[MILENA_SPILL_HEADER_SIZE];
    size_t offset = 0;
    *valid_bytes = 0;
    *records = 0;
    *bad_tail = false;
    for (;;) {
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
            spill_error(error, MILENA_ERR_OVERFLOW,
                        "Un registro spill válido puede exceder el límite configurado");
            return MILENA_ERR_OVERFLOW;
        }
        size_t length = (size_t)raw_length;
        size_t record_bytes = 0, end_offset = 0;
        if (!milena_size_add(MILENA_SPILL_HEADER_SIZE, length, &record_bytes) ||
            !milena_size_add(record_bytes, MILENA_SPILL_TRAILER_SIZE, &record_bytes) ||
            !milena_size_add(offset, record_bytes, &end_offset)) {
            spill_error(error, MILENA_ERR_OVERFLOW,
                        "El tamaño declarado del spill no es representable");
            return MILENA_ERR_OVERFLOW;
        }
        unsigned char *payload = length ? (unsigned char *)malloc(length) : NULL;
        if (length && !payload) {
            spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente al recuperar spill");
            return MILENA_ERR_MEMORY;
        }
        unsigned char trailer[MILENA_SPILL_TRAILER_SIZE];
        bool complete = read_exact(file, payload, length) &&
                        read_exact(file, trailer, sizeof(trailer));
        bool checksum_ok = complete &&
            spill_checksum(payload, length) == get_u32(trailer);
        free(payload);
        if (!checksum_ok) {
            if (ferror(file)) {
                spill_error(error, MILENA_ERR_IO, "No se pudo leer el spill");
                return MILENA_ERR_IO;
            }
            *bad_tail = true;
            break;
        }
        if (end_offset > quota) {
            spill_error(error, MILENA_ERR_OVERFLOW,
                        "El spill válido supera la cuota configurada");
            return MILENA_ERR_OVERFLOW;
        }
        offset = end_offset;
        *valid_bytes = offset;
        if (*records == SIZE_MAX) {
            spill_error(error, MILENA_ERR_OVERFLOW, "Demasiados registros spill");
            return MILENA_ERR_OVERFLOW;
        }
        (*records)++;
    }
    return MILENA_OK;
}

static bool activate_recovered_file(const char *tmp_path, const char *path) {
#ifdef _WIN32
    return MoveFileExA(tmp_path, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp_path, path) == 0;
#endif
}

static unsigned long current_process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
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
        if (errno == ENOENT) {
            if (error) milena_error_clear(error);
            return MILENA_OK;
        }
        spill_error(error, MILENA_ERR_IO, "No se pudo abrir el spill para recuperar");
        return MILENA_ERR_IO;
    }
    if (fseek(input, 0, SEEK_END) != 0) {
        fclose(input);
        spill_error(error, MILENA_ERR_IO, "No se pudo medir el spill");
        return MILENA_ERR_IO;
    }
    long end = ftell(input);
    if (end < 0 || (uintmax_t)end > (uintmax_t)SIZE_MAX) {
        fclose(input);
        spill_error(error, MILENA_ERR_OVERFLOW, "El archivo spill es demasiado grande");
        return MILENA_ERR_OVERFLOW;
    }
    size_t total = (size_t)end;
    rewind(input);
    size_t valid = 0, records = 0;
    bool bad_tail = false;
    MilenaStatus status = scan_store(input, quota_bytes, max_record_bytes,
                                     &valid, &records, &bad_tail, error);
    if (fclose(input) != 0 && status == MILENA_OK) {
        spill_error(error, MILENA_ERR_IO, "No se pudo cerrar el spill");
        status = MILENA_ERR_IO;
    }
    if (status != MILENA_OK) return status;

    recovery->records_recovered = records;
    recovery->bytes_recovered = valid;
    recovery->bytes_discarded = total >= valid ? total - valid : 0;
    recovery->truncated_tail = bad_tail && recovery->bytes_discarded > 0;
    if (recovery->truncated_tail) {
        size_t path_len = strlen(path);
        if (path_len > SIZE_MAX - 80) {
            spill_error(error, MILENA_ERR_OVERFLOW, "La ruta spill es demasiado larga");
            return MILENA_ERR_OVERFLOW;
        }
        size_t temp_capacity = path_len + 80;
        char *tmp_path = (char *)malloc(temp_capacity);
        if (!tmp_path) {
            spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente al truncar spill");
            return MILENA_ERR_MEMORY;
        }
        FILE *output = NULL;
        for (unsigned int attempt = 0; attempt < 16 && !output; attempt++) {
            unsigned long serial = ++spill_temp_counter;
            int n = snprintf(tmp_path, temp_capacity, "%s.recover.%lu.%lu",
                             path, current_process_id(), serial);
            if (n < 0 || (size_t)n >= temp_capacity) break;
            output = fopen(tmp_path, "wbx");
            if (!output && errno != EEXIST) break;
        }
        if (!output) {
            free(tmp_path);
            spill_error(error, MILENA_ERR_IO, "No se pudo crear spill recuperado");
            return MILENA_ERR_IO;
        }
        input = fopen(path, "rb");
        if (!input) {
            fclose(output);
            remove(tmp_path);
            free(tmp_path);
            spill_error(error, MILENA_ERR_IO, "No se pudo reabrir spill para recuperarlo");
            return MILENA_ERR_IO;
        }
        unsigned char buffer[8192];
        size_t left = valid;
        bool copied = true;
        while (left > 0) {
            size_t want = left < sizeof(buffer) ? left : sizeof(buffer);
            if (fread(buffer, 1, want, input) != want ||
                fwrite(buffer, 1, want, output) != want) {
                copied = false;
                break;
            }
            left -= want;
        }
        bool input_closed = fclose(input) == 0;
        bool output_closed = fclose(output) == 0;
        bool replaced = copied && input_closed && output_closed &&
                        activate_recovered_file(tmp_path, path);
        if (!replaced) remove(tmp_path);
        free(tmp_path);
        if (!replaced) {
            spill_error(error, MILENA_ERR_IO,
                        "No se pudo activar spill recuperado; el original se conserva");
            return MILENA_ERR_IO;
        }
    }
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_store_visit(const char *path, size_t quota_bytes,
                                      size_t max_record_bytes,
                                      MilenaSpillVisitFn visitor,
                                      void *context,
                                      MilenaError *error) {
    if (!path || !path[0] || !visitor) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para recorrer spill");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaSpillRecovery recovery;
    MilenaStatus status = milena_spill_store_recover(path, quota_bytes,
                                                      max_record_bytes,
                                                      &recovery, error);
    if (status != MILENA_OK) return status;
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            if (error) milena_error_clear(error);
            return MILENA_OK;
        }
        spill_error(error, MILENA_ERR_IO, "No se pudo abrir el spill para recorrerlo");
        return MILENA_ERR_IO;
    }
    unsigned char header[MILENA_SPILL_HEADER_SIZE];
    size_t index = 0, bytes_seen = 0;
    for (;;) {
        size_t header_bytes = fread(header, 1, sizeof(header), file);
        if (header_bytes == 0) {
            if (ferror(file)) {
                status = MILENA_ERR_IO;
                spill_error(error, status, "Error al leer spill durante recorrido");
            }
            break;
        }
        if (header_bytes != sizeof(header)) {
            status = MILENA_ERR_DATA;
            spill_error(error, status, "Cabecera spill truncada durante recorrido");
            break;
        }
        uint64_t raw_length = get_u64(header + 8);
        if (memcmp(header, SPILL_MAGIC, 4) != 0 ||
            get_u32(header + 4) != MILENA_SPILL_VERSION ||
            raw_length > SIZE_MAX || raw_length > max_record_bytes) {
            status = MILENA_ERR_DATA;
            spill_error(error, status, "Cabecera spill inválida durante recorrido");
            break;
        }
        size_t length = (size_t)raw_length, record_bytes = 0, end_offset = 0;
        if (!milena_size_add(MILENA_SPILL_HEADER_SIZE, length, &record_bytes) ||
            !milena_size_add(record_bytes, MILENA_SPILL_TRAILER_SIZE, &record_bytes) ||
            !milena_size_add(bytes_seen, record_bytes, &end_offset) ||
            end_offset > quota_bytes) {
            status = MILENA_ERR_OVERFLOW;
            spill_error(error, status, "El spill excede la cuota durante recorrido");
            break;
        }
        unsigned char *payload = length ? (unsigned char *)malloc(length) : NULL;
        if (length && !payload) {
            status = MILENA_ERR_MEMORY;
            spill_error(error, status, "Memoria insuficiente al recorrer spill");
            break;
        }
        unsigned char trailer[MILENA_SPILL_TRAILER_SIZE];
        bool complete = read_exact(file, payload, length) &&
                        read_exact(file, trailer, sizeof(trailer));
        bool checksum_ok = complete &&
            spill_checksum(payload, length) == get_u32(trailer);
        if (!checksum_ok) {
            free(payload);
            status = ferror(file) ? MILENA_ERR_IO : MILENA_ERR_DATA;
            spill_error(error, status, "Registro spill inválido durante recorrido");
            break;
        }
        status = visitor(payload, length, index, context, error);
        free(payload);
        if (status != MILENA_OK) break;
        if (index == SIZE_MAX) {
            status = MILENA_ERR_OVERFLOW;
            spill_error(error, status, "Demasiados registros durante recorrido");
            break;
        }
        index++;
        bytes_seen = end_offset;
    }
    if (fclose(file) != 0 && status == MILENA_OK) {
        status = MILENA_ERR_IO;
        spill_error(error, status, "No se pudo cerrar spill");
    }
    if (status == MILENA_OK && error) milena_error_clear(error);
    return status;
}

static FILE *spill_create_file_exclusive(const char *path, bool *exists) {
    if (exists) *exists = false;
#ifdef _WIN32
    HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (exists && (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS))
            *exists = true;
        return NULL;
    }
    int descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (descriptor < 0) {
        CloseHandle(handle);
        (void)DeleteFileA(path);
        return NULL;
    }
    FILE *file = _fdopen(descriptor, "w+b");
    if (!file) {
        _close(descriptor);
        (void)DeleteFileA(path);
    }
    return file;
#else
    int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = open(path, flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        if (exists && errno == EEXIST) *exists = true;
        return NULL;
    }
    FILE *file = fdopen(descriptor, "w+b");
    if (!file) {
        int saved_errno = errno;
        (void)close(descriptor);
        (void)unlink(path);
        errno = saved_errno;
    }
    return file;
#endif
}

MilenaStatus milena_spill_store_create_exclusive(
    const char *path, size_t quota_bytes, size_t max_record_bytes,
    MilenaSpillStore *store, MilenaError *error) {
    if (!path || !path[0] || !store || max_record_bytes == 0 ||
        quota_bytes < MILENA_SPILL_HEADER_SIZE + MILENA_SPILL_TRAILER_SIZE) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para crear spill exclusivo");
        return MILENA_ERR_ARGUMENT;
    }
    memset(store, 0, sizeof(*store));
    bool existed = false;
    FILE *file = spill_create_file_exclusive(path, &existed);
    if (!file) {
        MilenaStatus status = existed ? MILENA_ERR_ARGUMENT : MILENA_ERR_IO;
        spill_error(error, status, existed ? "La ruta spill ya existe" :
                    "No se pudo crear spill exclusivo");
        return status;
    }
    char *store_path = milena_strdup(path);
    if (!store_path) {
        (void)fclose(file);
#ifdef _WIN32
        (void)DeleteFileA(path);
#else
        (void)unlink(path);
#endif
        spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente para crear spill");
        return MILENA_ERR_MEMORY;
    }
    store->path = store_path;
    store->file = file;
    store->quota_bytes = quota_bytes;
    store->max_record_bytes = max_record_bytes;
    store->bytes_used = 0;
    store->record_count = 0;
    store->failed = false;
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
    MilenaStatus status = milena_spill_store_recover(path, quota_bytes,
                                                      max_record_bytes,
                                                      &recovery, error);
    if (status != MILENA_OK) return status;
    FILE *file = fopen(path, "ab");
    if (!file) {
        spill_error(error, MILENA_ERR_IO, "No se pudo abrir el spill para escritura");
        return MILENA_ERR_IO;
    }
    char *store_path = milena_strdup(path);
    if (!store_path) {
        fclose(file);
        spill_error(error, MILENA_ERR_MEMORY, "Memoria insuficiente para abrir spill");
        return MILENA_ERR_MEMORY;
    }
    store->path = store_path;
    store->file = file;
    store->quota_bytes = quota_bytes;
    store->max_record_bytes = max_record_bytes;
    store->bytes_used = recovery.bytes_recovered;
    store->record_count = recovery.records_recovered;
    store->failed = false;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_store_append(MilenaSpillStore *store, const void *data,
                                       size_t length, MilenaError *error) {
    if (!store || !store->file || store->failed ||
        (length > 0 && !data) || length > store->max_record_bytes) {
        spill_error(error, store && store->failed ? MILENA_ERR_IO : MILENA_ERR_ARGUMENT,
                    "Registro inválido o spill requiere recuperación tras error de escritura");
        return store && store->failed ? MILENA_ERR_IO : MILENA_ERR_ARGUMENT;
    }
    size_t total = 0;
    if (store->bytes_used > store->quota_bytes || store->record_count == SIZE_MAX ||
        !milena_size_add(MILENA_SPILL_HEADER_SIZE, length, &total) ||
        !milena_size_add(total, MILENA_SPILL_TRAILER_SIZE, &total) ||
        total > store->quota_bytes - store->bytes_used) {
        spill_error(error, MILENA_ERR_OVERFLOW, "La cuota de spill fue agotada");
        return MILENA_ERR_OVERFLOW;
    }
    unsigned char header[MILENA_SPILL_HEADER_SIZE], trailer[MILENA_SPILL_TRAILER_SIZE];
    memcpy(header, SPILL_MAGIC, 4);
    put_u32(header + 4, MILENA_SPILL_VERSION);
    put_u64(header + 8, (uint64_t)length);
    put_u32(trailer, spill_checksum((const unsigned char *)data, length));
    if (fwrite(header, 1, sizeof(header), store->file) != sizeof(header) ||
        (length > 0 && fwrite(data, 1, length, store->file) != length) ||
        fwrite(trailer, 1, sizeof(trailer), store->file) != sizeof(trailer) ||
        fflush(store->file) != 0) {
        store->failed = true;
        spill_error(error, MILENA_ERR_IO, "No se pudo escribir el registro spill; reabra para recuperar");
        return MILENA_ERR_IO;
    }
    store->bytes_used += total;
    store->record_count++;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_store_close(MilenaSpillStore *store, MilenaError *error) {
    if (!store || !store->file) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Spill no abierto");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = fclose(store->file) == 0 ? MILENA_OK : MILENA_ERR_IO;
    free(store->path);
    store->path = NULL;
    store->file = NULL;
    if (status != MILENA_OK) spill_error(error, status, "No se pudo cerrar el spill");
    else if (error) milena_error_clear(error);
    return status;
}

MilenaStatus milena_spill_reader_open(const char *path, size_t quota_bytes,
                                      size_t max_record_bytes,
                                      MilenaSpillReader *reader,
                                      MilenaError *error) {
    if (!path || !path[0] || !reader || max_record_bytes == 0) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Parámetros inválidos para lector spill");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaSpillRecovery recovery;
    MilenaStatus status = milena_spill_store_recover(path, quota_bytes,
                                                      max_record_bytes,
                                                      &recovery, error);
    if (status != MILENA_OK) return status;
    FILE *file = fopen(path, "rb");
    if (!file) { spill_error(error, MILENA_ERR_IO, "No se pudo abrir run spill"); return MILENA_ERR_IO; }
    reader->file = file; reader->quota_bytes = quota_bytes;
    reader->max_record_bytes = max_record_bytes; reader->bytes_read = 0;
    reader->record_count = 0;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_reader_next(MilenaSpillReader *reader,
                                      void *buffer, size_t capacity,
                                      size_t *length, bool *has_record,
                                      MilenaError *error) {
    if (!reader || !reader->file || !length || !has_record) {
        spill_error(error, MILENA_ERR_ARGUMENT, "Lector spill no inicializado");
        return MILENA_ERR_ARGUMENT;
    }
    *length = 0; *has_record = false;
    unsigned char header[MILENA_SPILL_HEADER_SIZE];
    size_t got = fread(header, 1, sizeof(header), reader->file);
    if (got == 0) {
        if (ferror(reader->file)) { spill_error(error, MILENA_ERR_IO, "Error al leer run spill"); return MILENA_ERR_IO; }
        if (error) milena_error_clear(error);
        return MILENA_OK;
    }
    if (got != sizeof(header)) { spill_error(error, MILENA_ERR_DATA, "Cabecera spill truncada"); return MILENA_ERR_DATA; }
    uint64_t raw_length = get_u64(header + 8);
    if (memcmp(header, SPILL_MAGIC, 4) != 0 || get_u32(header + 4) != MILENA_SPILL_VERSION ||
        raw_length > SIZE_MAX || raw_length > reader->max_record_bytes) {
        spill_error(error, MILENA_ERR_DATA, "Cabecera de run spill inválida"); return MILENA_ERR_DATA;
    }
    size_t record_length = (size_t)raw_length, record_size = 0, next_bytes = 0;
    if (!milena_size_add(MILENA_SPILL_HEADER_SIZE, record_length, &record_size) ||
        !milena_size_add(record_size, MILENA_SPILL_TRAILER_SIZE, &record_size) ||
        !milena_size_add(reader->bytes_read, record_size, &next_bytes) ||
        next_bytes > reader->quota_bytes) {
        spill_error(error, MILENA_ERR_OVERFLOW, "Run spill excede cuota al leer"); return MILENA_ERR_OVERFLOW;
    }
    if (record_length > capacity || (record_length > 0 && !buffer)) {
        spill_error(error, MILENA_ERR_OVERFLOW, "Buffer insuficiente para registro spill"); return MILENA_ERR_OVERFLOW;
    }
    unsigned char trailer[MILENA_SPILL_TRAILER_SIZE];
    if (!read_exact(reader->file, buffer, record_length) || !read_exact(reader->file, trailer, sizeof(trailer))) {
        spill_error(error, ferror(reader->file) ? MILENA_ERR_IO : MILENA_ERR_DATA, "Payload spill truncado");
        return ferror(reader->file) ? MILENA_ERR_IO : MILENA_ERR_DATA;
    }
    if (spill_checksum((const unsigned char *)buffer, record_length) != get_u32(trailer)) {
        spill_error(error, MILENA_ERR_DATA, "Checksum de registro spill inválido"); return MILENA_ERR_DATA;
    }
    if (reader->record_count == SIZE_MAX) { spill_error(error, MILENA_ERR_OVERFLOW, "Demasiados registros spill"); return MILENA_ERR_OVERFLOW; }
    reader->bytes_read = next_bytes; reader->record_count++;
    *length = record_length; *has_record = true;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}

MilenaStatus milena_spill_reader_close(MilenaSpillReader *reader,
                                       MilenaError *error) {
    if (!reader || !reader->file) { spill_error(error, MILENA_ERR_ARGUMENT, "Lector spill no abierto"); return MILENA_ERR_ARGUMENT; }
    int result = fclose(reader->file); reader->file = NULL;
    if (result != 0) { spill_error(error, MILENA_ERR_IO, "No se pudo cerrar lector spill"); return MILENA_ERR_IO; }
    if (error) milena_error_clear(error);
    return MILENA_OK;
}
