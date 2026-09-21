#include "entrypoints.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void write_fixture(const char *path) {
    FILE *out = fopen(path, "wb");
    assert(out);
    fputs("fecha,precio,cantidad\n2026-01-01,10,2\n2026-01-02,3,4\n", out);
    assert(fclose(out) == 0);
}

static void rewind_and_assert(FILE *out, const char *expected) {
    char buffer[512] = {0};
    rewind(out);
    assert(fread(buffer, 1, sizeof(buffer) - 1, out) > 0);
    assert(strstr(buffer, expected) != NULL);
}

int main(void) {
    const char *csv = "tests/entrypoint_fixture.csv";
    const char *json = "tests/entrypoint_fixture.json";
    write_fixture(csv);

    MilenaError error;
    FILE *output = tmpfile();
    assert(output);
    assert(milena_cli_inspect(csv, output, &error) == MILENA_OK);
    rewind_and_assert(output, "fecha | precio | cantidad");
    fclose(output);

    output = tmpfile();
    assert(output);
    assert(milena_cli_analyze(csv, json, output, &error) == MILENA_OK);
    rewind_and_assert(output, "Total: 32.00 | Promedio: 16.00");
    fclose(output);

    remove(csv);
    remove(json);
    puts("entrypoints: old CLI outputs preserved through canonical frontend");
    return 0;
}
