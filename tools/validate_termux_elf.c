/* C17 fail-closed ELF boundary check for a native Termux/aarch64 binary. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} CapturedOutput;

static void captured_free(CapturedOutput *output)
{
    free(output->text);
    output->text = NULL;
    output->length = 0U;
    output->capacity = 0U;
}

static bool captured_append(CapturedOutput *output, const char *data, size_t length)
{
    size_t required;
    size_t next_capacity;
    char *next;

    if (length > (size_t)-1 - output->length - 1U) {
        return false;
    }
    required = output->length + length + 1U;
    if (required > output->capacity) {
        next_capacity = output->capacity == 0U ? 4096U : output->capacity;
        while (next_capacity < required) {
            if (next_capacity > (size_t)-1 / 2U) {
                next_capacity = required;
                break;
            }
            next_capacity *= 2U;
        }
        next = (char *)realloc(output->text, next_capacity);
        if (next == NULL) {
            return false;
        }
        output->text = next;
        output->capacity = next_capacity;
    }
    memcpy(output->text + output->length, data, length);
    output->length += length;
    output->text[output->length] = '\0';
    return true;
}

static int run_and_capture(char *const arguments[], CapturedOutput *output)
{
    int pipe_fds[2];
    pid_t child;
    int status = 0;
    char buffer[4096];

    output->text = NULL;
    output->length = 0U;
    output->capacity = 0U;
    if (pipe(pipe_fds) != 0) {
        return -1;
    }
    child = fork();
    if (child < 0) {
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
            dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(pipe_fds[1]);
        execvp(arguments[0], arguments);
        (void)fprintf(stderr, "cannot execute %s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    (void)close(pipe_fds[1]);
    for (;;) {
        ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            if (!captured_append(output, buffer, (size_t)count)) {
                (void)close(pipe_fds[0]);
                (void)waitpid(child, &status, 0);
                captured_free(output);
                return -1;
            }
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            (void)close(pipe_fds[0]);
            (void)waitpid(child, &status, 0);
            captured_free(output);
            return -1;
        }
    }
    (void)close(pipe_fds[0]);
    {
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            captured_free(output);
            return -1;
        }
    }
    if (!WIFEXITED(status)) {
        return 128;
    }
    return WEXITSTATUS(status);
}

static bool contains_case_insensitive(const char *text, const char *fragment)
{
    size_t fragment_length = strlen(fragment);
    const unsigned char *cursor = (const unsigned char *)text;

    if (fragment_length == 0U) {
        return true;
    }
    while (*cursor != 0U) {
        size_t index;
        for (index = 0U; index < fragment_length; ++index) {
            unsigned char actual = cursor[index];
            unsigned char expected = (unsigned char)fragment[index];
            if (actual == 0U || tolower(actual) != tolower(expected)) {
                break;
            }
        }
        if (index == fragment_length) {
            return true;
        }
        ++cursor;
    }
    return false;
}

static bool has_forbidden_dependency(const char *text, const char *const fragments[],
                                     size_t fragment_count, const char **fragment_out)
{
    size_t index;
    for (index = 0U; index < fragment_count; ++index) {
        if (strstr(text, fragments[index]) != NULL) {
            *fragment_out = fragments[index];
            return true;
        }
    }
    return false;
}

static int validate_binary(const char *path)
{
    struct stat metadata;
    CapturedOutput header = {NULL, 0U, 0U};
    CapturedOutput program = {NULL, 0U, 0U};
    CapturedOutput dynamic = {NULL, 0U, 0U};
    char *header_args[] = {(char *)"readelf", (char *)"-h", (char *)path, NULL};
    char *program_args[] = {(char *)"readelf", (char *)"-l", (char *)path, NULL};
    char *dynamic_args[] = {(char *)"readelf", (char *)"-d", (char *)path, NULL};
    const char *const forbidden[] = {
        "libc.so.6", "ld-linux", "libpthread.so.0", "libstdc++.so.6", "GLIBC_"
    };
    const char *found = NULL;
    const char *text_parts[3];
    size_t index;
    int result = 1;

    if (stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        (void)fprintf(stderr, "ERROR: missing binary: %s\n", path);
        return 1;
    }
    if (run_and_capture(header_args, &header) != 0 ||
        run_and_capture(program_args, &program) != 0 ||
        run_and_capture(dynamic_args, &dynamic) != 0) {
        (void)fprintf(stderr, "ERROR: readelf failed: %s\n",
                      dynamic.text != NULL ? dynamic.text :
                      (program.text != NULL ? program.text :
                       (header.text != NULL ? header.text : "could not execute readelf")));
        goto cleanup;
    }
    text_parts[0] = header.text;
    text_parts[1] = program.text;
    text_parts[2] = dynamic.text;
    if (!contains_case_insensitive(header.text, "Class: ELF64") ||
        !contains_case_insensitive(header.text, "little endian") ||
        !contains_case_insensitive(header.text, "Machine: AArch64")) {
        (void)fprintf(stderr, "ERROR: ELF is not native aarch64 (ELF64, little endian, AArch64 required)\n");
        goto cleanup;
    }
    if (strstr(program.text, "/system/bin/linker64") == NULL) {
        (void)fprintf(stderr, "ERROR: ELF does not use the bionic aarch64 interpreter /system/bin/linker64\n");
        goto cleanup;
    }
    if (strstr(dynamic.text, "libc.so") == NULL) {
        (void)fprintf(stderr, "ERROR: ELF is not dynamically linked against bionic libc.so\n");
        goto cleanup;
    }
    for (index = 0U; index < sizeof(text_parts) / sizeof(text_parts[0]); ++index) {
        if (has_forbidden_dependency(text_parts[index], forbidden,
                                     sizeof(forbidden) / sizeof(forbidden[0]), &found)) {
            (void)fprintf(stderr, "ERROR: glibc/host dependency detected: %s\n", found);
            goto cleanup;
        }
    }
    (void)printf("Termux ELF boundary: OK (%s)\n", path);
    result = 0;

cleanup:
    captured_free(&header);
    captured_free(&program);
    captured_free(&dynamic);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "usage: validate_termux_elf BINARY\n");
        return 2;
    }
    return validate_binary(argv[1]);
}
