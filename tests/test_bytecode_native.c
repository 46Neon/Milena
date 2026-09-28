#define _POSIX_C_SOURCE 200809L
#include "bytecode_compiler.h"

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

static int encode(const MilenaBytecodeInstruction *instructions, size_t count,
                  uint16_t registers, uint8_t **bytes_out, size_t *length_out) {
    MilenaBytecodeProgram program = {
        MILENA_BYTECODE_VERSION_MAJOR, MILENA_BYTECODE_VERSION_MINOR,
        registers, count, instructions, NULL, 0, NULL, 0
    };
    size_t length = 0;
    MilenaBytecodeDiagnostic diagnostic;
    if (!milena_bytecode_encoded_size(count, &length)) return 1;
    uint8_t *bytes = malloc(length);
    if (!bytes) return 1;
    size_t written = 0;
    MilenaBytecodeStatus status = milena_bytecode_encode(
        &program, bytes, length, &written, NULL, &diagnostic);
    if (status != MILENA_BC_OK || written != length) {
        fprintf(stderr, "bytecode setup failed: %s\n", diagnostic.message);
        free(bytes);
        return 1;
    }
    *bytes_out = bytes;
    *length_out = length;
    return 0;
}

static int execute(const char *path, int *exit_code, char *output,
                   size_t output_capacity) {
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
    char buffer[64];
    for (;;) {
        ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            (void)close(pipe_fds[0]);
            return -1;
        }
        if (count == 0) break;
        size_t received = (size_t)count;
        if (used + received >= output_capacity) overflow = true;
        else {
            memcpy(output + used, buffer, received);
            used += received;
        }
    }
    (void)close(pipe_fds[0]);
    if (!output_capacity) return -1;
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

static int compile_and_check_error(const MilenaBytecodeInstruction *instructions,
                                   size_t count, uint16_t registers,
                                   MilenaBytecodeStatus expected_status,
                                   int expected_exit) {
    char directory[] = "/tmp/milena-bytecode-native-XXXXXX";
    CHECK(mkdtemp(directory) != NULL, "could not create AOT test directory");
    char path[160];
    (void)snprintf(path, sizeof(path), "%s/native", directory);
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(encode(instructions, count, registers, &bytes, &length) == 0,
          "could not encode AOT test bytecode");
    MilenaBytecodeDiagnostic diagnostic;
    double result = -1.0;
    CHECK(milena_bytecode_run(bytes, length, NULL, &result, &diagnostic) == expected_status,
          "VM status differs from expected native-runtime comparison");
    MilenaError error;
    CHECK(milena_bytecode_compile_native(bytes, length, path, &error) == MILENA_OK,
          error.message);
    char output[128];
    int exit_code = -1;
    int run_status = execute(path, &exit_code, output, sizeof(output));
    (void)unlink(path);
    (void)rmdir(directory);
    free(bytes);
    CHECK(run_status == 0 && exit_code == expected_exit && output[0] == '\0',
          "native runtime failure/step exhaustion must match the VM status mapping");
    return 0;
}

int main(void) {
    const MilenaBytecodeInstruction divide_by_zero[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 1.0},
        {MILENA_BC_CONST_F64, 1, 0, 0, 0.0},
        {MILENA_BC_DIV, 2, 0, 1, 0.0},
        {MILENA_BC_RETURN, 2, 0, 0, 0.0}
    };
    const MilenaBytecodeInstruction infinite_loop[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 1.0},
        {MILENA_BC_JUMP_IF_FALSE, 0, 3, 0, 0.0},
        {MILENA_BC_JUMP, 1, 0, 0, 0.0},
        {MILENA_BC_RETURN, 0, 0, 0, 0.0}
    };
    CHECK(compile_and_check_error(divide_by_zero, 4, 3,
                                  MILENA_BC_RUNTIME_ERROR, 70) == 0,
          "native division-by-zero behavior differed from VM");
    CHECK(compile_and_check_error(infinite_loop, 4, 1,
                                  MILENA_BC_STEP_LIMIT, 71) == 0,
          "native instruction budget behavior differed from VM");

    uint8_t *bytes = NULL;
    size_t length = 0;
    const MilenaBytecodeInstruction valid[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 42.0},
        {MILENA_BC_RETURN, 0, 0, 0, 0.0}
    };
    CHECK(encode(valid, 2, 1, &bytes, &length) == 0,
          "could not encode validation test bytecode");
    bytes[0] = (uint8_t)'X';
    char directory[] = "/tmp/milena-bytecode-invalid-XXXXXX";
    CHECK(mkdtemp(directory) != NULL, "could not create invalid-input test directory");
    char path[160];
    (void)snprintf(path, sizeof(path), "%s/must-not-exist", directory);
    MilenaError error;
    CHECK(milena_bytecode_compile_native(bytes, length, path, &error) == MILENA_ERR_DATA,
          "invalid bytecode must be rejected before invoking native compilation");
    CHECK(access(path, F_OK) != 0,
          "invalid bytecode must not publish an executable");
    (void)rmdir(directory);
    free(bytes);

    char injection_marker[96];
    (void)snprintf(injection_marker, sizeof(injection_marker),
                   "milena_aot_shell_marker_%ld", (long)getpid());
    errno = 0;
    CHECK(access(injection_marker, F_OK) != 0 && errno == ENOENT,
          "unexpected pre-existing shell-injection marker");
    const MilenaBytecodeInstruction literal_return[] = {
        {MILENA_BC_CONST_F64, 0, 0, 0, 5.25},
        {MILENA_BC_RETURN, 0, 0, 0, 0.0}
    };
    CHECK(encode(literal_return, 2, 1, &bytes, &length) == 0,
          "could not encode argv-safety test bytecode");
    char odd_directory[] = "/tmp/milena-bytecode-argv-XXXXXX";
    CHECK(mkdtemp(odd_directory) != NULL, "could not create argv-safety test directory");
    (void)snprintf(path, sizeof(path), "%s/native$(touch %s)",
                   odd_directory, injection_marker);
    CHECK(milena_bytecode_compile_native(bytes, length, path, &error) == MILENA_OK,
          error.message);
    errno = 0;
    CHECK(access(injection_marker, F_OK) != 0 && errno == ENOENT,
          "output path was interpreted by a shell");
    char output[128];
    int exit_code = -1;
    CHECK(execute(path, &exit_code, output, sizeof(output)) == 0 && exit_code == 0,
          "native artifact with shell metacharacters in its path did not execute");
    char *end = NULL;
    double native_result = strtod(output, &end);
    CHECK(end != output && strcmp(end, "\n") == 0 && fabs(native_result - 5.25) < 1e-12,
          "native executable returned the wrong value");
    (void)unlink(path);
    (void)rmdir(odd_directory);
    free(bytes);

    /* The native backend consumes the same verified v1.2 boolean contract. */
    const MilenaBytecodeInstruction typed_code[] = {
        {MILENA_BC_FUNCTION, 0, 0, 0, 0.0},
        {MILENA_BC_CONST_BOOL, 0, 0, 0, 1.0},
        {MILENA_BC_JUMP_IF_FALSE, 0, 4, 0, 0.0},
        {MILENA_BC_CONST_F64, 1, 0, 0, 42.0},
        {MILENA_BC_RETURN, 1, 0, 0, 0.0},
        {MILENA_BC_CONST_F64, 1, 0, 0, 0.0},
        {MILENA_BC_RETURN, 1, 0, 0, 0.0}
    };
    const uint8_t typed_registers[] = {
        MILENA_BC_TYPE_BOOLEAN, MILENA_BC_TYPE_NUMBER
    };
    const uint8_t typed_returns[] = {MILENA_BC_TYPE_NUMBER};
    MilenaBytecodeProgram typed_program = {
        MILENA_BYTECODE_VERSION_MAJOR, MILENA_BYTECODE_VERSION_TYPED_MINOR,
        2, 7, typed_code, typed_registers, 2, typed_returns, 1
    };
    CHECK(milena_bytecode_typed_encoded_size(7, 2, 1, &length),
          "could not size typed AOT bytecode");
    bytes = malloc(length);
    CHECK(bytes != NULL, "could not allocate typed AOT bytecode");
    size_t typed_written = 0;
    MilenaBytecodeDiagnostic typed_diagnostic;
    CHECK(milena_bytecode_encode(&typed_program, bytes, length, &typed_written,
                                 NULL, &typed_diagnostic) == MILENA_BC_OK &&
          typed_written == length, "could not encode typed AOT bytecode");
    double typed_result = 0.0;
    CHECK(milena_bytecode_run(bytes, length, NULL, &typed_result,
                              &typed_diagnostic) == MILENA_BC_OK &&
          fabs(typed_result - 42.0) < 1e-12, "typed VM result differed");
    char typed_directory[] = "/tmp/milena-bytecode-typed-aot-XXXXXX";
    CHECK(mkdtemp(typed_directory) != NULL,
          "could not create typed AOT test directory");
    (void)snprintf(path, sizeof(path), "%s/native", typed_directory);
    CHECK(milena_bytecode_compile_native(bytes, length, path, &error) == MILENA_OK,
          "native backend rejected valid typed bytecode");
    CHECK(execute(path, &exit_code, output, sizeof(output)) == 0 && exit_code == 0,
          "typed native executable did not succeed");
    native_result = strtod(output, &end);
    CHECK(end != output && strcmp(end, "\n") == 0 &&
          fabs(native_result - typed_result) < 1e-12,
          "typed native executable differed from VM");
    (void)unlink(path);
    (void)rmdir(typed_directory);
    free(bytes);

    puts("bytecode native AOT tests: verified input, v1.2 types, direct native control flow, runtime errors, budget, and safe argv OK");
    return 0;
}
