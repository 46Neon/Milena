/* C17 replacement for the canonical grouped CSV streaming benchmark. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#define MKDIR(path) mkdir((path), 0700)
#define RMDIR(path) rmdir(path)
#endif

#define MAX_LARGE_ROWS UINT64_C(1000000)
#define RUN_TIMEOUT_SECONDS 180
#define TEMP_DIR "benchmarks/.grouped-stream-benchmark"
#define CSV_PATH TEMP_DIR "/datos.csv"
#define SCRIPT_PATH TEMP_DIR "/agrupado.milena"
#define REPORT_PATH TEMP_DIR "/reporte.json"
#define MAX_REPORT_BYTES (16U * 1024U * 1024U)

typedef struct {
    const char *label;
    uint64_t rows_requested;
    uint64_t groups_expected;
    uint64_t expected_invalid;
    uint64_t rows;
    uint64_t rows_valid;
    uint64_t rows_malformed;
    uint64_t groups;
    uint64_t bytes;
    uint64_t record_bytes;
    uint64_t record_capacity;
    double process_seconds;
    double backend_milliseconds;
    double rows_per_second;
    double megabytes_per_second;
    bool large;
} CaseResult;

static bool clock_seconds(double *value)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) return false;
    *value = (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    *value = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
    return true;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || *text == '\0' || *text == '-') return false;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool json_number(const char *json, const char *key, double *value)
{
    char needle[96];
    const char *p;
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(needle)) return false;
    p = strstr(json, needle);
    if (p == NULL) return false;
    p = strchr(p + (size_t)written, ':');
    if (p == NULL) return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p == '-') return false;
    errno = 0;
    *value = strtod(p, NULL);
    return errno == 0;
}

static bool json_u64(const char *json, const char *key, uint64_t *value)
{
    double number;
    if (!json_number(json, key, &number) || number < 0.0 || number > 18446744073709549568.0) return false;
    *value = (uint64_t)number;
    return true;
}

static uint64_t sum_sum_invalid(const char *json, bool *ok)
{
    const char *cursor = json;
    bool sum_operation = false;
    uint64_t total = 0U;
    *ok = true;
    while ((cursor = strstr(cursor, "\"operacion\"")) != NULL) {
        const char *colon = strchr(cursor, ':');
        const char *next;
        if (colon == NULL) { *ok = false; return 0U; }
        while (*++colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n') { }
        if (*colon != '"') { *ok = false; return 0U; }
        sum_operation = strncmp(colon + 1, "suma\"", 5U) == 0;
        next = strstr(colon + 1, "\"operacion\"");
        if (sum_operation) {
            const char *invalid = strstr(colon + 1, "\"valores_invalidos\"");
            if (invalid != NULL && (next == NULL || invalid < next)) {
                const char *number = strchr(invalid, ':');
                uint64_t value;
                if (number == NULL) { *ok = false; return 0U; }
                ++number;
                while (*number == ' ' || *number == '\t' || *number == '\r' || *number == '\n') ++number;
                if (!parse_u64(number, &value) || UINT64_MAX - total < value) { *ok = false; return 0U; }
                total += value;
            }
        }
        cursor = colon + 1;
    }
    return total;
}

static bool write_csv(uint64_t rows, uint64_t groups, uint64_t malformed_every, uint64_t *malformed)
{
    FILE *file = fopen(CSV_PATH, "wb");
    uint64_t index;
    *malformed = 0U;
    if (file == NULL) return false;
    if (fputs("grupo,valor,referencia\n", file) == EOF) { (void)fclose(file); return false; }
    for (index = 0U; index < rows; ++index) {
        bool bad = malformed_every != 0U && (index + 1U) % malformed_every == 0U;
        if (fprintf(file, "G%04u,", (unsigned int)(index % groups)) < 0) { (void)fclose(file); return false; }
        if (bad) {
            if (fputs("no-num,", file) == EOF) { (void)fclose(file); return false; }
            ++*malformed;
        } else if (fprintf(file, "%.1f,", (double)(index % 101U) / 10.0) < 0) { (void)fclose(file); return false; }
        if (fprintf(file, "r%08llu\n", (unsigned long long)index) < 0) { (void)fclose(file); return false; }
    }
    return fclose(file) == 0;
}

static bool write_program(uint64_t groups)
{
    FILE *file = fopen(SCRIPT_PATH, "wb");
    if (file == NULL) return false;
    if (fprintf(file,
        ".analisis benchmark_agrupado {\n"
        "    datos desde \"datos.csv\" procesar por lotes de 4096 filas con grupos de %llu\n"
        "    agrupar por \"grupo\" resumir { suma de \"valor\"; contar de \"referencia\"; }\n"
        "    guardar resultado en \"reporte.json\"\n"
        "}\n", (unsigned long long)groups) < 0) { (void)fclose(file); return false; }
    return fclose(file) == 0;
}

static bool quote_argument(const char *input, char *output, size_t capacity)
{
#ifdef _WIN32
    size_t used = 0U;
    const char *p = input;
    if (capacity < 3U) return false;
    output[used++] = '"';
    while (*p != '\0') {
        if (*p == '"' || *p == '\n' || *p == '\r' || *p == '%' || *p == '!') return false;
        if (used + 2U >= capacity) return false;
        output[used++] = *p++;
    }
    output[used++] = '"'; output[used] = '\0';
    return true;
#else
    size_t used = 0U;
    const char *p = input;
    if (capacity < 3U) return false;
    output[used++] = '\'';
    while (*p != '\0') {
        if (*p == '\'') {
            static const char escaped[] = "'\\''";
            size_t n = sizeof(escaped) - 1U;
            if (used + n + 2U >= capacity) return false;
            (void)memcpy(output + used, escaped, n); used += n; ++p;
        } else {
            if (used + 2U >= capacity) return false;
            output[used++] = *p++;
        }
    }
    output[used++] = '\''; output[used] = '\0';
    return true;
#endif
}

static bool run_milena(const char *binary, double *elapsed)
{
    char quoted_binary[1024];
    char quoted_script[256];
    char command[1600];
    double start;
    double finish;
    int status;
    if (!quote_argument(binary, quoted_binary, sizeof(quoted_binary)) || !quote_argument(SCRIPT_PATH, quoted_script, sizeof(quoted_script))) return false;
#ifdef _WIN32
    if (snprintf(command, sizeof(command), "%s run %s > NUL 2>&1", quoted_binary, quoted_script) < 0) return false;
#else
    if (snprintf(command, sizeof(command), "timeout %d %s run %s > /dev/null 2>&1", RUN_TIMEOUT_SECONDS, quoted_binary, quoted_script) < 0) return false;
#endif
    if (!clock_seconds(&start)) return false;
    status = system(command);
    if (status != 0 || !clock_seconds(&finish) || finish < start) return false;
    *elapsed = finish - start;
    return true;
}

static char *read_report(void)
{
    FILE *file = fopen(REPORT_PATH, "rb");
    long size;
    char *buffer;
    if (file == NULL) return NULL;
    if (fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0L || (unsigned long)size > (unsigned long)MAX_REPORT_BYTES || fseek(file, 0L, SEEK_SET) != 0) { (void)fclose(file); return NULL; }
    buffer = (char *)malloc((size_t)size + 1U);
    if (buffer == NULL) { (void)fclose(file); return NULL; }
    if (fread(buffer, 1U, (size_t)size, file) != (size_t)size) { free(buffer); (void)fclose(file); return NULL; }
    buffer[(size_t)size] = '\0';
    (void)fclose(file);
    return buffer;
}

static bool run_case(CaseResult *result, const char *binary, const char *label,
                     uint64_t rows, uint64_t groups, uint64_t malformed_every, bool large)
{
    uint64_t expected_bad;
    char *report;
    double process_seconds;
    uint64_t actual_group_limit;
    bool invalid_ok;
    (void)remove(REPORT_PATH);
    if (!write_csv(rows, groups, malformed_every, &expected_bad) || !write_program(groups) || !run_milena(binary, &process_seconds)) return false;
    report = read_report();
    if (report == NULL) return false;
    if (!json_u64(report, "filas", &result->rows) || !json_u64(report, "filas_validas", &result->rows_valid) ||
        !json_u64(report, "filas_malformadas", &result->rows_malformed) || !json_u64(report, "grupos", &result->groups) ||
        !json_u64(report, "limite_grupos", &actual_group_limit) || actual_group_limit != groups || !json_u64(report, "bytes_entrada", &result->bytes) ||
        !json_u64(report, "pico_registro_bytes", &result->record_bytes) || !json_u64(report, "capacidad_buffer_registro_bytes", &result->record_capacity) ||
        !json_number(report, "tiempo_ms", &result->backend_milliseconds) || !json_number(report, "filas_por_segundo", &result->rows_per_second) ||
        !json_number(report, "megabytes_por_segundo", &result->megabytes_per_second)) { free(report); return false; }
    if (result->rows != rows || result->rows_malformed != expected_bad || result->groups != groups || sum_sum_invalid(report, &invalid_ok) != expected_bad || !invalid_ok) { free(report); return false; }
    free(report);
    result->label = label;
    result->rows_requested = rows;
    result->groups_expected = groups;
    result->expected_invalid = expected_bad;
    result->process_seconds = process_seconds;
    result->large = large;
    return true;
}

static bool json_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    if (fputc('"', out) == EOF) return false;
    while (*p != 0U) {
        if (*p == '"' || *p == '\\') { if (fputc('\\', out) == EOF || fputc((int)*p, out) == EOF) return false; }
        else if (*p == '\n') { if (fputs("\\n", out) == EOF) return false; }
        else if (*p == '\r') { if (fputs("\\r", out) == EOF) return false; }
        else if (*p == '\t') { if (fputs("\\t", out) == EOF) return false; }
        else if (*p < 0x20U) { if (fprintf(out, "\\u%04x", (unsigned int)*p) < 0) return false; }
        else if (fputc((int)*p, out) == EOF) return false;
        ++p;
    }
    return fputc('"', out) != EOF;
}

static void platform_name(char *out, size_t capacity)
{
#ifdef _WIN32
    (void)snprintf(out, capacity, "Windows");
#else
    struct utsname system_info;
    if (uname(&system_info) == 0) (void)snprintf(out, capacity, "%s %s %s", system_info.sysname, system_info.release, system_info.machine);
    else (void)snprintf(out, capacity, "POSIX");
#endif
}

static void machine_name(char *out, size_t capacity)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    (void)snprintf(out, capacity, "aarch64");
#elif defined(__x86_64__) || defined(_M_X64)
    (void)snprintf(out, capacity, "x86_64");
#elif defined(__i386__) || defined(_M_IX86)
    (void)snprintf(out, capacity, "i386");
#elif defined(__arm__) || defined(_M_ARM)
    (void)snprintf(out, capacity, "arm");
#else
    (void)snprintf(out, capacity, "unknown");
#endif
}

static bool write_payload(FILE *out, const CaseResult *cases, size_t count, const char *compiler, const char *commit)
{
    char platform[256];
    char machine[64];
    size_t index;
    platform_name(platform, sizeof(platform));
    machine_name(machine, sizeof(machine));
    if (fprintf(out, "{\n  \"schema\": \"milena-grouped-stream-benchmark-v1\",\n  \"workloads\": [\n") < 0) return false;
    for (index = 0U; index < count; ++index) {
        const CaseResult *c = &cases[index];
        if (fprintf(out, "    {\"fixture\":") < 0 || !json_string(out, c->label) || fprintf(out,
            ",\"rows_requested\":%llu,\"groups_expected\":%llu,\"expected_invalid_numeric\":%llu,\"rows\":%llu,\"rows_valid\":%llu,\"rows_malformed\":%llu,\"groups\":%llu,\"bytes\":%llu,\"elapsed_seconds_process\":%.9f,\"backend_elapsed_milliseconds\":%.3f,\"rows_per_second\":%.6f,\"megabytes_per_second\":%.6f,\"observed_record_bytes\":%llu,\"record_buffer_capacity_bytes\":%llu,\"large_opt_in\":%s}%s\n",
            (unsigned long long)c->rows_requested, (unsigned long long)c->groups_expected, (unsigned long long)c->expected_invalid,
            (unsigned long long)c->rows, (unsigned long long)c->rows_valid, (unsigned long long)c->rows_malformed,
            (unsigned long long)c->groups, (unsigned long long)c->bytes, c->process_seconds, c->backend_milliseconds,
            c->rows_per_second, c->megabytes_per_second, (unsigned long long)c->record_bytes,
            (unsigned long long)c->record_capacity, c->large ? "true" : "false", index + 1U == count ? "" : ",") < 0) return false;
    }
    if (fputs("  ],\n  \"environment\": {\"platform\":", out) == EOF || !json_string(out, platform) || fputs(",\"machine\":", out) == EOF || !json_string(out, machine) || fputs(",\"compiler\":", out) == EOF || !json_string(out, compiler) || fputs(",\"commit\":", out) == EOF || !json_string(out, commit) ||
        fputs("},\n  \"methodology\": \"Deterministic generated CSV; canonical lexer-parser-AST-semantic-runtime-stream backend; bounded group and state budgets; wall-clock observations.\",\n  \"limitations\": [\"No spill-to-disk, CSV-aware partition execution, Arrow/Parquet, cloud, Spark, Flink or distributed execution is measured.\",\"Results are not a latency or industrial-scale claim.\"]\n}\n", out) == EOF) return false;
    return true;
}

static void usage(FILE *out, const char *program)
{
    (void)fprintf(out, "Usage: %s [--large-rows N] [--output PATH]\n", program);
}

int main(int argc, char **argv)
{
    const char *binary = getenv("MILENA_BIN");
    const char *compiler = getenv("CC");
    const char *commit = getenv("GITHUB_SHA");
    const char *output_path = NULL;
    uint64_t large_rows = 0U;
    CaseResult cases[3];
    size_t count = 0U;
    size_t index;
    FILE *output = NULL;
    FILE *buffer = NULL;
    bool ok = true;
    (void)setlocale(LC_NUMERIC, "C");
#ifdef _WIN32
    if (binary == NULL || *binary == '\0') binary = "milena.exe";
#else
    if (binary == NULL || *binary == '\0') binary = "./milena";
#endif
    if (compiler == NULL) compiler = "make default CC";
    if (commit == NULL) commit = "unknown";
    for (index = 1U; index < (size_t)argc; ++index) {
        if ((strcmp(argv[index], "--help") == 0) || (strcmp(argv[index], "-h") == 0)) { usage(stdout, argv[0]); return 0; }
        if (strcmp(argv[index], "--large-rows") == 0 && index + 1U < (size_t)argc) {
            if (!parse_u64(argv[++index], &large_rows) || large_rows > MAX_LARGE_ROWS) { (void)fprintf(stderr, "--large-rows must be between 0 and %llu\n", (unsigned long long)MAX_LARGE_ROWS); return 2; }
        } else if (strcmp(argv[index], "--output") == 0 && index + 1U < (size_t)argc) output_path = argv[++index];
        else { usage(stderr, argv[0]); return 2; }
    }
    {
        FILE *executable = fopen(binary, "rb");
        if (executable == NULL) { (void)fprintf(stderr, "build ./milena first (make all)\n"); return 1; }
        (void)fclose(executable);
    }
    if (MKDIR(TEMP_DIR) != 0) { (void)fprintf(stderr, "Cannot create temporary benchmark directory: %s\n", TEMP_DIR); return 1; }
    if (!run_case(&cases[count++], binary, "small", 100U, 4U, 17U, false) ||
        !run_case(&cases[count++], binary, "medium", 10000U, 32U, 997U, false)) ok = false;
    if (ok && large_rows > 0U) ok = run_case(&cases[count++], binary, "large-opt-in", large_rows, large_rows < 1024U ? large_rows : 1024U, 997U, true);
    if (!ok) (void)fprintf(stderr, "Grouped streaming benchmark failed validation or execution.\n");
    if (ok) {
        buffer = tmpfile();
        if (buffer == NULL || !write_payload(buffer, cases, count, compiler, commit) || fflush(buffer) != 0 || fseek(buffer, 0L, SEEK_SET) != 0) ok = false;
        if (ok) {
            char chunk[4096];
            size_t got;
            while ((got = fread(chunk, 1U, sizeof(chunk), buffer)) > 0U) if (fwrite(chunk, 1U, got, stdout) != got) { ok = false; break; }
            if (output_path != NULL && ok) {
                output = fopen(output_path, "wb");
                if (output == NULL || fseek(buffer, 0L, SEEK_SET) != 0) ok = false;
                else while ((got = fread(chunk, 1U, sizeof(chunk), buffer)) > 0U) if (fwrite(chunk, 1U, got, output) != got) { ok = false; break; }
            }
        }
    }
    if (output != NULL && fclose(output) != 0) ok = false;
    if (buffer != NULL && fclose(buffer) != 0) ok = false;
    (void)remove(CSV_PATH); (void)remove(SCRIPT_PATH); (void)remove(REPORT_PATH); (void)RMDIR(TEMP_DIR);
    return ok ? 0 : 1;
}
