#define _POSIX_C_SOURCE 200809L
#include "bytecode_compiler.h"

#include "canonical_compiler.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

static uint16_t test_read_u16le(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t test_read_u32le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int check_typed_v12(const uint8_t *bytes, size_t length,
                           bool require_boolean_constant,
                           bool require_boolean_comparison) {
    if (!bytes || length < MILENA_BYTECODE_HEADER_SIZE ||
        test_read_u16le(bytes + 4) != MILENA_BYTECODE_VERSION_MAJOR ||
        test_read_u16le(bytes + 6) != MILENA_BYTECODE_VERSION_TYPED_MINOR)
        return 1;
    uint16_t register_count = test_read_u16le(bytes + 8);
    uint32_t instruction_count = test_read_u32le(bytes + 12);
    size_t metadata = MILENA_BYTECODE_HEADER_SIZE +
                      (size_t)instruction_count * MILENA_BYTECODE_INSTRUCTION_SIZE;
    if (!register_count || metadata > length || length - metadata < register_count)
        return 1;
    size_t function_count = 0;
    bool found_boolean_register = false;
    bool found_boolean_constant = false;
    bool found_boolean_comparison = false;
    for (uint32_t pc = 0; pc < instruction_count; ++pc) {
        const uint8_t *record = bytes + MILENA_BYTECODE_HEADER_SIZE +
                                (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        if (record[0] == MILENA_BC_FUNCTION) function_count++;
    }
    if (!function_count || function_count > MILENA_BYTECODE_MAX_FUNCTIONS ||
        length - metadata != (size_t)register_count + function_count)
        return 1;
    for (uint16_t reg = 0; reg < register_count; ++reg) {
        uint8_t type = bytes[metadata + reg];
        if (type != MILENA_BC_TYPE_NUMBER && type != MILENA_BC_TYPE_BOOLEAN)
            return 1;
        if (type == MILENA_BC_TYPE_BOOLEAN) found_boolean_register = true;
    }
    if (bytes[metadata + register_count] != MILENA_BC_TYPE_NUMBER)
        return 1;
    for (uint32_t pc = 0; pc < instruction_count; ++pc) {
        const uint8_t *record = bytes + MILENA_BYTECODE_HEADER_SIZE +
                                (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        uint32_t destination = test_read_u32le(record + 4);
        if (record[0] == MILENA_BC_CONST_BOOL) {
            if (destination >= register_count ||
                bytes[metadata + destination] != MILENA_BC_TYPE_BOOLEAN) return 1;
            found_boolean_constant = true;
        }
        if (record[0] >= MILENA_BC_EQ && record[0] <= MILENA_BC_GE) {
            if (destination >= register_count ||
                bytes[metadata + destination] != MILENA_BC_TYPE_BOOLEAN) return 1;
            found_boolean_comparison = true;
        }
        if (record[0] == MILENA_BC_JUMP_IF_FALSE &&
            (destination >= register_count ||
             bytes[metadata + destination] != MILENA_BC_TYPE_BOOLEAN)) return 1;
    }
    if ((require_boolean_constant &&
         (!found_boolean_register || !found_boolean_constant)) ||
        (require_boolean_comparison &&
         (!found_boolean_register || !found_boolean_comparison))) return 1;
    return 0;
}

static int execute_native(const char *path, int *exit_code,
                          char *output, size_t output_capacity) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return -1;
    pid_t child = fork();
    if (child < 0) {
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(126);
        (void)close(pipe_fds[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    (void)close(pipe_fds[1]);
    size_t used = 0;
    bool overflow = false;
    char chunk[64];
    for (;;) {
        ssize_t count = read(pipe_fds[0], chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            (void)close(pipe_fds[0]);
            return -1;
        }
        if (count == 0) break;
        size_t received = (size_t)count;
        if (used + received >= output_capacity) {
            overflow = true;
        } else {
            memcpy(output + used, chunk, received);
            used += received;
        }
    }
    (void)close(pipe_fds[0]);
    if (output_capacity == 0) return -1;
    output[used] = '\0';
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) *exit_code = 128 + WTERMSIG(status);
    else *exit_code = 125;
    return overflow ? 1 : 0;
}

static int native_path(char *directory, size_t directory_capacity,
                       char *executable, size_t executable_capacity) {
    (void)directory_capacity;
    (void)executable_capacity;
    (void)snprintf(directory, directory_capacity,
                   "/tmp/milena-bytecode-aot-test-XXXXXX");
    if (!mkdtemp(directory)) return -1;
    (void)snprintf(executable, executable_capacity, "%s/native-result", directory);
    return 0;
}

static int check_native_success(const uint8_t *bytes, size_t length,
                                double expected) {
    char directory[128];
    char executable[192];
    char output[128];
    int exit_code = -1;
    MilenaError error;
    MilenaBytecodeDiagnostic diagnostic;
    double vm_result = 0.0;
    if (native_path(directory, sizeof(directory), executable, sizeof(executable)) != 0)
        return 1;
    if (milena_bytecode_run(bytes, length, NULL, &vm_result, &diagnostic) != MILENA_BC_OK ||
        !near(vm_result, expected)) {
        (void)rmdir(directory);
        return 1;
    }
    if (milena_bytecode_compile_native(bytes, length, executable, &error) != MILENA_OK) {
        fprintf(stderr, "native compilation failed: %s\n", error.message);
        (void)rmdir(directory);
        return 1;
    }
    int execution_status = execute_native(executable, &exit_code, output, sizeof(output));
    (void)unlink(executable);
    (void)rmdir(directory);
    if (execution_status != 0 || exit_code != 0) return 1;
    char *end = NULL;
    errno = 0;
    double native_result = strtod(output, &end);
    if (errno != 0 || end == output || strcmp(end, "\n") != 0 ||
        !near(native_result, vm_result)) return 1;
    return 0;
}

static int check_native_error_status(const uint8_t *bytes, size_t length,
                                     MilenaBytecodeStatus expected_vm_status,
                                     int expected_exit_code) {
    char directory[128];
    char executable[192];
    char output[128];
    int exit_code = -1;
    MilenaError error;
    MilenaBytecodeDiagnostic diagnostic;
    double result = -1.0;
    if (native_path(directory, sizeof(directory), executable, sizeof(executable)) != 0)
        return 1;
    if (milena_bytecode_run(bytes, length, NULL, &result, &diagnostic) != expected_vm_status) {
        (void)rmdir(directory);
        return 1;
    }
    if (milena_bytecode_compile_native(bytes, length, executable, &error) != MILENA_OK) {
        fprintf(stderr, "native compilation failed: %s\n", error.message);
        (void)rmdir(directory);
        return 1;
    }
    int execution_status = execute_native(executable, &exit_code, output, sizeof(output));
    (void)unlink(executable);
    (void)rmdir(directory);
    return execution_status == 0 && exit_code == expected_exit_code && output[0] == '\0' ? 0 : 1;
}

static int check_end_to_end(const char *function_source,
                            const char *reference_source,
                            double expected, bool require_boolean_constant,
                            bool require_boolean_comparison) {
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
    CHECK(check_typed_v12(bytes, length, require_boolean_constant,
                          require_boolean_comparison) == 0,
          "canonical source lowering must emit complete v1.2 HIR-derived type metadata");
    CHECK(milena_bytecode_verify(bytes, length, NULL, &diagnostic) == MILENA_BC_OK,
          diagnostic.message);
    CHECK(milena_bytecode_run(bytes, length, NULL, &actual, &diagnostic) == MILENA_BC_OK,
          diagnostic.message);
    CHECK(near(actual, expected), "compiled entry result differs from expected value");
    CHECK(check_native_success(bytes, length, expected) == 0,
          "native AOT result must match the same verified source-derived bytecode and VM");

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

static int check_call_limits(const char *source) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    MilenaError error;
    MilenaBytecodeDiagnostic diagnostic;
    CHECK(milena_bytecode_compile_source(source, &bytes, &length, &error) == MILENA_OK,
          error.message);
    double value = -1.0;
    MilenaBytecodeLimits shallow = {.max_call_depth = 1};
    CHECK(milena_bytecode_run(bytes, length, &shallow, &value, &diagnostic) ==
              MILENA_BC_LIMIT_EXCEEDED && value == 0.0,
          "bounded VM call frames must fail with a zero result at the configured depth");
    CHECK(milena_bytecode_run(bytes, length, NULL, &value, &diagnostic) == MILENA_BC_OK &&
              near(value, 7.0),
          "a subsequent run must succeed after call-frame failure cleanup");
    MilenaBytecodeLimits low_fuel = {.max_steps = 2};
    CHECK(milena_bytecode_run(bytes, length, &low_fuel, &value, &diagnostic) ==
              MILENA_BC_STEP_LIMIT && value == 0.0,
          "nested calls must consume the shared bounded instruction budget");
    CHECK(milena_bytecode_run(bytes, length, NULL, &value, &diagnostic) == MILENA_BC_OK &&
              near(value, 7.0),
          "a subsequent run must succeed after fuel exhaustion cleanup");
    uint8_t *mutated = malloc(length);
    CHECK(mutated != NULL, "could not allocate malformed-call test buffer");
    memcpy(mutated, bytes, length);
    bool changed = false;
    for (size_t pc = 0; pc < (length - MILENA_BYTECODE_HEADER_SIZE) /
                              MILENA_BYTECODE_INSTRUCTION_SIZE; ++pc) {
        uint8_t *record = mutated + MILENA_BYTECODE_HEADER_SIZE +
                          pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        if (record[0] == MILENA_BC_CALL) {
            record[8] = 63; /* nonexistent function id */
            changed = true;
            break;
        }
    }
    CHECK(changed && milena_bytecode_verify(mutated, length, NULL, &diagnostic) ==
                         MILENA_BC_BAD_OPERAND,
          "verifier must reject malformed function ids before VM execution");
    free(mutated);
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
    uint8_t *bytes = NULL;
    size_t length = 0;
    MilenaError compile_error;
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
    const char *boolean_locals =
        "funcion principal() { variable activo = verdadero; "
        "variable coincide = activo == verdadero; variable resultado = 0; "
        "si (coincide) { resultado = 17; } sino { resultado = 21; } "
        "retornar resultado; }";
    const char *boolean_locals_reference =
        "funcion principal() { variable activo = verdadero; "
        "variable coincide = activo == verdadero; variable resultado = 0; "
        "si (coincide) { resultado = 17; } sino { resultado = 21; } "
        "retornar resultado; } variable salida = principal();";
    const char *numeric_condition =
        "funcion principal() { si (1) { retornar 1; } "
        "sino { retornar 0; } }";
    const char *recursive =
        "funcion principal() { retornar ida(1); } "
        "funcion ida(x) { retornar vuelta(x); } "
        "funcion vuelta(y) { retornar ida(y); }";
    /* `suma` is a reserved statistical-function token, not a user-function
       identifier. Use an ordinary identifier while exercising forward calls. */
    const char *forward_calls =
        "funcion principal() { retornar combinar(doble(5), 3); } "
        "funcion doble(x) { retornar x * 2; } "
        "funcion combinar(a, b) { retornar a + b; }";
    const char *forward_calls_reference =
        "funcion principal() { retornar combinar(doble(5), 3); } "
        "funcion doble(x) { retornar x * 2; } "
        "funcion combinar(a, b) { retornar a + b; } "
        "variable salida = principal();";
    const char *nested_calls =
        "funcion principal() { retornar uno(7); } "
        "funcion uno(x) { retornar dos(x); } "
        "funcion dos(y) { retornar y; }";
    const char *call_runtime_error =
        "funcion principal() { retornar dividir(1, 0); } "
        "funcion dividir(a, b) { retornar a / b; }";
    const char *bad_call_arity =
        "funcion principal() { retornar doble(); } "
        "funcion doble(x) { retornar x; }";
    const char *bad_call_type =
        "funcion principal() { retornar elevar(verdadero); } "
        "funcion elevar(x) { retornar x; }";
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

    CHECK(check_end_to_end(arithmetic, arithmetic_reference, 13.0, false, false) == 0,
          "arithmetic source-to-v1.2-bytecode end-to-end test failed");
    CHECK(check_end_to_end(branch_true, branch_true_reference, 10.0, false, true) == 0,
          "comparison-driven control-flow source test failed");
    CHECK(check_end_to_end(branch_false, branch_false_reference, 20.0, true, false) == 0,
          "boolean-literal control-flow source test failed");
    CHECK(check_end_to_end(boolean_locals, boolean_locals_reference, 17.0, true, true) == 0,
          "boolean locals/comparison/control-flow source test failed");
    CHECK(check_end_to_end(forward_calls, forward_calls_reference, 13.0, false, false) == 0,
          "forward helper calls must agree across interpreter, bytecode VM, and native AOT");
    CHECK(check_call_limits(nested_calls) == 0,
          "call frame, fuel, malformed-target, and cleanup tests failed");
    CHECK(milena_bytecode_compile_source(call_runtime_error, &bytes, &length,
                                         &compile_error) == MILENA_OK,
          compile_error.message);
    CHECK(check_native_error_status(bytes, length, MILENA_BC_RUNTIME_ERROR, 70) == 0,
          "runtime errors inside a direct call must match VM and native AOT status");
    free(bytes); bytes = NULL; length = 0;
    CHECK(milena_bytecode_compile_source(bad_call_arity, &bytes, &length,
                                         &compile_error) == MILENA_ERR_TYPE &&
              bytes == NULL && length == 0 && compile_error.line > 0,
          "canonical semantic resolution must reject wrong call arity before lowering");
    CHECK(milena_bytecode_compile_source(bad_call_type, &bytes, &length,
                                         &compile_error) == MILENA_ERR_TYPE &&
              bytes == NULL && length == 0 && compile_error.line > 0,
          "canonical semantic resolution must reject argument/signature type mismatches");
    CHECK(check_rejected(recursive, "recursión") == 0,
          "recursive and mutually recursive graphs must be explicitly rejected");
    CHECK(check_rejected(parameter, "cero parámetros") == 0,
          "parameters should be explicitly rejected");
    CHECK(check_rejected(global, "sentencias globales") == 0,
          "global statements should be explicitly rejected");
    CHECK(milena_bytecode_compile_source(multiple_functions, &bytes, &length,
                                         &compile_error) == MILENA_OK,
          "an unused non-recursive helper function should be accepted");
    free(bytes); bytes = NULL; length = 0;
    CHECK(check_rejected(unreachable, "inalcanzable") == 0,
          "unreachable statements should be explicitly rejected");
    CHECK(check_rejected(fallthrough, "Todos los caminos") == 0,
          "a function with a fallthrough path should be explicitly rejected");
    CHECK(check_rejected(mixed_comparison, "Comparación HIR mixta") == 0,
          "mixed-type comparisons should be explicitly rejected without coercion");
    CHECK(check_rejected(numeric_condition, "booleana en bytecode v1.2") == 0,
          "numeric control conditions must fail closed rather than weaken v1.2 verification");

    puts("bytecode compiler tests: canonical HIR-to-v1.2 lowering, verification, VM, and AOT parity OK");
    return 0;
}
