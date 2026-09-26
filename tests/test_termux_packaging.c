#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../tools/milena_sha256.h"

#define PATH_CAPACITY 4096U
#define OUTPUT_CAPACITY 16384U
#define RECIPE_PATH "packaging/termux-packages/milena/build.sh"
#define ARTIFACT_VALIDATOR "scripts/validate_termux_artifact.py"
#define RECIPE_VALIDATOR "./tools/check_repository_contracts"

static int failures;

static void report_failure(const char *label, const char *detail)
{
    (void)fprintf(stderr, "FAIL %s: %s\n", label, detail);
    ++failures;
}

static bool join_path(char output[PATH_CAPACITY], const char *base, const char *suffix)
{
    int length = snprintf(output, PATH_CAPACITY, "%s/%s", base, suffix);
    if (length < 0 || (size_t)length >= PATH_CAPACITY) {
        return false;
    }
    return true;
}

static bool ensure_directory(const char *path)
{
    struct stat status;
    if (mkdir(path, (mode_t)0755) == 0) {
        return true;
    }
    if (errno != EEXIST || stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
        return false;
    }
    return true;
}

static bool make_directories(const char *path)
{
    char copy[PATH_CAPACITY];
    size_t length = strlen(path);
    size_t index;
    if (length == 0U || length >= sizeof(copy)) {
        return false;
    }
    memcpy(copy, path, length + 1U);
    for (index = 1U; index < length; ++index) {
        if (copy[index] == '/') {
            copy[index] = '\0';
            if (copy[0] != '\0' && !ensure_directory(copy)) {
                return false;
            }
            copy[index] = '/';
        }
    }
    return ensure_directory(copy);
}

static bool write_bytes(const char *path, const void *data, size_t length, mode_t mode)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (descriptor < 0) {
        return false;
    }
    while (written < length) {
        ssize_t count = write(descriptor, bytes + written, length - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            (void)close(descriptor);
            return false;
        }
        written += (size_t)count;
    }
    {
        bool mode_ok = fchmod(descriptor, mode) == 0;
        bool close_ok = close(descriptor) == 0;
        return mode_ok && close_ok;
    }
}

static bool write_text(const char *path, const char *text)
{
    return write_bytes(path, text, strlen(text), (mode_t)0644);
}

static char *read_text(const char *path)
{
    FILE *file = fopen(path, "rb");
    long end_position;
    size_t length;
    char *contents;
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (end_position = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    length = (size_t)end_position;
    if ((long)length != end_position || length == SIZE_MAX) {
        (void)fclose(file);
        return NULL;
    }
    contents = (char *)malloc(length + 1U);
    if (contents == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(contents, 1U, length, file) != length) {
        free(contents);
        (void)fclose(file);
        return NULL;
    }
    contents[length] = '\0';
    if (fclose(file) != 0) {
        free(contents);
        return NULL;
    }
    return contents;
}

static char *replace_once(const char *source, const char *needle, const char *replacement)
{
    const char *match = strstr(source, needle);
    size_t source_length = strlen(source);
    size_t needle_length = strlen(needle);
    size_t replacement_length = strlen(replacement);
    size_t prefix_length;
    size_t suffix_length;
    size_t result_length;
    char *result;
    if (match == NULL || needle_length == 0U) {
        return NULL;
    }
    prefix_length = (size_t)(match - source);
    suffix_length = source_length - prefix_length - needle_length;
    if (prefix_length > SIZE_MAX - replacement_length ||
        prefix_length + replacement_length > SIZE_MAX - suffix_length) {
        return NULL;
    }
    result_length = prefix_length + replacement_length + suffix_length;
    if (result_length == SIZE_MAX) {
        return NULL;
    }
    result = (char *)malloc(result_length + 1U);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, source, prefix_length);
    memcpy(result + prefix_length, replacement, replacement_length);
    memcpy(result + prefix_length + replacement_length,
           match + needle_length, suffix_length);
    result[result_length] = '\0';
    return result;
}

static int child_status_code(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 255;
}

static int run_capture(char *const arguments[], char output[OUTPUT_CAPACITY])
{
    int descriptors[2];
    pid_t child;
    int status = 0;
    size_t used = 0U;
    char buffer[2048];
    output[0] = '\0';
    if (pipe(descriptors) != 0) {
        (void)snprintf(output, OUTPUT_CAPACITY, "pipe failed: %s", strerror(errno));
        return 255;
    }
    child = fork();
    if (child < 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        (void)snprintf(output, OUTPUT_CAPACITY, "fork failed: %s", strerror(errno));
        return 255;
    }
    if (child == 0) {
        (void)close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0 ||
            dup2(descriptors[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(descriptors[1]);
        execvp(arguments[0], arguments);
        (void)fprintf(stderr, "could not execute %s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    (void)close(descriptors[1]);
    for (;;) {
        ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        if (used < OUTPUT_CAPACITY - 1U) {
            size_t available = OUTPUT_CAPACITY - 1U - used;
            size_t copied = (size_t)count < available ? (size_t)count : available;
            memcpy(output + used, buffer, copied);
            used += copied;
            output[used] = '\0';
        }
    }
    (void)close(descriptors[0]);
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return 255;
        }
    }
    return child_status_code(status);
}

static void expect_process(const char *label, bool expect_success, char *const arguments[])
{
    char output[OUTPUT_CAPACITY];
    int status = run_capture(arguments, output);
    bool succeeded = status == 0;
    if (succeeded != expect_success) {
        (void)fprintf(stderr, "FAIL %s: expected %s, got exit status %d\n%s",
                      label, expect_success ? "success" : "failure", status, output);
        if (output[0] != '\0' && output[strlen(output) - 1U] != '\n') {
            (void)fputc('\n', stderr);
        }
        ++failures;
    }
}

static bool wait_for_child(pid_t child, int *exit_code)
{
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return false;
    }
    *exit_code = child_status_code(status);
    return true;
}

static int run_to_file(char *const arguments[], const char *output_path)
{
    pid_t child = fork();
    int exit_code;
    if (child < 0) {
        return 255;
    }
    if (child == 0) {
        int descriptor = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)0644);
        if (descriptor < 0 || dup2(descriptor, STDOUT_FILENO) < 0) {
            _exit(126);
        }
        (void)close(descriptor);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    if (!wait_for_child(child, &exit_code)) {
        return 255;
    }
    return exit_code;
}

static bool executable_on_path(const char *program)
{
    const char *path = getenv("PATH");
    const char *cursor;
    if (path == NULL) {
        return false;
    }
    cursor = path;
    while (*cursor != '\0') {
        const char *separator = strchr(cursor, ':');
        size_t length = separator == NULL ? strlen(cursor) : (size_t)(separator - cursor);
        char candidate[PATH_CAPACITY];
        int written;
        if (length == 0U) {
            written = snprintf(candidate, sizeof(candidate), "./%s", program);
        } else {
            char directory[PATH_CAPACITY];
            if (length >= sizeof(directory)) {
                return false;
            }
            memcpy(directory, cursor, length);
            directory[length] = '\0';
            written = snprintf(candidate, sizeof(candidate), "%s/%s", directory, program);
        }
        if (written >= 0 && (size_t)written < sizeof(candidate) && access(candidate, X_OK) == 0) {
            return true;
        }
        if (separator == NULL) {
            break;
        }
        cursor = separator + 1;
    }
    return false;
}

static bool make_temp_directory(char output[PATH_CAPACITY], const char *prefix)
{
    const char *base = getenv("TMPDIR");
    char absolute_base[PATH_CAPACITY];
    int length;
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    if (base[0] != '/') {
        char current_directory[PATH_CAPACITY];
        if (getcwd(current_directory, sizeof(current_directory)) == NULL) {
            return false;
        }
        length = snprintf(absolute_base, sizeof(absolute_base), "%s/%s", current_directory, base);
        if (length < 0 || (size_t)length >= sizeof(absolute_base)) {
            return false;
        }
        base = absolute_base;
    }
    length = snprintf(output, PATH_CAPACITY, "%s/%s-XXXXXX", base, prefix);
    if (length < 0 || (size_t)length >= PATH_CAPACITY) {
        return false;
    }
    return mkdtemp(output) != NULL;
}

static bool remove_tree(const char *path)
{
    struct stat status;
    if (lstat(path, &status) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(status.st_mode)) {
        return unlink(path) == 0;
    }
    {
        DIR *directory = opendir(path);
        struct dirent *entry;
        if (directory == NULL) {
            return false;
        }
        while ((entry = readdir(directory)) != NULL) {
            char child_path[PATH_CAPACITY];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (!join_path(child_path, path, entry->d_name) || !remove_tree(child_path)) {
                (void)closedir(directory);
                return false;
            }
        }
        if (closedir(directory) != 0) {
            return false;
        }
    }
    return rmdir(path) == 0;
}

static void check_recipe(const char *label, const char *recipe,
                         bool expect_success, const char *const extra[], size_t extra_count)
{
    char *arguments[12];
    size_t index;
    if (extra_count > 8U) {
        report_failure(label, "too many recipe-validator arguments in the test");
        return;
    }
    arguments[0] = (char *)RECIPE_VALIDATOR;
    arguments[1] = (char *)"termux-recipe";
    arguments[2] = (char *)recipe;
    for (index = 0U; index < extra_count; ++index) {
        arguments[index + 3U] = (char *)extra[index];
    }
    arguments[extra_count + 3U] = NULL;
    expect_process(label, expect_success, arguments);
}

static bool write_replacement(const char *base, const char *needle,
                              const char *replacement, const char *path)
{
    char *content = replace_once(base, needle, replacement);
    bool written;
    if (content == NULL) {
        return false;
    }
    written = write_text(path, content);
    free(content);
    return written;
}

static char *file_uri(const char *absolute_path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t path_length = strlen(absolute_path);
    size_t index;
    size_t encoded_length = 0U;
    char *uri;
    size_t cursor = 0U;
    for (index = 0U; index < path_length; ++index) {
        unsigned char value = (unsigned char)absolute_path[index];
        bool safe = (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
                    (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
                    (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
                    value == (unsigned char)'/' || value == (unsigned char)'-' ||
                    value == (unsigned char)'_' || value == (unsigned char)'.' ||
                    value == (unsigned char)'~';
        encoded_length += safe ? 1U : 3U;
    }
    if (encoded_length > SIZE_MAX - 8U) {
        return NULL;
    }
    uri = (char *)malloc(encoded_length + 8U);
    if (uri == NULL) {
        return NULL;
    }
    memcpy(uri, "file://", 7U);
    cursor = 7U;
    for (index = 0U; index < path_length; ++index) {
        unsigned char value = (unsigned char)absolute_path[index];
        bool safe = (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
                    (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
                    (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
                    value == (unsigned char)'/' || value == (unsigned char)'-' ||
                    value == (unsigned char)'_' || value == (unsigned char)'.' ||
                    value == (unsigned char)'~';
        if (safe) {
            uri[cursor++] = (char)value;
        } else {
            uri[cursor++] = '%';
            uri[cursor++] = hex[value >> 4U];
            uri[cursor++] = hex[value & 0x0fU];
        }
    }
    uri[cursor] = '\0';
    return uri;
}

static void test_recipe_validator(const char *temporary_root)
{
    char recipe_text_path[PATH_CAPACITY];
    char unterminated_path[PATH_CAPACITY];
    char invalid_sha_path[PATH_CAPACITY];
    char debian_path[PATH_CAPACITY];
    char official_dir[PATH_CAPACITY];
    char official_script[PATH_CAPACITY];
    char archive_path[PATH_CAPACITY];
    char archive_dir[PATH_CAPACITY];
    char fetch_path[PATH_CAPACITY];
    char mismatch_path[PATH_CAPACITY];
    const char *sha_line = "TERMUX_PKG_SHA256=56e189bbd1e89aa25a7e8588e0606f0ea42d3bf5f1086fcfa3442d632d571153";
    char *base = read_text(RECIPE_PATH);
    if (base == NULL) {
        report_failure("recipe fixture", "could not read candidate Termux recipe");
        return;
    }
    check_recipe("candidate recipe", RECIPE_PATH, true, NULL, 0U);

    if (!join_path(recipe_text_path, temporary_root, "missing-sha.sh") ||
        !join_path(unterminated_path, temporary_root, "unterminated-sha.sh") ||
        !join_path(invalid_sha_path, temporary_root, "invalid-sha.sh") ||
        !join_path(debian_path, temporary_root, "debian-path.sh")) {
        report_failure("recipe fixtures", "temporary path is too long");
        free(base);
        return;
    }
    {
        char replacement[256];
        int length = snprintf(replacement, sizeof(replacement), "TERMUX_PKG_SHA256=\n%s", sha_line);
        if (length < 0 || (size_t)length >= sizeof(replacement) ||
            !write_replacement(base, sha_line, replacement, recipe_text_path)) {
            report_failure("empty SHA duplicate fixture", "could not write fixture");
        } else {
            check_recipe("empty SHA assignment followed by valid duplicate", recipe_text_path, true, NULL, 0U);
        }
    }
    {
        char replacement[256];
        int length = snprintf(replacement, sizeof(replacement), "TERMUX_PKG_SHA256=\"unterminated\n%s", sha_line);
        if (length < 0 || (size_t)length >= sizeof(replacement) ||
            !write_replacement(base, sha_line, replacement, unterminated_path)) {
            report_failure("unterminated SHA duplicate fixture", "could not write fixture");
        } else {
            check_recipe("unterminated SHA quote followed by valid duplicate", unterminated_path, true, NULL, 0U);
        }
    }
    if (!write_replacement(base, sha_line, "TERMUX_PKG_SHA256=\n", invalid_sha_path)) {
        report_failure("invalid SHA fixture", "could not write fixture");
    } else {
        check_recipe("missing SHA value", invalid_sha_path, false, NULL, 0U);
    }
    if (!write_replacement(base, "$TERMUX_PREFIX/bin/milena", "/usr/bin/milena", debian_path)) {
        report_failure("Debian path fixture", "could not write fixture");
    } else {
        check_recipe("Debian install path", debian_path, false, NULL, 0U);
    }

    if (!join_path(official_dir, temporary_root, "official") ||
        !join_path(official_script, official_dir, "build-package.sh") ||
        !make_directories(official_dir)) {
        report_failure("official checkout fixtures", "could not create fixture directory");
    } else {
        const char *missing_args[] = {"--official-dir", official_dir};
        const char *equals_args[1];
        check_recipe("missing official build-package.sh", RECIPE_PATH, false, missing_args, 2U);
        if (!write_text(official_script, "#!/bin/sh\nexit 0\n") || chmod(official_script, (mode_t)0644) != 0) {
            report_failure("official build-package.sh fixture", "could not create non-executable fixture");
        } else {
            check_recipe("non-executable official build-package.sh", RECIPE_PATH, false, missing_args, 2U);
            if (chmod(official_script, (mode_t)0755) != 0) {
                report_failure("official build-package.sh executable bit", "chmod failed");
            } else {
                check_recipe("executable official build-package.sh", RECIPE_PATH, true, missing_args, 2U);
                {
                    char option[PATH_CAPACITY + 32U];
                    int length = snprintf(option, sizeof(option), "--official-dir=%s", official_dir);
                    if (length < 0 || (size_t)length >= sizeof(option)) {
                        report_failure("official directory equals argument", "option path is too long");
                    } else {
                        equals_args[0] = option;
                        check_recipe("official directory equals form", RECIPE_PATH, true, equals_args, 1U);
                    }
                }
            }
        }
    }

    if (executable_on_path("curl")) {
        static const char payload[] = "local fixture for optional recipe SHA256 fetch\n";
        static const char original_url[] = "TERMUX_PKG_SRCURL=\"https://github.com/46Neon/Milena/archive/refs/tags/v${TERMUX_PKG_VERSION}.tar.gz\"";
        char digest[65];
        char url_assignment[PATH_CAPACITY * 3U + 64U];
        char sha_assignment[96];
        char *uri;
        char *url_template;
        char *with_url;
        char *with_digest;
        const char *fetch_args[] = {"--fetch"};
        if (!join_path(archive_dir, temporary_root, "archive/refs/tags") ||
            !make_directories(archive_dir) ||
            !join_path(archive_path, archive_dir, "v0.2.0.tar.gz") ||
            !write_bytes(archive_path, payload, sizeof(payload) - 1U, (mode_t)0644) ||
            milena_sha256_file(archive_path, digest, NULL) != 0) {
            report_failure("local fetch archive fixture", "could not create or hash fixture");
        } else {
            uri = file_uri(archive_path);
            url_template = uri == NULL ? NULL : replace_once(uri, "v0.2.0.tar.gz", "v${TERMUX_PKG_VERSION}.tar.gz");
            if (uri == NULL || url_template == NULL) {
                report_failure("local fetch archive URL", "could not construct a versioned file URI");
            } else {
                int url_length = snprintf(url_assignment, sizeof(url_assignment),
                                          "TERMUX_PKG_SRCURL=\"%s\"", url_template);
                int sha_length = snprintf(sha_assignment, sizeof(sha_assignment),
                                         "TERMUX_PKG_SHA256=\"%s\"", digest);
                if (url_length < 0 || (size_t)url_length >= sizeof(url_assignment) ||
                    sha_length < 0 || (size_t)sha_length >= sizeof(sha_assignment)) {
                    report_failure("local fetch recipe", "replacement value is too long");
                } else {
                    with_url = replace_once(base, original_url, url_assignment);
                    with_digest = with_url == NULL ? NULL : replace_once(with_url, sha_line, sha_assignment);
                    if (with_url == NULL || with_digest == NULL ||
                        !join_path(fetch_path, temporary_root, "file-fetch.sh") ||
                        !write_text(fetch_path, with_digest)) {
                        report_failure("local fetch recipe", "could not prepare recipe fixture");
                    } else {
                        check_recipe("local file URL with matching SHA", fetch_path, true, fetch_args, 1U);
                        if (!join_path(mismatch_path, temporary_root, "file-fetch-mismatch.sh")) {
                            report_failure("mismatched fetch fixture", "temporary path is too long");
                        } else {
                            char zero_digest[65];
                            char zero_assignment[96];
                            char *mismatch_source;
                            memset(zero_digest, '0', 64U);
                            zero_digest[64] = '\0';
                            (void)snprintf(zero_assignment, sizeof(zero_assignment),
                                           "TERMUX_PKG_SHA256=\"%s\"", zero_digest);
                            mismatch_source = replace_once(with_digest, sha_assignment, zero_assignment);
                            if (mismatch_source == NULL || !write_text(mismatch_path, mismatch_source)) {
                                report_failure("mismatched fetch fixture", "could not write fixture");
                            } else {
                                check_recipe("local file URL with mismatched SHA", mismatch_path, false, fetch_args, 1U);
                            }
                            free(mismatch_source);
                        }
                    }
                    free(with_url);
                    free(with_digest);
                }
            }
            free(uri);
            free(url_template);
        }
    }
    free(base);
}

static bool copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    FILE *output;
    unsigned char buffer[8192];
    size_t count;
    if (input == NULL) {
        return false;
    }
    output = fopen(destination, "wb");
    if (output == NULL) {
        (void)fclose(input);
        return false;
    }
    while ((count = fread(buffer, 1U, sizeof(buffer), input)) != 0U) {
        if (fwrite(buffer, 1U, count, output) != count) {
            (void)fclose(input);
            (void)fclose(output);
            return false;
        }
    }
    {
        bool input_ok = ferror(input) == 0;
        bool input_close_ok = fclose(input) == 0;
        bool output_close_ok = fclose(output) == 0;
        return input_ok && input_close_ok && output_close_ok;
    }
}

static bool create_package(const char *directory, const char *architecture,
                           bool debian_path, char package_path[PATH_CAPACITY])
{
    char stage[PATH_CAPACITY];
    char binary_directory[PATH_CAPACITY];
    char binary_path[PATH_CAPACITY];
    char doc_directory[PATH_CAPACITY];
    char readme_path[PATH_CAPACITY];
    char control_directory[PATH_CAPACITY];
    char control_path[PATH_CAPACITY];
    char control_text[512];
    char *arguments[7];
    int previous_failures = failures;
    int length;
    static const char binary_fixture[] = "fixture, not a compiled Termux binary\n";
    static const char readme_fixture[] = "fixture documentation\n";
    if (!join_path(stage, directory, "stage") || !make_directories(stage)) {
        return false;
    }
    if (!join_path(binary_directory, stage,
                   debian_path ? "usr/bin" : "data/data/com.termux/files/usr/bin") ||
        !make_directories(binary_directory) || !join_path(binary_path, binary_directory, "milena") ||
        !write_bytes(binary_path, binary_fixture, sizeof(binary_fixture) - 1U, (mode_t)0755)) {
        return false;
    }
    if (!join_path(doc_directory, stage, "data/data/com.termux/files/usr/share/doc/milena") ||
        !make_directories(doc_directory) || !join_path(readme_path, doc_directory, "README.md") ||
        !write_text(readme_path, readme_fixture) || !join_path(control_directory, stage, "DEBIAN") ||
        !make_directories(control_directory) || !join_path(control_path, control_directory, "control")) {
        return false;
    }
    length = snprintf(control_text, sizeof(control_text),
                      "Package: milena\nVersion: 0.2.0\nArchitecture: %s\n"
                      "Maintainer: Milena Fixture <fixture@example.invalid>\nDescription: fixture\n",
                      architecture);
    if (length < 0 || (size_t)length >= sizeof(control_text) ||
        !write_bytes(control_path, control_text, (size_t)length, (mode_t)0644) ||
        !join_path(package_path, directory, "milena_0.2.0_aarch64.deb")) {
        return false;
    }
    arguments[0] = (char *)"dpkg-deb";
    arguments[1] = (char *)"--build";
    arguments[2] = (char *)"--root-owner-group";
    arguments[3] = stage;
    arguments[4] = package_path;
    arguments[5] = NULL;
    expect_process("dpkg-deb fixture build", true, arguments);
    return failures == previous_failures;
}

static void run_artifact_validator(const char *label, const char *package,
                                   bool expect_success,
                                   const char *const extra[], size_t extra_count)
{
    char *arguments[12];
    size_t index;
    if (extra_count > 7U) {
        report_failure(label, "too many artifact-validator arguments in the test");
        return;
    }
    arguments[0] = (char *)"python3";
    arguments[1] = (char *)ARTIFACT_VALIDATOR;
    arguments[2] = (char *)package;
    for (index = 0U; index < extra_count; ++index) {
        arguments[index + 3U] = (char *)extra[index];
    }
    arguments[extra_count + 3U] = NULL;
    expect_process(label, expect_success, arguments);
}

static void test_termux_elf_validator(const char *temporary_root)
{
    static const char fake_readelf[] =
        "#!/bin/sh\n"
        "mode=${MILENA_FAKE_READELF_MODE:-valid}\n"
        "case \"$1\" in\n"
        "  -h)\n"
        "    case \"$mode\" in\n"
        "      readelf-failure) exit 1 ;;\n"
        "      wrong-class) printf 'Class: ELF32\\nData: little endian\\nMachine: AArch64\\n' ;;\n"
        "      wrong-endian) printf 'Class: ELF64\\nData: big endian\\nMachine: AArch64\\n' ;;\n"
        "      wrong-machine) printf 'Class: ELF64\\nData: little endian\\nMachine: x86-64\\n' ;;\n"
        "      *) printf 'Class: ELF64\\nData: little endian\\nMachine: AArch64\\n' ;;\n"
        "    esac ;;\n"
        "  -l)\n"
        "    case \"$mode\" in\n"
        "      wrong-interpreter) printf 'Requesting program interpreter: /lib64/ld-linux-x86-64.so.2\\n' ;;\n"
        "      forbidden-ld-linux) printf 'Requesting program interpreter: /system/bin/linker64\\nld-linux marker\\n' ;;\n"
        "      *) printf 'Requesting program interpreter: /system/bin/linker64\\n' ;;\n"
        "    esac ;;\n"
        "  -d)\n"
        "    case \"$mode\" in\n"
        "      missing-libc) printf 'Shared library: [libm.so]\\n' ;;\n"
        "      forbidden-libc6) printf 'Shared library: [libc.so]\\nShared library: [libc.so.6]\\n' ;;\n"
        "      forbidden-libpthread) printf 'Shared library: [libc.so]\\nShared library: [libpthread.so.0]\\n' ;;\n"
        "      forbidden-libstdcxx) printf 'Shared library: [libc.so]\\nShared library: [libstdc++.so.6]\\n' ;;\n"
        "      forbidden-glibc-version) printf 'Shared library: [libc.so]\\nSymbol: GLIBC_2.31\\n' ;;\n"
        "      *) printf 'Shared library: [libc.so]\\n' ;;\n"
        "    esac ;;\n"
        "  *) exit 2 ;;\n"
        "esac\n";
    static const struct {
        const char *mode;
        const char *label;
        bool succeeds;
    } cases[] = {
        {"valid", "accept representative Bionic AArch64 ELF", true},
        {"wrong-class", "reject non-ELF64 image", false},
        {"wrong-endian", "reject big-endian image", false},
        {"wrong-machine", "reject non-AArch64 image", false},
        {"wrong-interpreter", "reject non-Bionic interpreter", false},
        {"missing-libc", "reject image without Bionic libc", false},
        {"forbidden-libc6", "reject glibc libc dependency", false},
        {"forbidden-ld-linux", "reject glibc dynamic loader marker", false},
        {"forbidden-libpthread", "reject glibc pthread dependency", false},
        {"forbidden-libstdcxx", "reject host libstdc++ dependency", false},
        {"forbidden-glibc-version", "reject GLIBC symbol version", false},
        {"readelf-failure", "fail closed when readelf fails", false}
    };
    char mock_dir[PATH_CAPACITY];
    char readelf_path[PATH_CAPACITY];
    char fake_binary[PATH_CAPACITY];
    char missing_binary[PATH_CAPACITY];
    char new_path[PATH_CAPACITY * 2U];
    const char *old_path = getenv("PATH");
    char *saved_path = old_path == NULL ? NULL : strdup(old_path);
    size_t index;
    int length;

    if (!join_path(mock_dir, temporary_root, "fake-bin") ||
        !join_path(readelf_path, mock_dir, "readelf") ||
        !join_path(fake_binary, temporary_root, "fake-bionic-elf") ||
        !join_path(missing_binary, temporary_root, "missing-elf") ||
        !make_directories(mock_dir) || !write_text(readelf_path, fake_readelf) ||
        chmod(readelf_path, (mode_t)0755) != 0 || !write_text(fake_binary, "fixture bytes\n")) {
        report_failure("ELF validator fixtures", "could not prepare mocked readelf and binary");
        free(saved_path);
        return;
    }
    length = snprintf(new_path, sizeof(new_path), "%s:%s", mock_dir,
                      saved_path == NULL ? "" : saved_path);
    if (length < 0 || (size_t)length >= sizeof(new_path) ||
        setenv("PATH", new_path, 1) != 0) {
        report_failure("ELF validator PATH fixture", "could not prepend mocked readelf");
        free(saved_path);
        return;
    }
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char *arguments[3];
        if (setenv("MILENA_FAKE_READELF_MODE", cases[index].mode, 1) != 0) {
            report_failure(cases[index].label, "could not select mocked readelf case");
            continue;
        }
        arguments[0] = (char *)"./tools/validate_termux_elf";
        arguments[1] = fake_binary;
        arguments[2] = NULL;
        expect_process(cases[index].label, cases[index].succeeds, arguments);
    }
    {
        char *arguments[3];
        arguments[0] = (char *)"./tools/validate_termux_elf";
        arguments[1] = missing_binary;
        arguments[2] = NULL;
        expect_process("reject missing ELF file", false, arguments);
    }
    if (saved_path == NULL) {
        (void)unsetenv("PATH");
    } else if (setenv("PATH", saved_path, 1) != 0) {
        report_failure("restore PATH", "could not restore original PATH after ELF fixtures");
    }
    (void)unsetenv("MILENA_FAKE_READELF_MODE");
    free(saved_path);
}

static void test_artifact_boundary(const char *temporary_root)
{
    char valid_dir[PATH_CAPACITY];
    char package[PATH_CAPACITY];
    char checksum_path[PATH_CAPACITY];
    char digest[65];
    char checksum_text[PATH_CAPACITY + 96U];
    char wrong_arch_dir[PATH_CAPACITY];
    char wrong_arch_package[PATH_CAPACITY];
    char wrong_path_dir[PATH_CAPACITY];
    char wrong_path_package[PATH_CAPACITY];
    char apt_root[PATH_CAPACITY];
    char pool_dir[PATH_CAPACITY];
    char copied_package[PATH_CAPACITY];
    char index_dir[PATH_CAPACITY];
    char packages_path[PATH_CAPACITY];
    char packages_gz_path[PATH_CAPACITY];
    char release_path[PATH_CAPACITY];
    char inrelease_path[PATH_CAPACITY];
    char release_signature_path[PATH_CAPACITY];
    char keyring_path[PATH_CAPACITY];
    char index_text[PATH_CAPACITY * 2U];
    char release_text[PATH_CAPACITY];
    char packages_digest[65];
    uint64_t package_size = 0U;
    uint64_t compressed_size = 0U;
    char *gzip_arguments[5];
    const char *checksum_args[2];
    const char *apt_args[2];
    if (!join_path(valid_dir, temporary_root, "valid") ||
        !create_package(valid_dir, "aarch64", false, package) ||
        milena_sha256_file(package, digest, &package_size) != 0 ||
        !join_path(checksum_path, temporary_root, "milena.sha256")) {
        report_failure("valid package fixture", "could not build or hash valid package");
        return;
    }
    {
        int length = snprintf(checksum_text, sizeof(checksum_text), "%s  %s\n", digest, package);
        if (length < 0 || (size_t)length >= sizeof(checksum_text) ||
            !write_text(checksum_path, checksum_text)) {
            report_failure("checksum fixture", "could not write checksum file");
        } else {
            checksum_args[0] = "--checksum";
            checksum_args[1] = checksum_path;
            run_artifact_validator("valid Termux package and checksum", package, true, checksum_args, 2U);
        }
    }
    if (!join_path(wrong_arch_dir, temporary_root, "wrong-arch") ||
        !create_package(wrong_arch_dir, "arm64", false, wrong_arch_package)) {
        report_failure("wrong architecture fixture", "could not build package");
    } else {
        run_artifact_validator("reject wrong package architecture", wrong_arch_package, false, NULL, 0U);
    }
    if (!join_path(wrong_path_dir, temporary_root, "wrong-path") ||
        !create_package(wrong_path_dir, "aarch64", true, wrong_path_package)) {
        report_failure("Debian path fixture", "could not build package");
    } else {
        run_artifact_validator("reject Debian filesystem path", wrong_path_package, false, NULL, 0U);
    }

    if (!join_path(apt_root, temporary_root, "apt") ||
        !join_path(pool_dir, apt_root, "pool/main/m/milena") || !make_directories(pool_dir) ||
        !join_path(copied_package, pool_dir, "milena_0.2.0_aarch64.deb") ||
        !copy_file(package, copied_package) || !join_path(index_dir, apt_root, "dists/stable/main/binary-aarch64") ||
        !make_directories(index_dir) || !join_path(packages_path, index_dir, "Packages") ||
        !join_path(packages_gz_path, index_dir, "Packages.gz") ||
        milena_sha256_file(copied_package, digest, &package_size) != 0) {
        report_failure("APT pool fixture", "could not create package pool");
        return;
    }
    {
        int length = snprintf(index_text, sizeof(index_text),
                              "Package: milena\nVersion: 0.2.0\nArchitecture: aarch64\n"
                              "Filename: pool/main/m/milena/milena_0.2.0_aarch64.deb\n"
                              "Size: %" PRIu64 "\nSHA256: %s\n",
                              package_size, digest);
        if (length < 0 || (size_t)length >= sizeof(index_text) || !write_text(packages_path, index_text)) {
            report_failure("APT Packages fixture", "could not write package index");
            return;
        }
    }
    gzip_arguments[0] = (char *)"gzip";
    gzip_arguments[1] = (char *)"-n";
    gzip_arguments[2] = (char *)"-c";
    gzip_arguments[3] = packages_path;
    gzip_arguments[4] = NULL;
    if (run_to_file(gzip_arguments, packages_gz_path) != 0 ||
        milena_sha256_file(packages_gz_path, packages_digest, &compressed_size) != 0) {
        report_failure("APT gzip fixture", "gzip or SHA-256 failed");
        return;
    }
    if (!join_path(release_path, apt_root, "dists/stable/Release") ||
        !join_path(inrelease_path, apt_root, "dists/stable/InRelease") ||
        !join_path(release_signature_path, apt_root, "dists/stable/Release.gpg") ||
        !join_path(keyring_path, apt_root, "milena-archive-keyring.asc")) {
        report_failure("APT release fixtures", "temporary path is too long");
        return;
    }
    {
        int length = snprintf(release_text, sizeof(release_text),
                              "Origin: Milena\nSuite: stable\nComponents: main\n"
                              "Architectures: aarch64\nSHA256:\n"
                              " %s %" PRIu64 " main/binary-aarch64/Packages.gz\n",
                              packages_digest, compressed_size);
        if (length < 0 || (size_t)length >= sizeof(release_text) ||
            !write_text(release_path, release_text) ||
            !write_text(inrelease_path, "fixture signature placeholder\n") ||
            !write_text(release_signature_path, "fixture signature placeholder\n") ||
            !write_text(keyring_path, "fixture key placeholder\n")) {
            report_failure("APT Release fixtures", "could not write APT metadata");
            return;
        }
    }
    apt_args[0] = "--apt-root";
    apt_args[1] = apt_root;
    run_artifact_validator("valid APT repository structure", package, true, apt_args, 2U);
}

int main(void)
{
    char recipe_root[PATH_CAPACITY];
    char artifact_root[PATH_CAPACITY];
    bool recipe_created;
    bool artifact_created;
    recipe_created = make_temp_directory(recipe_root, "milena-termux-recipe");
    artifact_created = make_temp_directory(artifact_root, "milena-termux-boundary");
    if (!recipe_created || !artifact_created) {
        (void)fprintf(stderr, "ERROR: could not create temporary fixture directories: %s\n", strerror(errno));
        if (recipe_created) {
            (void)remove_tree(recipe_root);
        }
        if (artifact_created) {
            (void)remove_tree(artifact_root);
        }
        return 1;
    }
    test_recipe_validator(recipe_root);
    test_termux_elf_validator(artifact_root);
    test_artifact_boundary(artifact_root);
    if (!remove_tree(recipe_root)) {
        report_failure("recipe fixture cleanup", "could not remove temporary directory");
    }
    if (!remove_tree(artifact_root)) {
        report_failure("artifact fixture cleanup", "could not remove temporary directory");
    }
    if (failures != 0) {
        (void)fprintf(stderr, "Termux packaging boundary tests: %d failure(s)\n", failures);
        return 1;
    }
    (void)puts("Termux packaging boundary tests: OK");
    return 0;
}
