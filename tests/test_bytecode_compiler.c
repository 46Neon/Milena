#include "bytecode_compiler.h"

#include "canonical_compiler.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (message)); \
            return 1; \
        } \
    } while (0)

static bool near(double actual, double expected) {
    return fabs(actual - expected) < 1e-12;
}

static int check_end_to_end(const char *function_source,
                            const char *reference_source,
                            double expected) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    MilenaError error;
    MilenaBytecodeDiagnostic diagnostic;
    double actual = -123.0;
    CHECK(milena_bytecode_compile_source(function_source, &bytes, &length,
                                         &error) == MILENA_OK,
          error.message);
    CHECK(bytes != NULL && length > MILENA_BYTECODE_HEADER_SIZE,
          "source lowering must return encoded bytecode");
    CHECK(milena_bytecode_verify(bytes, length, NULL, &diagnostic) == MILENA_BC_OK,
          diagnostic.message);
    CHECK(milena_bytecode_run(bytes, length, NULL, &actual, &diagnostic) == MILENA_BC_OK,
          diagnostic.message);
    CHECK(near(actual, expected), "compiled entry result differs from expected value");

    /* The interpreter runs the corresponding complete program with a global
       binding. The bytecode API compiles only principal's entry-function body. */
    MilenaCanonicalProgram canonical;
    milena_canonical_program_init(&canonical);
    CHECK(milena_canonical_program_parse(&canonical, reference_source, &error) == MILENA_OK,
          error.message);
    Interpreter interpreter;
    CHECK(interpreter_init(&interpreter, canonical.ast),
          "interpreter initialization failed for reference program");
    CHECK(interpreter_run(&interpreter),
          "interpreter failed to execute the wrapper/global-binding program");
    double reference = -123.0;
    CHECK(interpreter_get_number(&interpreter, "salida", &reference),
          "interpreter did not publish the expected global binding");
    CHECK(near(actual, reference), "bytecode entry result differs from interpreter reference");
    interpreter_destroy(&interpreter);
    milena_canonical_program_release(&canonical);
    free(bytes);
    return 0;
}

static int check_rejected(const char *source, const char *message_fragment) {
    uint8_t *bytes = (uint8_t *)(uintptr_t)1;
    size_t length = 99;
    MilenaError error;
    MilenaStatus status = milena_bytecode_compile_source(source, &bytes,
                                                          &length, &error);
    CHECK(status == MILENA_ERR_UNSUPPORTED,
          "unsupported source subset must fail with MILENA_ERR_UNSUPPORTED");
    CHECK(bytes == NULL && length == 0,
          "failed lowering must not publish partial bytecode");
    CHECK(error.line > 0 && error.column > 0,
          "unsupported lowering diagnostic must preserve a source span");
    CHECK(strstr(error.message, message_fragment) != NULL,
          "unsupported lowering diagnostic must name the unsupported construct");
    return 0;
}

int main(void) {
    const char *arithmetic =
        "funcion principal() { variable base = 3; "
        "variable total = base * 4 + 2; total = total - 1; retornar total; }";
    const char *arithmetic_reference =
        "funcion principal() { variable base = 3; "
        "variable total = base * 4 + 2; total = total - 1; retornar total; } "
        "variable salida = principal();";
    const char *branch_true =
        "funcion principal() { variable x = 0; "
        "si (3 > 2) { x = 10; } sino { x = 20; } retornar x; }";
    const char *branch_true_reference =
        "funcion principal() { variable x = 0; "
        "si (3 > 2) { x = 10; } sino { x = 20; } retornar x; } "
        "variable salida = principal();";
    const char *branch_false =
        "funcion principal() { variable x = 0; "
        "si (falso) { x = 10; } sino { x = 20; } retornar x; }";
    const char *branch_false_reference =
        "funcion principal() { variable x = 0; "
        "si (falso) { x = 10; } sino { x = 20; } retornar x; } "
        "variable salida = principal();";
    const char *call =
        "funcion principal() { retornar principal(); }";
    const char *parameter =
        "funcion principal(x) { retornar x; }";
    const char *global =
        "funcion principal() { retornar 1; } variable extra = 2;";
    const char *multiple_functions =
        "funcion principal() { retornar 1; } funcion auxiliar() { retornar 2; }";
    const char *unreachable =
        "funcion principal() { retornar 1; variable despues = 2; }";
    const char *fallthrough =
        "funcion principal() { si (verdadero) { retornar 1; } }";
    const char *mixed_comparison =
        "funcion principal() { si (verdadero < 2) { retornar 1; } sino { retornar 0; } }";

    CHECK(check_end_to_end(arithmetic, arithmetic_reference, 13.0) == 0,
          "arithmetic source-to-bytecode end-to-end test failed");
    CHECK(check_end_to_end(branch_true, branch_true_reference, 10.0) == 0,
          "true-branch source-to-bytecode end-to-end test failed");
    CHECK(check_end_to_end(branch_false, branch_false_reference, 20.0) == 0,
          "false-branch source-to-bytecode end-to-end test failed");
    CHECK(check_rejected(call, "llamadas de función") == 0,
          "calls should be explicitly rejected");
    CHECK(check_rejected(parameter, "cero parámetros") == 0,
          "parameters should be explicitly rejected");
    CHECK(check_rejected(global, "sentencias globales") == 0,
          "global statements should be explicitly rejected");
    CHECK(check_rejected(multiple_functions, "una función") == 0,
          "multiple functions should be explicitly rejected");
    CHECK(check_rejected(unreachable, "inalcanzable") == 0,
          "unreachable statements should be explicitly rejected");
    CHECK(check_rejected(fallthrough, "Todos los caminos") == 0,
          "a function with a fallthrough path should be explicitly rejected");
    CHECK(check_rejected(mixed_comparison, "Comparación HIR mixta") == 0,
          "mixed-type comparisons should be explicitly rejected without coercion");

    puts("bytecode compiler tests: canonical HIR lowering, verification, VM, and interpreter reference OK");
    return 0;
}
