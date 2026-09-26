#include "common.h"
#include "entrypoints.h"
#include "native_aot.h"
#include <limits.h>

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
    printf("  %s build <archivo.milena> -o <ejecutable>\n", program);
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
    } else if (strcmp(argv[1], "build") == 0 && argc == 5 &&
               strcmp(argv[3], "-o") == 0) {
        status = milena_cli_build(argv[2], argv[4], &error);
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
