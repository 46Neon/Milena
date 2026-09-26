#include "common.h"
#include "entrypoints.h"
#include "bytecode.h"
#include "bytecode_compiler.h"
#include "canonical_compiler.h"
#include <limits.h>

#define CLI_MAX_SOURCE_BYTES (16u * 1024u * 1024u)

static void cli_set_error(MilenaError *error, MilenaStatus status,
                          const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static MilenaStatus cli_read_source(const char *path, char **source_out,
                                    MilenaError *error) {
    if (source_out) *source_out = NULL;
    if (!path || !source_out) {
        cli_set_error(error, MILENA_ERR_ARGUMENT,
                      "Se requiere una ruta de fuente .milena");
        return MILENA_ERR_ARGUMENT;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
                       "No se pudo abrir el archivo de fuente: %s", path);
        cli_set_error(error, MILENA_ERR_IO, message);
        return MILENA_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        cli_set_error(error, MILENA_ERR_IO,
                      "No se pudo determinar el tamaño del archivo de fuente");
        return MILENA_ERR_IO;
    }
    long end = ftell(file);
    if (end < 0 || (unsigned long)end > (unsigned long)CLI_MAX_SOURCE_BYTES) {
        (void)fclose(file);
        cli_set_error(error, MILENA_ERR_UNSUPPORTED,
                      "El archivo de fuente supera el límite de 16 MiB de la CLI bytecode");
        return MILENA_ERR_UNSUPPORTED;
    }
    size_t length = (size_t)end;
    if (fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        cli_set_error(error, MILENA_ERR_IO,
                      "No se pudo volver al inicio del archivo de fuente");
        return MILENA_ERR_IO;
    }
    char *source = malloc(length + 1u);
    if (!source) {
        (void)fclose(file);
        cli_set_error(error, MILENA_ERR_MEMORY,
                      "Sin memoria para leer el archivo de fuente");
        return MILENA_ERR_MEMORY;
    }
    size_t received = fread(source, 1u, length, file);
    bool failed = received != length || ferror(file) != 0;
    if (fclose(file) != 0) failed = true;
    if (failed) {
        free(source);
        cli_set_error(error, MILENA_ERR_IO,
                      "No se pudo leer completamente el archivo de fuente");
        return MILENA_ERR_IO;
    }
    if (memchr(source, '\0', length) != NULL) {
        free(source);
        cli_set_error(error, MILENA_ERR_PARSE,
                      "La fuente .milena contiene un byte NUL no admitido");
        return MILENA_ERR_PARSE;
    }
    source[length] = '\0';
    *source_out = source;
    return MILENA_OK;
}

static MilenaStatus cli_compile_source(const char *path, uint8_t **bytes_out,
                                       size_t *length_out, MilenaError *error) {
    if (bytes_out) *bytes_out = NULL;
    if (length_out) *length_out = 0;
    char *source = NULL;
    MilenaStatus status = cli_read_source(path, &source, error);
    if (status != MILENA_OK) return status;

    MilenaCanonicalProgram canonical;
    milena_canonical_program_init(&canonical);
    status = milena_canonical_program_parse(&canonical, source, error);
    if (status != MILENA_OK) {
        milena_canonical_program_release(&canonical);
        free(source);
        return status;
    }
    MilenaCanonicalCompilerInput input = {0};
    status = milena_canonical_compiler_input(&canonical, &input, error);
    if (status == MILENA_OK && (!input.hir || input.data_hir)) {
        cli_set_error(error, MILENA_ERR_UNSUPPORTED,
                      "La CLI bytecode solo admite programas de HIR escalar tipada; la HIR de datos aún no está soportada");
        status = MILENA_ERR_UNSUPPORTED;
    }
    if (status == MILENA_OK) {
        status = milena_bytecode_compile_hir(input.hir, bytes_out,
                                             length_out, error);
    }
    milena_canonical_program_release(&canonical);
    free(source);
    if (status != MILENA_OK) return status;

    MilenaBytecodeDiagnostic diagnostic;
    if (*length_out < MILENA_BYTECODE_HEADER_SIZE ||
        (*bytes_out)[4] != MILENA_BYTECODE_VERSION_MAJOR ||
        (*bytes_out)[5] != 0u ||
        (*bytes_out)[6] != MILENA_BYTECODE_VERSION_TYPED_MINOR ||
        (*bytes_out)[7] != 0u) {
        free(*bytes_out);
        *bytes_out = NULL;
        *length_out = 0;
        cli_set_error(error, MILENA_ERR_INTERNAL,
                      "El compilador HIR no produjo bytecode tipado MLBC v1.2");
        return MILENA_ERR_INTERNAL;
    }
    MilenaBytecodeStatus bytecode_status = milena_bytecode_verify(
        *bytes_out, *length_out, NULL, &diagnostic);
    if (bytecode_status != MILENA_BC_OK) {
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
                       "El bytecode MLBC v1.2 no pasó verificación en offset %zu: %s",
                       diagnostic.byte_offset, diagnostic.message);
        free(*bytes_out);
        *bytes_out = NULL;
        *length_out = 0;
        cli_set_error(error, MILENA_ERR_DATA, message);
        return MILENA_ERR_DATA;
    }
    return MILENA_OK;
}

static MilenaStatus cli_run_vm(const char *path, MilenaError *error) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    MilenaStatus status = cli_compile_source(path, &bytes, &length, error);
    if (status != MILENA_OK) return status;

    double result = 0.0;
    MilenaBytecodeDiagnostic diagnostic;
    MilenaBytecodeStatus vm_status = milena_bytecode_run(
        bytes, length, NULL, &result, &diagnostic);
    free(bytes);
    if (vm_status != MILENA_BC_OK) {
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
                       "La VM MLBC falló (%s): %s",
                       milena_bytecode_status_name(vm_status), diagnostic.message);
        cli_set_error(error, MILENA_ERR_INTERNAL, message);
        return MILENA_ERR_INTERNAL;
    }
    if (printf("%a\n", result) < 0 || fflush(stdout) != 0) {
        cli_set_error(error, MILENA_ERR_IO,
                      "No se pudo escribir el resultado de la VM en stdout");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

static MilenaStatus cli_build_native(const char *source_path,
                                     const char *output_path,
                                     MilenaError *error) {
#if defined(MILENA_BYTECODE_NATIVE_AVAILABLE) && defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    if (!output_path || output_path[0] == '\0') {
        cli_set_error(error, MILENA_ERR_ARGUMENT,
                      "El comando build requiere una ruta de salida no vacía");
        return MILENA_ERR_ARGUMENT;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    MilenaStatus status = cli_compile_source(source_path, &bytes, &length, error);
    if (status == MILENA_OK)
        status = milena_bytecode_compile_native(bytes, length, output_path, error);
    free(bytes);
    return status;
#else
    (void)source_path;
    (void)output_path;
    cli_set_error(error, MILENA_ERR_UNSUPPORTED,
                  "El backend AOT nativo solo está disponible en Linux x86-64; este destino no está soportado");
    return MILENA_ERR_UNSUPPORTED;
#endif
}

static int runtime_self_check(void) {
    /* This is deliberately dependency-free: it runs on bionic as well as glibc. */
    if (CHAR_BIT != 8 || sizeof(void *) < 4 || sizeof(size_t) < sizeof(void *)) {
        fprintf(stderr, "Milena self-check: unsupported C runtime data model\n");
        return 1;
    }
    volatile double zero = 0.0;
    char probe[4];
    if (!isfinite(zero) || snprintf(probe, sizeof(probe), "%s", "ok") < 0) {
        fprintf(stderr, "Milena self-check: C library/math contract failed\n");
        return 1;
    }
    printf("Milena self-check: OK (%s, %zu-bit pointers)\n",
#if defined(__ANDROID__)
           "Android/bionic",
#elif defined(_WIN32)
           "Windows",
#else
           "POSIX",
#endif
           sizeof(void *) * CHAR_BIT);
    return 0;
}

static void usage(const char *program) {
    printf("Milena %s\n", MILENA_VERSION);
    printf("Uso:\n");
    printf("  %s analizar <csv> <json>\n", program);
    printf("  %s perfil <csv> <json>\n", program);
    printf("  %s run <archivo.milena>\n", program);
    printf("  %s vm <archivo.milena>\n", program);
    printf("  %s build <archivo.milena> -o <programa>\n", program);
    printf("  %s inspect <csv>\n", program);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("%s\n", MILENA_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--self-check") == 0) {
        return runtime_self_check();
    }
    MilenaError error;
    milena_error_clear(&error);
    MilenaStatus status = MILENA_ERR_ARGUMENT;

    if (strcmp(argv[1], "analizar") == 0 && argc == 4) {
        status = milena_cli_analyze(argv[2], argv[3], stdout, &error);
    } else if (strcmp(argv[1], "perfil") == 0 && argc == 4) {
        status = milena_cli_profile(argv[2], argv[3], stdout, &error);
    } else if (strcmp(argv[1], "run") == 0 && argc == 3) {
        status = milena_cli_run_script(argv[2], &error);
    } else if (strcmp(argv[1], "vm") == 0 && argc == 3) {
        status = cli_run_vm(argv[2], &error);
    } else if (strcmp(argv[1], "build") == 0 && argc == 5 &&
               strcmp(argv[3], "-o") == 0) {
        status = cli_build_native(argv[2], argv[4], &error);
    } else if (strcmp(argv[1], "inspect") == 0 && argc == 3) {
        status = milena_cli_inspect(argv[2], stdout, &error);
    } else {
        usage(argv[0]);
        return 2;
    }

    if (status != MILENA_OK) {
        milena_error_print(&error, stderr);
        return 1;
    }
    return 0;
}
