/* Reproducible measurements of a clean Milena build and canonical execution. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <sys/wait.h>
#endif

#define MAX_REPETITIONS 10000U
#define FIXTURE_PATH "benchmarks/ejecucion.milena"

static bool clock_seconds(double *value)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return false;
    }
    *value = (double)counter.QuadPart / (double)frequency.QuadPart;
    return true;
#else
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
        return false;
    }
    *value = (double)current.tv_sec + (double)current.tv_nsec / 1000000000.0;
    return true;
#endif
}

static bool command_succeeded(const char *command)
{
    int status = system(command);
    if (status == -1) {
        return false;
    }
#ifdef _WIN32
    return status == 0;
#else
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static bool run_timed(const char *command, double *elapsed)
{
    double start;
    double finish;
    if (!clock_seconds(&start)) {
        (void)fprintf(stderr, "ERROR: monotonic clock is unavailable\n");
        return false;
    }
    if (!command_succeeded(command)) {
        (void)fprintf(stderr, "ERROR: command failed: %s\n", command);
        return false;
    }
    if (!clock_seconds(&finish) || finish < start) {
        (void)fprintf(stderr, "ERROR: invalid clock sample\n");
        return false;
    }
    *elapsed = finish - start;
    return true;
}

static bool parse_repetitions(const char *text, size_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0UL ||
        parsed > (unsigned long)MAX_REPETITIONS) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static bool print_json_string(FILE *stream, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    if (fputc('"', stream) == EOF) {
        return false;
    }
    while (*cursor != 0U) {
        switch (*cursor) {
        case '"': if (fputs("\\\"", stream) == EOF) return false; break;
        case '\\': if (fputs("\\\\", stream) == EOF) return false; break;
        case '\b': if (fputs("\\b", stream) == EOF) return false; break;
        case '\f': if (fputs("\\f", stream) == EOF) return false; break;
        case '\n': if (fputs("\\n", stream) == EOF) return false; break;
        case '\r': if (fputs("\\r", stream) == EOF) return false; break;
        case '\t': if (fputs("\\t", stream) == EOF) return false; break;
        default:
            if (*cursor < 0x20U) {
                if (fprintf(stream, "\\u%04x", (unsigned int)*cursor) < 0) return false;
            } else if (fputc((int)*cursor, stream) == EOF) {
                return false;
            }
            break;
        }
        ++cursor;
    }
    return fputc('"', stream) != EOF;
}

static bool print_summary(FILE *stream, const double *samples, size_t count)
{
    double *sorted;
    double minimum = samples[0];
    double maximum = samples[0];
    double total = 0.0;
    double median;
    size_t index;

    sorted = (double *)malloc(count * sizeof(*sorted));
    if (sorted == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        sorted[index] = samples[index];
        if (samples[index] < minimum) minimum = samples[index];
        if (samples[index] > maximum) maximum = samples[index];
        total += samples[index];
    }
    qsort(sorted, count, sizeof(*sorted), compare_double);
    if (count % 2U == 0U) {
        median = (sorted[count / 2U - 1U] + sorted[count / 2U]) / 2.0;
    } else {
        median = sorted[count / 2U];
    }
    free(sorted);
    return fprintf(stream,
                   "\"samples\":%zu,\"min_seconds\":%.9f,\"median_seconds\":%.9f,\"mean_seconds\":%.9f,\"max_seconds\":%.9f",
                   count, minimum, median, total / (double)count, maximum) >= 0;
}

static void machine_name(char output[64])
{
#if defined(__aarch64__) || defined(_M_ARM64)
    (void)snprintf(output, 64U, "aarch64");
#elif defined(__x86_64__) || defined(_M_X64)
    (void)snprintf(output, 64U, "x86_64");
#elif defined(__i386__) || defined(_M_IX86)
    (void)snprintf(output, 64U, "i386");
#elif defined(__arm__) || defined(_M_ARM)
    (void)snprintf(output, 64U, "arm");
#else
    (void)snprintf(output, 64U, "unknown");
#endif
}

static void platform_name(char output[256])
{
#ifdef _WIN32
    (void)snprintf(output, 256U, "Windows");
#else
    struct utsname identity;
    if (uname(&identity) == 0) {
        (void)snprintf(output, 256U, "%s %s %s", identity.sysname,
                       identity.release, identity.machine);
    } else {
        (void)snprintf(output, 256U, "POSIX");
    }
#endif
}

static void print_usage(FILE *stream, const char *program)
{
    (void)fprintf(stream,
                  "Usage: %s [--compile-repetitions N] [--run-repetitions N] [--output PATH]\n"
                  "Measure clean C17 builds and canonical Milena script execution.\n",
                  program);
}

int main(int argc, char **argv)
{
    size_t compile_repetitions = 1U;
    size_t run_repetitions = 5U;
    const char *output_path = NULL;
    const char *compiler = getenv("CC");
    const char *commit = getenv("GITHUB_SHA");
    char platform[256];
    char machine[64];
    double *compile_samples;
    double *run_samples;
    const char *build_command;
    const char *execution_command;
    const char *make_check_command;
    size_t index;
    FILE *fixture;
    FILE *result_file = NULL;
    bool ok = true;

#ifdef _WIN32
    make_check_command = "make --version > NUL 2>&1";
    build_command = "make -B all > NUL";
    execution_command = "milena.exe run " FIXTURE_PATH " > NUL";
#else
    make_check_command = "make --version > /dev/null 2>&1";
    build_command = "make -B all > /dev/null";
    execution_command = "./milena run " FIXTURE_PATH " > /dev/null";
#endif
    for (index = 1U; index < (size_t)argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[index], "--compile-repetitions") == 0 && index + 1U < (size_t)argc) {
            if (!parse_repetitions(argv[++index], &compile_repetitions)) {
                (void)fprintf(stderr, "ERROR: compile repetitions must be between 1 and %u\n", MAX_REPETITIONS);
                return 2;
            }
        } else if (strcmp(argv[index], "--run-repetitions") == 0 && index + 1U < (size_t)argc) {
            if (!parse_repetitions(argv[++index], &run_repetitions)) {
                (void)fprintf(stderr, "ERROR: run repetitions must be between 1 and %u\n", MAX_REPETITIONS);
                return 2;
            }
        } else if (strcmp(argv[index], "--output") == 0 && index + 1U < (size_t)argc) {
            output_path = argv[++index];
        } else {
            print_usage(stderr, argv[0]);
            return 2;
        }
    }
    if (system(make_check_command) != 0) {
        (void)fprintf(stderr, "ERROR: make is required for the compile benchmark\n");
        return 1;
    }
    fixture = fopen(FIXTURE_PATH, "rb");
    if (fixture == NULL) {
        (void)fprintf(stderr, "ERROR: missing benchmark fixture: %s\n", FIXTURE_PATH);
        return 1;
    }
    (void)fclose(fixture);
    compile_samples = (double *)calloc(compile_repetitions, sizeof(*compile_samples));
    run_samples = (double *)calloc(run_repetitions, sizeof(*run_samples));
    if (compile_samples == NULL || run_samples == NULL) {
        (void)fprintf(stderr, "ERROR: unable to allocate benchmark samples\n");
        free(compile_samples);
        free(run_samples);
        return 1;
    }
    for (index = 0U; index < compile_repetitions; ++index) {
#ifdef _WIN32
        const char *clean_command = "make clean > NUL";
#else
        const char *clean_command = "make clean > /dev/null";
#endif
        if (!command_succeeded(clean_command) || !run_timed(build_command, &compile_samples[index])) {
            ok = false;
            break;
        }
    }
    if (ok) {
#ifdef _WIN32
        fixture = fopen("milena.exe", "rb");
#else
        fixture = fopen("milena", "rb");
#endif
        if (fixture == NULL) {
            (void)fprintf(stderr, "ERROR: build completed without producing the Milena binary\n");
            ok = false;
        } else {
            (void)fclose(fixture);
        }
    }
    for (index = 0U; ok && index < run_repetitions; ++index) {
        if (!run_timed(execution_command, &run_samples[index])) {
            ok = false;
        }
    }
    if (!ok) {
        free(compile_samples);
        free(run_samples);
        return 1;
    }
    if (output_path != NULL) {
        result_file = fopen(output_path, "wb");
        if (result_file == NULL) {
            (void)fprintf(stderr, "ERROR: cannot write output file: %s\n", output_path);
            free(compile_samples);
            free(run_samples);
            return 1;
        }
    }
    platform_name(platform);
    machine_name(machine);
    if (compiler == NULL || compiler[0] == '\0') compiler = "make default CC";
    if (commit == NULL || commit[0] == '\0') commit = "unknown";
    {
        FILE *streams[2] = {stdout, result_file};
        size_t stream_count = result_file == NULL ? 1U : 2U;
        size_t stream_index;
        for (stream_index = 0U; stream_index < stream_count; ++stream_index) {
            FILE *stream = streams[stream_index];
            (void)fprintf(stream, "{\n  \"schema\": \"milena-benchmark-v1\",\n  \"fixture\": \"%s\",\n", FIXTURE_PATH);
            (void)fprintf(stream, "  \"compile\": {\"command\": \"make clean && make -B all\", ");
            if (!print_summary(stream, compile_samples, compile_repetitions)) ok = false;
            (void)fprintf(stream, "},\n  \"execution\": {\"command\": ");
#ifdef _WIN32
            if (!print_json_string(stream, "milena.exe run benchmarks/ejecucion.milena")) ok = false;
#else
            if (!print_json_string(stream, "./milena run benchmarks/ejecucion.milena")) ok = false;
#endif
            (void)fprintf(stream, ", ");
            if (!print_summary(stream, run_samples, run_repetitions)) ok = false;
            (void)fprintf(stream, "},\n  \"environment\": {\"python\": null, \"harness\": \"C17\", \"platform\": ");
            if (!print_json_string(stream, platform)) ok = false;
            (void)fprintf(stream, ", \"machine\": ");
            if (!print_json_string(stream, machine)) ok = false;
            (void)fprintf(stream, ", \"compiler\": ");
            if (!print_json_string(stream, compiler)) ok = false;
            (void)fprintf(stream, ", \"commit\": ");
            if (!print_json_string(stream, commit)) ok = false;
            (void)fprintf(stream, "},\n  \"notes\": [\n    \"Wall-clock samples; no performance guarantee.\",\n    \"Compile samples start from a clean tree and use the repository Makefile.\",\n    \"Execution uses the canonical lexer-parser-AST-semantic-runtime path; this fixture is array-only.\"\n  ]\n}\n");
            if (ferror(stream)) ok = false;
        }
    }
    if (result_file != NULL && fclose(result_file) != 0) ok = false;
    free(compile_samples);
    free(run_samples);
    return ok ? 0 : 1;
}
