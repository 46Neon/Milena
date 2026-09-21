#include "common.h"
#include "entrypoints.h"

static void usage(const char *program) {
    printf("Milena %s\n", MILENA_VERSION);
    printf("Uso:\n");
    printf("  %s analizar <csv> <json>\n", program);
    printf("  %s perfil <csv> <json>\n", program);
    printf("  %s run <archivo.milena>\n", program);
    printf("  %s inspect <csv>\n", program);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    MilenaError error;
    milena_error_clear(&error);
    MilenaStatus status = MILENA_ERR_ARGUMENT;

    if (strcmp(argv[1], "analizar") == 0 && argc == 4) {
        status = milena_cli_analyze(argv[2], argv[3], stdout, &error);
    } else if (strcmp(argv[1], "perfil") == 0 && argc == 4) {
        status = milena_cli_profile(argv[2], argv[3], stdout, &error);
    } else if (strcmp(argv[1], "run") == 0 && argc == 3) {
        status = milena_cli_run_script(argv[2], &error);
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
