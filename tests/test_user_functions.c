#include "user_functions.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    MilenaFunctionTable t; double r; char e[160] = {0};
    milena_function_table_init(&t);
    assert(milena_parse_numeric_functions(
        "funcion doble(x) { retorna x * 2; }"
        "funcion suma(a, b) { variable local = a + b; retorna local; }"
        "funcion recursiva(n) { retorna n; }", &t, e, sizeof e));
    assert(milena_function_call(&t, "doble", (double[]){21}, 1, &r, e, sizeof e) && r == 42);
    assert(milena_function_call(&t, "suma", (double[]){4, 5}, 2, &r, e, sizeof e) && r == 9);
    assert(!milena_function_call(&t, "suma", (double[]){4}, 1, &r, e, sizeof e));
    assert(strstr(e, "cantidad") || strstr(e, "Función"));
    milena_function_table_release(&t); puts("user functions: ok"); return 0;
}
