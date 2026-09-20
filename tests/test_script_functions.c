#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "script.h"
int main(void) {
    const char *path = "test-functions.milena";
    FILE *f = fopen(path, "w"); assert(f);
    fputs("funcion doble(x) { retorna x * 2; }\n"
          "funcion suma(a, b) { variable parcial = doble(a); retorna parcial + b; }\n"
          "llamar suma(4, 5);\n", f); fclose(f);
    MilenaError e; milena_error_clear(&e);
    assert(milena_run_script(path, &e) == MILENA_OK);
    remove(path); puts("script functions: ok"); return 0;
}
