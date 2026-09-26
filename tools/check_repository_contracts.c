/* ISO C17 repository contracts for source boundaries, docs, and Termux packaging. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "milena_sha256.h"

#define ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

typedef struct {
    const char *begin;
    const char *end;
} Span;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

static const char *const product_sources[] = {
    "analysis.c", "array.c", "arrow_ipc.c", "bytecode.c", "bytecode_compiler.c", "bytecode_data.c", "canonical_compiler.c", "common.c",
    "dataset.c", "entrypoints.c", "external_merge.c", "external_sort.c",
    "finance.c", "group_key_codec.c", "grouped_aggregate.c", "interpreter.c",
    "language_grouped_spill.c", "language_runtime.c", "logger.c", "main.c",
    "mergeable_aggregate.c", "metrics.c", "partition_executor.c", "partition_plan.c",
    "partition_protocol.c", "partition_protocol_reduce.c", "partition_reduce.c",
    "process_executor.c", "query_plan.c", "schema.c", "script.c", "source_reader.c",
    "spill_store.c", "sqlite_backend.c", "sst_advanced.c", "sst_contingency.c",
    "sst_correlation.c", "sst_dates.c", "sst_histogram.c", "sst_inference.c",
    "sst_model.c", "sst_normality.c", "sst_rates.c", "sst_report.c",
    "sst_report_advanced.c", "sst_stats.c", "stream.c", "table.c"
};
static const char *const language_sources[] = {
    "ast.c", "language_semantic.c", "lexer.c", "parser.c", "symbol_table.c"
};
static const char *const function_sources[] = {
    "function_parser.c", "symbol.c", "user_functions.c"
};
static const char *const experimental_sources[] = {
    "arena.c", "assembler.c", "compiler.c", "forest.c", "gc.c", "instructions.c",
    "ir.c", "module.c", "semantic.c", "temp_scope.c", "vm.c", "bytecode_native.c"
};

static void list_init(StringList *list)
{
    list->items = NULL;
    list->count = 0U;
    list->capacity = 0U;
}

static void list_add_n(StringList *list, const char *value, size_t length)
{
    char *copy;
    size_t next_capacity;
    char **next_items;

    if (list->count == list->capacity) {
        next_capacity = list->capacity == 0U ? 8U : list->capacity * 2U;
        if (next_capacity < list->capacity || next_capacity > ((size_t)-1) / sizeof(*list->items)) {
            (void)fprintf(stderr, "ERROR: capacity overflow while recording check results\n");
            exit(EXIT_FAILURE);
        }
        next_items = (char **)realloc(list->items, next_capacity * sizeof(*list->items));
        if (next_items == NULL) {
            (void)fprintf(stderr, "ERROR: out of memory while recording check results\n");
            exit(EXIT_FAILURE);
        }
        list->items = next_items;
        list->capacity = next_capacity;
    }
    if (length == (size_t)-1) {
        (void)fprintf(stderr, "ERROR: invalid string length\n");
        exit(EXIT_FAILURE);
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        (void)fprintf(stderr, "ERROR: out of memory while copying check result\n");
        exit(EXIT_FAILURE);
    }
    memcpy(copy, value, length);
    copy[length] = '\0';
    list->items[list->count] = copy;
    ++list->count;
}

static void list_add(StringList *list, const char *value)
{
    list_add_n(list, value, strlen(value));
}

static void list_addf(StringList *list, const char *format, ...)
{
    va_list arguments;
    va_list copy_arguments;
    int length;
    char *message;

    va_start(arguments, format);
    va_copy(copy_arguments, arguments);
    length = vsnprintf(NULL, 0U, format, copy_arguments);
    va_end(copy_arguments);
    if (length < 0) {
        va_end(arguments);
        (void)fprintf(stderr, "ERROR: cannot format check result\n");
        exit(EXIT_FAILURE);
    }
    message = (char *)malloc((size_t)length + 1U);
    if (message == NULL) {
        va_end(arguments);
        (void)fprintf(stderr, "ERROR: out of memory while formatting check result\n");
        exit(EXIT_FAILURE);
    }
    (void)vsnprintf(message, (size_t)length + 1U, format, arguments);
    va_end(arguments);
    list_add_n(list, message, (size_t)length);
    free(message);
}

static void list_free(StringList *list)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    list_init(list);
}

static int compare_strings(const void *left_pointer, const void *right_pointer)
{
    const char *const *left = (const char *const *)left_pointer;
    const char *const *right = (const char *const *)right_pointer;
    return strcmp(*left, *right);
}

static void list_sort(StringList *list)
{
    if (list->count > 1U) {
        qsort(list->items, list->count, sizeof(*list->items), compare_strings);
    }
}

static bool list_contains(const StringList *list, const char *value)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index], value) == 0) {
            return true;
        }
    }
    return false;
}

static void list_add_unique(StringList *list, const char *value)
{
    if (!list_contains(list, value)) {
        list_add(list, value);
    }
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    size_t bytes_read;
    char *contents;

    if (file == NULL) {
        (void)fprintf(stderr, "ERROR: required check input is missing or unreadable: %s\n", path);
        exit(EXIT_FAILURE);
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        (void)fprintf(stderr, "ERROR: cannot seek check input: %s\n", path);
        exit(EXIT_FAILURE);
    }
    length = ftell(file);
    if (length < 0L || fseek(file, 0L, SEEK_SET) != 0 || (unsigned long)length >= (unsigned long)((size_t)-1)) {
        (void)fclose(file);
        (void)fprintf(stderr, "ERROR: cannot determine check input size: %s\n", path);
        exit(EXIT_FAILURE);
    }
    contents = (char *)malloc((size_t)length + 1U);
    if (contents == NULL) {
        (void)fclose(file);
        (void)fprintf(stderr, "ERROR: out of memory while reading: %s\n", path);
        exit(EXIT_FAILURE);
    }
    bytes_read = fread(contents, 1U, (size_t)length, file);
    if (bytes_read != (size_t)length || ferror(file) != 0) {
        free(contents);
        (void)fclose(file);
        (void)fprintf(stderr, "ERROR: cannot read check input: %s\n", path);
        exit(EXIT_FAILURE);
    }
    contents[bytes_read] = '\0';
    if (fclose(file) != 0) {
        free(contents);
        (void)fprintf(stderr, "ERROR: cannot close check input: %s\n", path);
        exit(EXIT_FAILURE);
    }
    return contents;
}

static bool contains(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static bool contains_case_insensitive_ascii(const char *text, const char *needle)
{
    size_t needle_length = strlen(needle);
    const unsigned char *cursor = (const unsigned char *)text;

    if (needle_length == 0U) {
        return true;
    }
    while (*cursor != 0U) {
        size_t index;
        for (index = 0U; index < needle_length; ++index) {
            unsigned char actual = cursor[index];
            unsigned char expected = (unsigned char)needle[index];
            if (actual == 0U) {
                break;
            }
            if (actual >= (unsigned char)'A' && actual <= (unsigned char)'Z') {
                actual = (unsigned char)(actual + ((unsigned char)'a' - (unsigned char)'A'));
            }
            if (expected >= (unsigned char)'A' && expected <= (unsigned char)'Z') {
                expected = (unsigned char)(expected + ((unsigned char)'a' - (unsigned char)'A'));
            }
            if (actual != expected) {
                break;
            }
        }
        if (index == needle_length) {
            return true;
        }
        ++cursor;
    }
    return false;
}

static Span find_make_sources(const char *makefile)
{
    const char *line = makefile;
    const char *start = NULL;
    const char *end;

    while (*line != '\0') {
        const char *cursor = line;
        if (strncmp(cursor, "SOURCES", 7U) == 0) {
            cursor += 7;
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            if (*cursor == '=') {
                start = cursor + 1;
                break;
            }
        }
        line = strchr(line, '\n');
        if (line == NULL) {
            break;
        }
        ++line;
    }
    if (start == NULL) {
        return (Span){makefile, makefile};
    }
    line = start;
    while (*line != '\0') {
        const char *line_start = line;
        const char *cursor;
        while (line_start > start && line_start[-1] != '\n') {
            --line_start;
        }
        cursor = line_start;
        if (strncmp(cursor, "OBJECTS", 7U) == 0) {
            cursor += 7;
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            if (*cursor == '=') {
                return (Span){start, line_start};
            }
        }
        line = strchr(line, '\n');
        if (line == NULL) {
            break;
        }
        ++line;
    }
    end = makefile + strlen(makefile);
    return (Span){start, end};
}

static bool ascii_word(unsigned char value)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool make_has_source_token(Span sources, const char *name, bool restricted_name)
{
    const char *cursor = sources.begin;
    size_t name_length = strlen(name);

    while (cursor < sources.end) {
        const char *found = NULL;
        const char *path_begin;
        const char *token_end;
        const char *last_c = NULL;
        const char *scan;

        for (scan = cursor; (size_t)(sources.end - scan) >= 4U; ++scan) {
            if (memcmp(scan, "src/", 4U) == 0) {
                found = scan;
                break;
            }
        }
        if (found == NULL) {
            break;
        }
        path_begin = found + 4;
        token_end = path_begin;
        while (token_end < sources.end && *token_end != '\\' && !isspace((unsigned char)*token_end)) {
            ++token_end;
        }
        if (restricted_name) {
            const char *name_end = path_begin;
            while (name_end < token_end && ascii_word((unsigned char)*name_end)) {
                ++name_end;
            }
            if ((size_t)(name_end - path_begin) + 2U == name_length &&
                (size_t)(token_end - name_end) >= 2U && name_end[0] == '.' && name_end[1] == 'c' &&
                memcmp(path_begin, name, name_length) == 0) {
                return true;
            }
        } else {
            for (scan = path_begin; (size_t)(token_end - scan) >= 2U; ++scan) {
                if (scan[0] == '.' && scan[1] == 'c') {
                    last_c = scan;
                }
            }
            if (last_c != NULL && (size_t)(last_c - path_begin) == name_length &&
                memcmp(path_begin, name, name_length) == 0) {
                return true;
            }
        }
        cursor = token_end;
    }
    return false;
}

static bool is_experimental(const char *name)
{
    size_t index;
    for (index = 0U; index < ARRAY_COUNT(experimental_sources); ++index) {
        if (strcmp(name, experimental_sources[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool manifest_has(const char *name)
{
    size_t index;
    for (index = 0U; index < ARRAY_COUNT(product_sources); ++index) {
        if (strcmp(name, product_sources[index]) == 0) {
            return true;
        }
    }
    for (index = 0U; index < ARRAY_COUNT(language_sources); ++index) {
        if (strcmp(name, language_sources[index]) == 0) {
            return true;
        }
    }
    for (index = 0U; index < ARRAY_COUNT(function_sources); ++index) {
        if (strcmp(name, function_sources[index]) == 0) {
            return true;
        }
    }
    for (index = 0U; index < ARRAY_COUNT(experimental_sources); ++index) {
        if (strcmp(name, experimental_sources[index]) == 0) {
            return true;
        }
    }
    return false;
}

static void check_source_manifest(int argc, char **argv)
{
    StringList actual;
    StringList errors;
    StringList missing;
    StringList extra;
    int argument;
    size_t index;
    const char *const *groups[] = {product_sources, language_sources, function_sources, experimental_sources};
    const size_t group_counts[] = {ARRAY_COUNT(product_sources), ARRAY_COUNT(language_sources), ARRAY_COUNT(function_sources), ARRAY_COUNT(experimental_sources)};
    const char *const group_names[] = {"producto", "lenguaje", "funciones", "experimental"};
    size_t group;

    list_init(&actual);
    list_init(&errors);
    list_init(&missing);
    list_init(&extra);
    for (argument = 2; argument < argc; ++argument) {
        const char *slash = strrchr(argv[argument], '/');
        const char *name = slash == NULL ? argv[argument] : slash + 1;
        list_add_unique(&actual, name);
    }
    for (group = 0U; group < ARRAY_COUNT(groups); ++group) {
        for (index = 0U; index < group_counts[group]; ++index) {
            size_t other_group;
            for (other_group = group + 1U; other_group < ARRAY_COUNT(groups); ++other_group) {
                size_t other_index;
                for (other_index = 0U; other_index < group_counts[other_group]; ++other_index) {
                    if (strcmp(groups[group][index], groups[other_group][other_index]) == 0) {
                        list_addf(&errors, "%s: aparece en %s y %s", groups[group][index], group_names[group], group_names[other_group]);
                    }
                }
            }
        }
    }
    for (index = 0U; index < actual.count; ++index) {
        if (!manifest_has(actual.items[index])) {
            list_add(&missing, actual.items[index]);
        }
    }
    for (group = 0U; group < ARRAY_COUNT(groups); ++group) {
        for (index = 0U; index < group_counts[group]; ++index) {
            if (!list_contains(&actual, groups[group][index])) {
                list_add_unique(&extra, groups[group][index]);
            }
        }
    }
    list_sort(&missing);
    list_sort(&extra);
    if (missing.count != 0U) {
        size_t length = 0U;
        char *joined;
        for (index = 0U; index < missing.count; ++index) {
            length += strlen(missing.items[index]) + (index == 0U ? 0U : 2U);
        }
        joined = (char *)malloc(length + 1U);
        if (joined == NULL) {
            (void)fprintf(stderr, "ERROR: out of memory while formatting manifest mismatch\n");
            exit(EXIT_FAILURE);
        }
        joined[0] = '\0';
        for (index = 0U; index < missing.count; ++index) {
            if (index != 0U) {
                (void)strcat(joined, ", ");
            }
            (void)strcat(joined, missing.items[index]);
        }
        list_addf(&errors, "fuentes sin categoría: %s", joined);
        free(joined);
    }
    if (extra.count != 0U) {
        size_t length = 0U;
        char *joined;
        for (index = 0U; index < extra.count; ++index) {
            length += strlen(extra.items[index]) + (index == 0U ? 0U : 2U);
        }
        joined = (char *)malloc(length + 1U);
        if (joined == NULL) {
            (void)fprintf(stderr, "ERROR: out of memory while formatting manifest mismatch\n");
            exit(EXIT_FAILURE);
        }
        joined[0] = '\0';
        for (index = 0U; index < extra.count; ++index) {
            if (index != 0U) {
                (void)strcat(joined, ", ");
            }
            (void)strcat(joined, extra.items[index]);
        }
        list_addf(&errors, "fuentes declaradas pero inexistentes: %s", joined);
        free(joined);
    }
    if (errors.count != 0U) {
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&actual);
        list_free(&errors);
        list_free(&missing);
        list_free(&extra);
        exit(EXIT_FAILURE);
    }
    (void)printf("OK: %lu fuentes C clasificadas\n", (unsigned long)actual.count);
    (void)printf("  - producto: %lu\n", (unsigned long)ARRAY_COUNT(product_sources));
    (void)printf("  - lenguaje: %lu\n", (unsigned long)ARRAY_COUNT(language_sources));
    (void)printf("  - funciones: %lu\n", (unsigned long)ARRAY_COUNT(function_sources));
    (void)printf("  - experimental: %lu\n", (unsigned long)ARRAY_COUNT(experimental_sources));
    list_free(&actual);
    list_free(&errors);
    list_free(&missing);
    list_free(&extra);
}

static bool has_include(const char *source, const char *header, bool allow_angle)
{
    const char *cursor = source;
    size_t header_length = strlen(header);
    while ((cursor = strstr(cursor, "#include")) != NULL) {
        const char *name = cursor + strlen("#include");
        bool separated = false;
        while (isspace((unsigned char)*name)) {
            separated = true;
            ++name;
        }
        if (separated && (*name == '"' || (allow_angle && *name == '<'))) {
            ++name;
            if (strncmp(name, header, header_length) == 0 &&
                (name[header_length] == '"' || name[header_length] == '>')) {
                return true;
            }
        }
        cursor += strlen("#include");
    }
    return false;
}

static void check_compiler_boundary(int argc, char **argv)
{
    char *makefile = read_file("Makefile");
    Span sources = find_make_sources(makefile);
    StringList errors;
    size_t index;
    int argument;
    const char *const canonical[] = {
        "lexer.c", "parser.c", "ast.c", "language_semantic.c", "language_runtime.c",
        "canonical_compiler.c", "table.c", "dataset.c"
    };
    const char *const guarded_headers[] = {"compiler.h", "ir.h", "vm.h", "gc.h"};

    list_init(&errors);
    if (sources.begin == sources.end) {
        list_add(&errors, "Makefile no contiene una asignación SOURCES reconocible");
    }
    for (index = 0U; index < ARRAY_COUNT(experimental_sources); ++index) {
        if (make_has_source_token(sources, experimental_sources[index], true)) {
            list_addf(&errors, "módulo experimental enlazado en el binario oficial: %s", experimental_sources[index]);
        }
    }
    for (index = 0U; index < ARRAY_COUNT(canonical); ++index) {
        if (!make_has_source_token(sources, canonical[index], true)) {
            list_addf(&errors, "falta una fuente del pipeline canónico: %s", canonical[index]);
        }
    }
    for (argument = 2; argument < argc; ++argument) {
        const char *name;
        char *text;
        size_t header_index;
        bool linked;
        const char *slash = strrchr(argv[argument], '/');
        name = slash == NULL ? argv[argument] : slash + 1;
        linked = make_has_source_token(sources, name, true);
        if (!linked) {
            continue;
        }
        text = read_file(argv[argument]);
        for (header_index = 0U; header_index < ARRAY_COUNT(guarded_headers); ++header_index) {
            if (has_include(text, guarded_headers[header_index], true)) {
                list_addf(&errors, "fuente oficial %s incluye la pila experimental %s", name, guarded_headers[header_index]);
            }
        }
        free(text);
    }
    if (errors.count != 0U) {
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&errors);
        free(makefile);
        exit(EXIT_FAILURE);
    }
    (void)printf("Compiler/IR/VM boundary: OK (unfinished stack excluded; explicit typed-bytecode CLI remains bounded)\n");
    list_free(&errors);
    free(makefile);
}

static void check_experimental_isolation(int argc, char **argv)
{
    char *makefile = read_file("Makefile");
    Span sources = find_make_sources(makefile);
    StringList leaked;
    StringList errors;
    size_t index;
    int argument;
    static const char *const forbidden_headers[] = {
        "arena.h", "assembler.h", "compiler.h", "forest.h", "gc.h", "instructions.h",
        "ir.h", "module.h", "semantic.h", "temp_scope.h", "vm.h"
    };

    list_init(&leaked);
    list_init(&errors);
    if (sources.begin == sources.end) {
        (void)fprintf(stderr, "ERROR: no se pudo localizar SOURCES en Makefile\n");
        free(makefile);
        exit(EXIT_FAILURE);
    }
    for (index = 0U; index < ARRAY_COUNT(experimental_sources); ++index) {
        if (make_has_source_token(sources, experimental_sources[index], false)) {
            list_add_unique(&leaked, experimental_sources[index]);
        }
    }
    list_sort(&leaked);
    if (leaked.count != 0U) {
        size_t length = 0U;
        char *joined;
        for (index = 0U; index < leaked.count; ++index) {
            length += strlen(leaked.items[index]) + (index == 0U ? 0U : 2U);
        }
        joined = (char *)malloc(length + 1U);
        if (joined == NULL) {
            (void)fprintf(stderr, "ERROR: out of memory while formatting experimental module list\n");
            exit(EXIT_FAILURE);
        }
        joined[0] = '\0';
        for (index = 0U; index < leaked.count; ++index) {
            if (index != 0U) {
                (void)strcat(joined, ", ");
            }
            (void)strcat(joined, leaked.items[index]);
        }
        (void)fprintf(stderr, "ERROR: módulos experimentales enlazados en el producto: %s\n", joined);
        free(joined);
        list_free(&leaked);
        list_free(&errors);
        free(makefile);
        exit(EXIT_FAILURE);
    }
    for (argument = 2; argument < argc; ++argument) {
        const char *slash = strrchr(argv[argument], '/');
        const char *name = slash == NULL ? argv[argument] : slash + 1;
        size_t header_index;
        char *text;
        if (is_experimental(name)) {
            continue;
        }
        text = read_file(argv[argument]);
        for (header_index = 0U; header_index < ARRAY_COUNT(forbidden_headers); ++header_index) {
            char include_literal[128];
            int count = snprintf(include_literal, sizeof(include_literal), "#include \"%s\"", forbidden_headers[header_index]);
            if (count > 0 && (size_t)count < sizeof(include_literal) && contains(text, include_literal)) {
                list_addf(&errors, "%s: incluye %s", name, forbidden_headers[header_index]);
            }
        }
        free(text);
    }
    if (errors.count != 0U) {
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&leaked);
        list_free(&errors);
        free(makefile);
        exit(EXIT_FAILURE);
    }
    (void)printf("OK: %lu módulos experimentales aislados del producto\n", (unsigned long)ARRAY_COUNT(experimental_sources));
    list_free(&leaked);
    list_free(&errors);
    free(makefile);
}

static void need(bool condition, const char *message)
{
    if (!condition) {
        (void)fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

static char *require_substring(char *text, const char *marker, const char *message)
{
    char *found = strstr(text, marker);
    need(found != NULL, message);
    return found;
}

static bool token_boundary_match(Span text, const char *token)
{
    size_t token_length = strlen(token);
    const char *cursor;
    if (token_length == 0U || text.end < text.begin ||
        (size_t)(text.end - text.begin) < token_length) {
        return false;
    }
    for (cursor = text.begin; cursor <= text.end - token_length; ++cursor) {
        bool left_ok;
        bool right_ok;
        if (memcmp(cursor, token, token_length) != 0) {
            continue;
        }
        left_ok = cursor == text.begin || !ascii_word((unsigned char)cursor[-1]);
        right_ok = cursor + token_length == text.end ||
                   !ascii_word((unsigned char)cursor[token_length]);
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

static bool stream_standalone_main(const char *stream)
{
    const char *cursor = stream;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *where_stream = strstr(cursor, "stream");
        const char *where_main;
        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        if (where_stream == NULL || where_stream >= line_end) {
            cursor = *line_end == '\0' ? line_end : line_end + 1;
            continue;
        }
        where_main = strstr(where_stream, "main");
        if (where_main != NULL && where_main < line_end) {
            const char *after = where_main + 4;
            while (*after != '\0' && isspace((unsigned char)*after)) {
                ++after;
            }
            if (*after == '(') {
                return true;
            }
        }
        cursor = *line_end == '\0' ? line_end : line_end + 1;
    }
    return false;
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    (void)fclose(file);
    return true;
}

static void check_stream_architecture(void)
{
    char *ast = read_file("include/ast.h");
    char *parser = read_file("src/parser.c");
    char *runtime = read_file("src/language_runtime.c");
    char *planner = read_file("src/query_plan.c");
    char *plan_header = read_file("include/query_plan.h");
    char *make = read_file("Makefile");
    char *windows_build = read_file("packaging/windows/build.ps1");
    char *stream = read_file("src/stream.c");
    char *stream_header = read_file("include/stream.h");
    char *streaming_docs = read_file("docs/STREAMING_EXECUTION.md");
    char *spill_contract = read_file("docs/GROUPED_SPILL_CONTRACT.md");
    char *pr25_docs = read_file("docs/PR25_BIG_DATA_FOUNDATION.md");
    char *make_sources;
    char *manifest_make;
    char *spill_start;
    char *runtime_stream_start;
    char *runtime_stream_end;
    static const char *const typed_ast_markers[] = {
        "ASTStreamOperation", "stream_chunk_rows", "stream_record_limit",
        "stream_column_limit", "stream_group_limit", "stream_operation_from_token",
        "parse_stream_group", "AST_BLOQUE_AGRUPAR"
    };
    static const char *const physical_fields[] = {
        "physical_operators", "partition_count", "worker_count", "csv_record_safe", "parallel_enabled"
    };
    static const char *const physical_plan_markers[] = {
        "MILENA_STREAM_PLAN_SCAN_CSV_RECORDS", "MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES",
        "MILENA_STREAM_PLAN_ORDER_BY_KEY", "MILENA_STREAM_PLAN_JSON_SINK",
        "plan->parallel_enabled = false", "plan->csv_record_safe = true"
    };
    static const char *const spill_doc_markers[] = {"contrato", "checksum", "límites", "limpieza", "e2e"};
    static const char *const stream_guard_markers[] = {"STREAM_HARD_MAX_GROUPS", "STREAM_GROUP_STATE_BUDGET", "qsort(groups"};
    static const char *const source_reader_api_markers[] = {
        "src/source_reader.c", "source_reader.c", "source_reader.h"
    };
    static const char *const main_runtime_markers[] = {
        "AST_COMANDO_SST", "runtime_write_sst", "milena_simple_interest", "src/finance.c"
    };
    size_t index;

    {
        bool all_ast_markers = true;
        for (index = 0U; index < ARRAY_COUNT(typed_ast_markers); ++index) {
            if (!contains(ast, typed_ast_markers[index]) && !contains(parser, typed_ast_markers[index])) {
                all_ast_markers = false;
            }
        }
        need(all_ast_markers, "missing typed AST contract");
    }
    need(contains(runtime, "milena_validate_ast(program, error)"), "runtime bypasses semantic validation");
    need(contains(runtime, "run_stream_dataset_with_options") && contains(runtime, "milena_stream_csv_summary_with_options"),
         "stream backend is not invoked by the canonical runtime");
    runtime_stream_start = require_substring(runtime, "static MilenaStatus run_stream_dataset_with_options", "runtime bypasses semantic validation");
    runtime_stream_end = require_substring(runtime_stream_start, "MilenaStatus milena_run_dataset_program", "runtime bypasses semantic validation");
    need(strstr(runtime_stream_start, "summary->stream_operation") != NULL && strstr(runtime_stream_start, "summary->stream_operation") < runtime_stream_end,
         "natural stream metrics do not use typed AST operations");
    need(strstr(runtime_stream_start, "milena_stream_csv_grouped_with_options") != NULL &&
         strstr(runtime_stream_start, "milena_stream_csv_grouped_with_options") < runtime_stream_end,
         "grouped streaming is not invoked by the canonical runtime");
    need(strstr(runtime_stream_start, "milena_stream_csv_grouped_spill_with_keys_and_options") != NULL &&
         strstr(runtime_stream_start, "milena_stream_csv_grouped_spill_with_keys_and_options") < runtime_stream_end,
         "streaming spill is not invoked by the canonical runtime");
    need(contains(stream_header, "milena_stream_csv_grouped_spill_with_keys_and_options") &&
         contains(stream, "milena_grouped_aggregate_finalize"),
         "streaming spill does not use the canonical reducer callback");
    need(contains(runtime_stream_start, "group_key_count") && contains(plan_header, "group_keys[2]"),
         "composite group keys bypass the typed plan");
    need(contains(planner, "AST_AGRUPACION_POR") && contains(runtime_stream_start, "plan->group_key") &&
         contains(runtime_stream_start, "group_key->value") && contains(runtime_stream_start, "MILENA_PHYSICAL_CSV_STREAM_GROUPED"),
         "grouping key bypasses the typed logical/physical plan");
    need(contains(runtime, "milena_stream_execution_plan_build"), "canonical runtime bypasses the typed streaming plan");
    for (index = 0U; index < ARRAY_COUNT(physical_fields); ++index) {
        if (!contains(plan_header, physical_fields[index])) {
            (void)fprintf(stderr, "typed query plan omits physical safety field: %s\n", physical_fields[index]);
            exit(EXIT_FAILURE);
        }
    }
    for (index = 0U; index < ARRAY_COUNT(physical_plan_markers); ++index) {
        if (!contains(planner, physical_plan_markers[index])) {
            (void)fprintf(stderr, "physical plan omits its record-safe contract: %s\n", physical_plan_markers[index]);
            exit(EXIT_FAILURE);
        }
    }
    need(contains(planner, "milena_stream_execution_plan_validate"), "planner does not validate the sequential record-safe pipeline");
    need((contains(planner, "AST_AGRUPACION_SPILL") && contains(planner, "MILENA_PHYSICAL_CSV_STREAM_GROUPED_SPILL") &&
          contains(runtime, "plan->spill_policy")), "spill resource AST is not carried by the typed physical plan");
    need(contains(stream_header, "milena_stream_csv_grouped_with_options"), "grouped streaming API is not declared in the canonical contract");
    need(!contains(make, "src/spill.c"), "unplanned legacy spill module entered product SOURCES");
    if (contains(make, "src/spill_store.c")) {
        need(contains(make, "src/language_grouped_spill.c") && manifest_has("language_grouped_spill.c") &&
             contains(ast, "AST_AGRUPACION_SPILL") && contains(parser, "AST_AGRUPACION_SPILL") &&
             contains(runtime, "milena_language_group_by_spill"),
             "grouped spill API lacks its typed canonical #agrupar adapter");
    }
    need(contains(streaming_docs, "max_record_bytes") && contains(streaming_docs, "memoria_reductor_bytes") &&
         contains_case_insensitive_ascii(streaming_docs, "rss global"),
         "streaming spill docs must distinguish record/reducer caps from RSS");
    need(contains_case_insensitive_ascii(pr25_docs, "streaming") && contains_case_insensitive_ascii(pr25_docs, "materializando"),
         "big-data status must describe the real streaming/table boundaries");
    for (index = 0U; index < ARRAY_COUNT(spill_doc_markers); ++index) {
        need(contains_case_insensitive_ascii(spill_contract, spill_doc_markers[index]), "grouped spill design contract is incomplete: marker");
    }
    for (index = 0U; index < ARRAY_COUNT(stream_guard_markers); ++index) {
        if (!contains(stream, stream_guard_markers[index])) {
            (void)fprintf(stderr, "bounded/deterministic grouping guard missing: %s\n", stream_guard_markers[index]);
            exit(EXIT_FAILURE);
        }
    }
    spill_start = require_substring(stream, "MilenaStatus milena_stream_csv_grouped_spill_with_keys_and_options", "streaming spill bypasses the bounded local source-reader/CSV parser");
    need(contains(spill_start, "milena_source_reader_read_record") && contains(spill_start, "stream_split") &&
         contains(spill_start, "milena_source_reader_open_local_csv"),
         "streaming spill bypasses the bounded local source-reader/CSV parser");
    need(contains(make, source_reader_api_markers[0]) && manifest_has(source_reader_api_markers[1]) &&
         contains(windows_build, source_reader_api_markers[1]) && contains(stream, source_reader_api_markers[2]),
         "local source reader is not classified and linked as product code");
    need(!contains(spill_start, "Dataset") && !contains(spill_start, "MilenaTable"),
         "streaming spill materializes Dataset/MilenaTable");
    need(contains(make, "src/stream.c") && manifest_has("stream.c"), "stream.c is not classified as official product");
    need(contains(make, "src/group_key_codec.c") && manifest_has("group_key_codec.c") &&
         contains(windows_build, "group_key_codec.c") && contains(stream, "group_key_codec.h") &&
         !file_exists("tests/support/group_key_codec.c"),
         "group key codec must be a single linked product source");
    need(contains(stream_header, "milena_stream_csv_grouped_spill_with_options") &&
         contains(stream_header, "milena_stream_csv_grouped_spill_with_keys_and_options"),
         "single-key compatibility API and composite-key spill API must both remain available");
    for (index = 0U; index < ARRAY_COUNT(main_runtime_markers); ++index) {
        if (!contains(runtime, main_runtime_markers[index]) && !contains(make, main_runtime_markers[index])) {
            (void)fprintf(stderr, "canonical analysis capability missing: %s\n", main_runtime_markers[index]);
            exit(EXIT_FAILURE);
        }
    }
    make_sources = strstr(make, "SOURCES =");
    need(make_sources != NULL, "Makefile no contiene una asignación SOURCES reconocible");
    make_sources += strlen("SOURCES =");
    manifest_make = strstr(make_sources, "OBJECTS");
    need(manifest_make != NULL, "Makefile no contiene el final de SOURCES reconocible");
    {
        Span product_span = {make_sources, manifest_make};
        static const char *const forbidden_tokens[] = {"compiler.c", "ir.c", "vm.c", "gc.c", "arena.c", "bytecode_native.c"};
        for (index = 0U; index < ARRAY_COUNT(forbidden_tokens); ++index) {
            if (token_boundary_match(product_span, forbidden_tokens[index])) {
                (void)fprintf(stderr, "experimental module leaked into product build: %s\n", forbidden_tokens[index]);
                exit(EXIT_FAILURE);
            }
        }
    }
    need(!stream_standalone_main(stream), "stream.c contains a standalone entry point");
    (void)printf("OK: global and grouped streaming use typed AST, canonical runtime, bounded state, and product sources\n");

    free(ast);
    free(parser);
    free(runtime);
    free(planner);
    free(plan_header);
    free(make);
    free(windows_build);
    free(stream);
    free(stream_header);
    free(streaming_docs);
    free(spill_contract);
    free(pr25_docs);
}




/* Phase 3 Python checker ports: local Markdown targets and Termux contracts. */
static bool ascii_equal_nocase(const char *left, const char *right, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z') {
            a = (unsigned char)(a + ((unsigned char)'a' - (unsigned char)'A'));
        }
        if (b >= (unsigned char)'A' && b <= (unsigned char)'Z') {
            b = (unsigned char)(b + ((unsigned char)'a' - (unsigned char)'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static char *optional_read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    size_t bytes_read;
    char *contents;
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0L || fseek(file, 0L, SEEK_SET) != 0 ||
        (unsigned long)length >= (unsigned long)((size_t)-1)) {
        (void)fclose(file);
        return NULL;
    }
    contents = (char *)malloc((size_t)length + 1U);
    if (contents == NULL) {
        (void)fclose(file);
        (void)fprintf(stderr, "ERROR: out of memory while reading %s\n", path);
        exit(EXIT_FAILURE);
    }
    bytes_read = fread(contents, 1U, (size_t)length, file);
    {
        bool read_failed = bytes_read != (size_t)length || ferror(file) != 0;
        int close_status = fclose(file);
        if (read_failed || close_status != 0) {
            free(contents);
            return NULL;
        }
    }
    contents[bytes_read] = '\0';
    return contents;
}

static void require_file_fragments(StringList *errors, const char *path,
                                   const char *const *fragments, size_t fragment_count)
{
    size_t index;
    char *text;
    if (!file_exists(path)) {
        list_addf(errors, "missing required file: %s", path);
        return;
    }
    text = read_file(path);
    for (index = 0U; index < fragment_count; ++index) {
        if (!contains(text, fragments[index])) {
            list_addf(errors, "%s missing required contract: %s", path, fragments[index]);
        }
    }
    free(text);
}

static bool is_scheme_char(unsigned char value, bool first)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (!first && ((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
                       value == (unsigned char)'+' || value == (unsigned char)'.' ||
                       value == (unsigned char)'-'));
}

static char *markdown_prose(const char *text)
{
    size_t text_length = strlen(text);
    char *output = (char *)malloc(text_length + 1U);
    size_t output_length = 0U;
    const char *line = text;
    bool in_fence = false;
    char fence = '\0';
    bool wrote_line = false;
    if (output == NULL) {
        (void)fprintf(stderr, "ERROR: out of memory while stripping Markdown code\n");
        exit(EXIT_FAILURE);
    }
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *cursor;
        const char *trimmed;
        bool is_fence = false;
        size_t line_length;
        if (end == NULL) {
            end = line + strlen(line);
        }
        line_length = (size_t)(end - line);
        trimmed = line;
        while (trimmed < end && isspace((unsigned char)*trimmed) != 0) {
            ++trimmed;
        }
        if ((size_t)(end - trimmed) >= 3U &&
            ((trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') ||
             (trimmed[0] == '~' && trimmed[1] == '~' && trimmed[2] == '~'))) {
            char current = trimmed[0];
            is_fence = true;
            if (!in_fence) {
                in_fence = true;
                fence = current;
            } else if (current == fence) {
                in_fence = false;
                fence = '\0';
            }
        }
        if (!in_fence && !is_fence) {
            bool wrote_char = false;
            if (wrote_line) {
                output[output_length++] = '\n';
            }
            cursor = line;
            while (cursor < end) {
                if (*cursor == '`') {
                    const char *closing = cursor + 1;
                    while (closing < end && *closing != '`') {
                        ++closing;
                    }
                    if (closing < end) {
                        cursor = closing + 1;
                        continue;
                    }
                }
                output[output_length++] = *cursor++;
                wrote_char = true;
            }
            (void)wrote_char;
            wrote_line = true;
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
        if (*line == '\0') {
            break;
        }
        (void)line_length;
    }
    output[output_length] = '\0';
    return output;
}

static char *markdown_destination(const char *begin, const char *end)
{
    const char *start = begin;
    const char *finish = end;
    const char *space;
    size_t length;
    char *result;
    while (start < finish && isspace((unsigned char)*start) != 0) {
        ++start;
    }
    while (finish > start && isspace((unsigned char)finish[-1]) != 0) {
        --finish;
    }
    if (start < finish && *start == '<') {
        const char *close = memchr(start + 1, '>', (size_t)(finish - start - 1));
        if (close != NULL) {
            ++start;
            finish = close;
        }
    } else {
        for (space = start; space < finish; ++space) {
            if (isspace((unsigned char)*space) != 0) {
                finish = space;
                break;
            }
        }
    }
    length = (size_t)(finish - start);
    result = (char *)malloc(length + 1U);
    if (result == NULL) {
        (void)fprintf(stderr, "ERROR: out of memory while parsing Markdown destination\n");
        exit(EXIT_FAILURE);
    }
    memcpy(result, start, length);
    result[length] = '\0';
    return result;
}

static void collect_markdown_targets(const char *prose, StringList *targets)
{
    const char *cursor = prose;
    while (*cursor != '\0') {
        const char *open = strchr(cursor, '[');
        const char *label_end;
        const char *destination_start;
        const char *destination_end;
        if (open == NULL) {
            break;
        }
        label_end = strchr(open + 1, ']');
        if (label_end != NULL && label_end[1] == '(') {
            destination_start = label_end + 2;
            destination_end = destination_start;
            while (*destination_end != '\0' && *destination_end != ')' && *destination_end != '\n') {
                ++destination_end;
            }
            if (destination_end > destination_start && *destination_end == ')') {
                char *target = markdown_destination(destination_start, destination_end);
                list_add(targets, target);
                free(target);
                cursor = destination_end + 1;
                continue;
            }
        }
        cursor = open + 1;
    }
}

static bool html_word(unsigned char value)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static void collect_html_targets(const char *text, StringList *targets)
{
    const char *cursor = text;
    while ((cursor = strchr(cursor, '<')) != NULL) {
        const char *tag = cursor + 1;
        const char *tag_end;
        const char *scan;
        bool valid_tag;
        {
            size_t tag_length = strlen(tag);
            bool is_img = tag_length >= 3U && ascii_equal_nocase(tag, "img", 3U);
            bool is_anchor = tag_length >= 1U && ascii_equal_nocase(tag, "a", 1U);
            valid_tag = (is_anchor || is_img) &&
                        !html_word((unsigned char)tag[is_img ? 3U : 1U]);
        }
        if (!valid_tag) {
            cursor += 1;
            continue;
        }
        tag_end = strchr(tag, '>');
        if (tag_end == NULL) {
            break;
        }
        scan = tag;
        while (scan < tag_end) {
            const char *attribute = NULL;
            const char *attribute_cursor;
            const char *value_start;
            const char *value_end;
            for (attribute_cursor = scan; attribute_cursor < tag_end; ++attribute_cursor) {
                if ((attribute_cursor == tag || !html_word((unsigned char)attribute_cursor[-1])) &&
                    (size_t)(tag_end - attribute_cursor) >= 5U &&
                    ((ascii_equal_nocase(attribute_cursor, "href", 4U) && attribute_cursor[4] == '=') ||
                     (ascii_equal_nocase(attribute_cursor, "src", 3U) && attribute_cursor[3] == '='))) {
                    attribute = attribute_cursor;
                    break;
                }
            }
            if (attribute == NULL) {
                break;
            }
            attribute_cursor = attribute + (ascii_equal_nocase(attribute, "href", 4U) ? 5U : 4U);
            if (*attribute_cursor == '\'' || *attribute_cursor == '"') {
                char quote = *attribute_cursor++;
                value_start = attribute_cursor;
                value_end = value_start;
                while (value_end < tag_end && *value_end != quote) {
                    ++value_end;
                }
                if (value_end > value_start && value_end < tag_end) {
                    char *target = (char *)malloc((size_t)(value_end - value_start) + 1U);
                    if (target == NULL) {
                        (void)fprintf(stderr, "ERROR: out of memory while parsing HTML target\n");
                        exit(EXIT_FAILURE);
                    }
                    memcpy(target, value_start, (size_t)(value_end - value_start));
                    target[value_end - value_start] = '\0';
                    list_add(targets, target);
                    free(target);
                }
                scan = value_end < tag_end ? value_end + 1 : tag_end;
            } else {
                scan = attribute_cursor;
            }
        }
        cursor = tag_end + 1;
    }
}

static int hex_value(unsigned char value)
{
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        return (int)(value - (unsigned char)'0');
    }
    if (value >= (unsigned char)'a' && value <= (unsigned char)'f') {
        return (int)(value - (unsigned char)'a') + 10;
    }
    if (value >= (unsigned char)'A' && value <= (unsigned char)'F') {
        return (int)(value - (unsigned char)'A') + 10;
    }
    return -1;
}

static char *percent_decode(const char *text, size_t length)
{
    char *decoded = (char *)malloc(length + 1U);
    size_t input = 0U;
    size_t output = 0U;
    if (decoded == NULL) {
        (void)fprintf(stderr, "ERROR: out of memory while decoding URL path\n");
        exit(EXIT_FAILURE);
    }
    while (input < length) {
        if (text[input] == '%' && input + 2U < length) {
            int high = hex_value((unsigned char)text[input + 1U]);
            int low = hex_value((unsigned char)text[input + 2U]);
            if (high >= 0 && low >= 0) {
                decoded[output++] = (char)(high * 16 + low);
                input += 3U;
                continue;
            }
        }
        decoded[output++] = text[input++];
    }
    decoded[output] = '\0';
    return decoded;
}

static bool known_repository_directory(const char *path)
{
    static const char *const directories[] = {
        ".", ".github", ".github/workflows", "assets", "benchmarks", "docs", "examples",
        "include", "packaging", "packaging/apt", "packaging/debian", "packaging/termux",
        "packaging/termux-packages", "packaging/termux-packages/milena", "packaging/windows",
        "scripts", "src", "tests", "tests/fixtures", "tests/fixtures/arrow_ipc", "third_party",
        "third_party/sqlite", "tools"
    };
    size_t index;
    if (*path == '\0') {
        return true;
    }
    for (index = 0U; index < ARRAY_COUNT(directories); ++index) {
        if (strcmp(path, directories[index]) == 0) {
            return true;
        }
    }
    return false;
}

static char *normalize_repository_path(const char *document, const char *target_path, bool *escaped)
{
    size_t doc_length = strlen(document);
    size_t target_length = strlen(target_path);
    size_t capacity = doc_length + target_length + 4U;
    char *joined = (char *)malloc(capacity);
    char *normalized = (char *)malloc(capacity);
    size_t stack_count = 0U;
    size_t *component_starts = (size_t *)malloc(capacity * sizeof(*component_starts));
    size_t *component_lengths = (size_t *)malloc(capacity * sizeof(*component_lengths));
    size_t length = 0U;
    const char *slash;
    const char *cursor;
    size_t index;
    if (joined == NULL || normalized == NULL || component_starts == NULL || component_lengths == NULL) {
        free(joined);
        free(normalized);
        free(component_starts);
        free(component_lengths);
        (void)fprintf(stderr, "ERROR: out of memory while resolving Markdown path\n");
        exit(EXIT_FAILURE);
    }
    if (target_path[0] != '/') {
        slash = strrchr(document, '/');
        if (slash != NULL) {
            size_t base_length = (size_t)(slash - document) + 1U;
            memcpy(joined, document, base_length);
            length = base_length;
        }
    }
    if (target_path[0] == '/') {
        length = 0U;
        cursor = target_path + 1;
    } else {
        cursor = target_path;
    }
    if (target_path[0] == '/') {
        while (*cursor != '\0') {
            joined[length++] = *cursor++;
        }
    } else {
        while (*cursor != '\0') {
            joined[length++] = *cursor++;
        }
    }
    joined[length] = '\0';
    *escaped = false;
    cursor = joined;
    while (*cursor != '\0') {
        const char *component = cursor;
        size_t component_length;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        component_length = (size_t)(cursor - component);
        if (component_length == 0U || (component_length == 1U && component[0] == '.')) {
            /* Ignore empty and current-directory components. */
        } else if (component_length == 2U && component[0] == '.' && component[1] == '.') {
            if (stack_count == 0U) {
                *escaped = true;
            } else {
                --stack_count;
            }
        } else {
            component_starts[stack_count] = (size_t)(component - joined);
            component_lengths[stack_count] = component_length;
            ++stack_count;
        }
        if (*cursor == '/') {
            ++cursor;
        }
    }
    length = 0U;
    for (index = 0U; index < stack_count; ++index) {
        if (index != 0U) {
            normalized[length++] = '/';
        }
        memcpy(normalized + length, joined + component_starts[index], component_lengths[index]);
        length += component_lengths[index];
    }
    normalized[length] = '\0';
    free(joined);
    free(component_starts);
    free(component_lengths);
    return normalized;
}

static bool markdown_target_exists(const char *normalized)
{
    if (file_exists(normalized)) {
        return true;
    }
    return known_repository_directory(normalized);
}

static void markdown_check_document(const char *document, StringList *failures)
{
    char *text = optional_read_file(document);
    char *prose;
    StringList targets;
    size_t index;
    const char *display_document = document;
    if (text == NULL) {
        return;
    }
    while (display_document[0] == '.' && display_document[1] == '/') {
        display_document += 2;
    }
    prose = markdown_prose(text);
    list_init(&targets);
    collect_markdown_targets(prose, &targets);
    collect_html_targets(prose, &targets);
    for (index = 0U; index < targets.count; ++index) {
        const char *target = targets.items[index];
        const char *path_start = target;
        const char *end = target + strlen(target);
        const char *colon = NULL;
        const char *scan;
        const char *path_end;
        bool has_scheme = false;
        bool has_netloc = false;
        size_t scheme_length = 0U;
        bool escaped;
        char *decoded;
        char *normalized;
        if (target[0] == '/' && target[1] == '/') {
            const char *host_end = target + 2;
            while (*host_end != '\0' && *host_end != '/' && *host_end != '?' && *host_end != '#') {
                ++host_end;
            }
            has_netloc = true;
            path_start = host_end;
        } else {
            for (scan = target; scan < end && *scan != '/' && *scan != '?' && *scan != '#'; ++scan) {
                if (*scan == ':') {
                    colon = scan;
                    break;
                }
            }
            if (colon != NULL && colon > target) {
                bool valid = is_scheme_char((unsigned char)target[0], true);
                scheme_length = (size_t)(colon - target);
                for (scan = target + 1; valid && scan < colon; ++scan) {
                    valid = is_scheme_char((unsigned char)*scan, false);
                }
                if (valid) {
                    has_scheme = true;
                    path_start = colon + 1;
                    if (path_start[0] == '/' && path_start[1] == '/') {
                        const char *host_end = path_start + 2;
                        while (*host_end != '\0' && *host_end != '/' && *host_end != '?' && *host_end != '#') {
                            ++host_end;
                        }
                        has_netloc = true;
                        path_start = host_end;
                    }
                }
            }
        }
        path_end = path_start;
        while (*path_end != '\0' && *path_end != '?' && *path_end != '#') {
            ++path_end;
        }
        if (path_start == path_end || has_netloc ||
            (has_scheme && ((scheme_length == 4U && ascii_equal_nocase(target, "http", 4U)) ||
                             (scheme_length == 5U && ascii_equal_nocase(target, "https", 5U)) ||
                             (scheme_length == 6U && ascii_equal_nocase(target, "mailto", 6U)) ||
                             (scheme_length == 3U && ascii_equal_nocase(target, "tel", 3U)) ||
                             (scheme_length == 4U && ascii_equal_nocase(target, "data", 4U))))) {
            continue;
        }
        decoded = percent_decode(path_start, (size_t)(path_end - path_start));
        normalized = normalize_repository_path(display_document, decoded, &escaped);
        if (escaped) {
            list_addf(failures, "%s: target escapes repository: %s", display_document, target);
        } else if (!markdown_target_exists(normalized)) {
            list_addf(failures, "%s: missing target: %s", display_document, target);
        }
        free(decoded);
        free(normalized);
    }
    list_free(&targets);
    free(prose);
    free(text);
}

static void check_markdown_links(int argc, char **argv)
{
    StringList documents;
    StringList failures;
    int argument;
    size_t index;
    list_init(&documents);
    list_init(&failures);
    for (argument = 2; argument < argc; ++argument) {
        list_add(&documents, argv[argument]);
    }
    list_sort(&documents);
    for (index = 0U; index < documents.count; ++index) {
        markdown_check_document(documents.items[index], &failures);
    }
    if (failures.count != 0U) {
        (void)fprintf(stderr, "Broken local Markdown targets:\n");
        for (index = 0U; index < failures.count; ++index) {
            (void)fprintf(stderr, "- %s\n", failures.items[index]);
        }
        list_free(&failures);
        list_free(&documents);
        exit(EXIT_FAILURE);
    }
    (void)printf("All local Markdown targets resolve.\n");
    list_free(&failures);
    list_free(&documents);
}

static void termux_industrial_check(void)
{
    StringList errors;
    static const char *const workflow_fragments[] = {
        "concurrency:", "cancel-in-progress: false", "confirm_device:",
        "if: inputs.confirm_device == true", "timeout-minutes:", "termux-real-smoke.sh",
        "artifacts/termux-runner/toolchain.txt", "if-no-files-found: error"
    };
    static const char *const publish_fragments[] = {
        "concurrency:", "confirm_publish:", "if: inputs.confirm_publish == true",
        "--pattern 'milena_*_aarch64.deb'", "gpgv", "--provenance", "--require-provenance",
        "--require-elf", "if-no-files-found: error"
    };
    static const char *const preflight_fragments[] = {
        "uname -m", "dpkg --print-architecture", "termux-info", "readelf", "workspace-sha256.txt"
    };
    static const char *const artifact_fragments[] = {"expected_filename", "provenance", "gpgv", "binary-aarch64"};
    static const char *const generator_fragments[] = {"PACKAGE_ARCH", "sha256sum", "MILENA_GPG_KEY_ID", "binary-aarch64"};
    static const char *const plan_fragments[] = {"no crea ni registra", "install", "actualización", "eliminación", "SBOM"};
    static const char *const security_fragments[] = {"Trust boundary", "gpgv"};
    static const char *const checklist_fragments[] = {"confirm_device=true", "confirm_publish=true"};
    static const char *const runbook_fragments[] = {"Artefacto o firma inválida", "Runner no conforme"};
    char *workflow;
    char *device;
    list_init(&errors);
    require_file_fragments(&errors, ".github/workflows/termux-aarch64-contract.yml", workflow_fragments, ARRAY_COUNT(workflow_fragments));
    require_file_fragments(&errors, ".github/workflows/publish-apt.yml", publish_fragments, ARRAY_COUNT(publish_fragments));
    require_file_fragments(&errors, "scripts/termux-runner-preflight.sh", preflight_fragments, ARRAY_COUNT(preflight_fragments));
    require_file_fragments(&errors, "scripts/validate_termux_artifact.py", artifact_fragments, ARRAY_COUNT(artifact_fragments));
    require_file_fragments(&errors, "packaging/termux/generate-apt-repo.sh", generator_fragments, ARRAY_COUNT(generator_fragments));
    require_file_fragments(&errors, "docs/TERMUX_VALIDATION_PLAN.md", plan_fragments, ARRAY_COUNT(plan_fragments));
    require_file_fragments(&errors, "docs/TERMUX_SECURITY.md", security_fragments, ARRAY_COUNT(security_fragments));
    require_file_fragments(&errors, "docs/TERMUX_RELEASE_CHECKLIST.md", checklist_fragments, ARRAY_COUNT(checklist_fragments));
    require_file_fragments(&errors, "docs/TERMUX_INCIDENT_RUNBOOK.md", runbook_fragments, ARRAY_COUNT(runbook_fragments));
    workflow = optional_read_file(".github/workflows/publish-apt.yml");
    if (workflow != NULL) {
        if (contains(workflow, "\n  push:")) {
            list_add(&errors, "APT publication must be manual; tag pushes are not an approval");
        }
        free(workflow);
    }
    workflow = optional_read_file(".github/workflows/termux-aarch64-contract.yml");
    if (workflow == NULL) {
        device = NULL;
    } else {
        const char *marker = "  termux-aarch64-contract:";
        const char *found = strstr(workflow, marker);
        device = found == NULL ? NULL : (char *)found + strlen(marker);
    }
    if (device == NULL || *device == '\0' ||
        !contains(device, "runs-on: [self-hosted, termux, aarch64, milena]")) {
        list_add(&errors, "Android contract must use the registered self-hosted Termux/aarch64 runner");
    }
    if (workflow != NULL) {
        free(workflow);
    }
    if (errors.count != 0U) {
        size_t index;
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&errors);
        exit(EXIT_FAILURE);
    }
    (void)printf("Termux industrial static contract: OK\n");
    list_free(&errors);
}

static char *capture_recipe_assignment(const char *text, const char *name, bool anchored)
{
    const char *line = text;
    size_t name_length = strlen(name);
    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        const char *cursor = line;
        const char *start;
        const char *end;
        char quote = '\0';
        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        if (!anchored) {
            while (cursor < line_end && isspace((unsigned char)*cursor) != 0) {
                ++cursor;
            }
        }
        if ((size_t)(line_end - cursor) >= name_length + 1U &&
            memcmp(cursor, name, name_length) == 0 && cursor[name_length] == '=') {
            start = cursor + name_length + 1U;
            if (start < line_end && (*start == '\'' || *start == '"')) {
                quote = *start++;
                end = start;
                while (end < line_end && *end != quote) {
                    ++end;
                }
                if (end == line_end) {
                    line = *line_end == '\0' ? line_end : line_end + 1;
                    continue;
                }
            } else {
                end = start;
                while (end < line_end && isspace((unsigned char)*end) == 0 && *end != '#') {
                    ++end;
                }
                if (end == start) {
                    line = *line_end == '\0' ? line_end : line_end + 1;
                    continue;
                }
            }
            {
                size_t value_length = (size_t)(end - start);
                char *value = (char *)malloc(value_length + 1U);
                if (value == NULL) {
                    (void)fprintf(stderr, "ERROR: out of memory while parsing Termux recipe\n");
                    exit(EXIT_FAILURE);
                }
                memcpy(value, start, value_length);
                value[value_length] = '\0';
                return value;
            }
        }
        line = *line_end == '\0' ? line_end : line_end + 1;
    }
    return NULL;
}

static char *capture_recipe_termux_version(const char *text) {     return capture_recipe_assignment(text, "TERMUX_PKG_VERSION", false); }

static char *capture_runtime_version(const char *text)
{
    const char *line = text;
    static const char marker[] = "#define MILENA_VERSION ";
    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        const char *cursor;
        const char *end;
        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        if ((size_t)(line_end - line) >= sizeof(marker) - 1U &&
            memcmp(line, marker, sizeof(marker) - 1U) == 0) {
            cursor = line + sizeof(marker) - 1U;
            if (cursor < line_end && *cursor == '"') {
                ++cursor;
                end = cursor;
                while (end < line_end && *end != '"') {
                    ++end;
                }
                if (end < line_end && end > cursor) {
                    size_t value_length = (size_t)(end - cursor);
                    char *value = (char *)malloc(value_length + 1U);
                    if (value == NULL) {
                        (void)fprintf(stderr, "ERROR: out of memory while reading runtime version\n");
                        exit(EXIT_FAILURE);
                    }
                    memcpy(value, cursor, value_length);
                    value[value_length] = '\0';
                    return value;
                }
            }
        }
        line = *line_end == '\0' ? line_end : line_end + 1;
    }
    return NULL;
}

static bool is_hex_string(const char *text, size_t expected_length)
{
    size_t index;
    if (strlen(text) != expected_length) {
        return false;
    }
    for (index = 0U; index < expected_length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'f') ||
              (value >= (unsigned char)'A' && value <= (unsigned char)'F'))) {
            return false;
        }
    }
    return true;
}

static bool valid_recipe_version(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
        return false;
    }
    ++cursor;
    while (*cursor != 0U) {
        unsigned char value = *cursor++;
        if (!((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
              value == (unsigned char)'.' || value == (unsigned char)'+' || value == (unsigned char)':' ||
              value == (unsigned char)'~' || value == (unsigned char)'-')) {
            return false;
        }
    }
    return true;
}

static bool recipe_has_debian_usr_bin(const char *text)
{
    const char *cursor = text;
    static const char prefix[] = "com.termux/files";
    while ((cursor = strstr(cursor, "/usr/bin")) != NULL) {
        const char *after = cursor + strlen("/usr/bin");
        bool allowed_prefix = (size_t)(cursor - text) >= sizeof(prefix) - 1U &&
            memcmp(cursor - (sizeof(prefix) - 1U), prefix, sizeof(prefix) - 1U) == 0;
        if ((*after == '/' || *after == '\'' || *after == '"' || *after == '\0') && !allowed_prefix) {
            return true;
        }
        ++cursor;
    }
    return false;
}

static void validate_termux_recipe_static(StringList *errors, const char *recipe_text);

static char *join_recipe_path(const char *directory, const char *leaf)
{
    size_t directory_length = strlen(directory);
    size_t leaf_length = strlen(leaf);
    bool slash = directory_length != 0U && directory[directory_length - 1U] != '/';
    char *path;
    if (directory_length > (size_t)-1 - leaf_length - (slash ? 2U : 1U)) {
        return NULL;
    }
    path = (char *)malloc(directory_length + leaf_length + (slash ? 2U : 1U));
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, directory, directory_length);
    if (slash) {
        path[directory_length++] = '/';
    }
    memcpy(path + directory_length, leaf, leaf_length + 1U);
    return path;
}

static char *recipe_source_url(const char *source, const char *version)
{
    static const char token[] = "${TERMUX_PKG_VERSION}";
    const char *match = strstr(source, token);
    size_t prefix, suffix, version_length;
    char *result;
    if (match == NULL) {
        return NULL;
    }
    prefix = (size_t)(match - source);
    suffix = strlen(match + sizeof(token) - 1U);
    version_length = strlen(version);
    if (prefix > (size_t)-1 - version_length - suffix - 1U) {
        return NULL;
    }
    result = (char *)malloc(prefix + version_length + suffix + 1U);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, source, prefix);
    memcpy(result + prefix, version, version_length);
    memcpy(result + prefix + version_length, match + sizeof(token) - 1U, suffix + 1U);
    return result;
}

static bool fetch_recipe_digest(const char *url, const char *expected, StringList *errors)
{
    char temporary[] = "milena-termux-recipe-XXXXXX";
    int fd = mkstemp(temporary);
    pid_t child;
    int status;
    char actual[65];
    uint64_t file_size;
    bool success = false;
    if (fd < 0) {
        list_add(errors, "could not create temporary file for source SHA256 validation");
        return false;
    }
    (void)close(fd);
    child = fork();
    if (child == 0) {
        execlp("curl", "curl", "--location", "--fail", "--silent", "--show-error",
               "--max-time", "30", "--output", temporary, url, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        list_add(errors, "could not start curl for source SHA256 validation");
    } else {
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            list_add(errors, "could not wait for curl during source SHA256 validation");
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            if (milena_sha256_file(temporary, actual, &file_size) != 0) {
                list_add(errors, "could not read fetched source for SHA256 validation");
            } else if (strcmp(actual, expected) != 0 && strcasecmp(actual, expected) != 0) {
                list_addf(errors, "source SHA256 mismatch: fetched %s", actual);
            } else {
                success = true;
            }
        } else {
            list_add(errors, "could not fetch TERMUX_PKG_SRCURL for digest validation");
        }
    }
    (void)unlink(temporary);
    return success;
}

static void check_termux_recipe(int argc, char **argv)
{
    const char *recipe_path = "packaging/termux-packages/milena/build.sh";
    const char *official_dir = NULL;
    bool fetch = false;
    char *recipe_text;
    char *values[9];
    static const char *const required[] = {
        "TERMUX_PKG_HOMEPAGE", "TERMUX_PKG_DESCRIPTION", "TERMUX_PKG_LICENSE",
        "TERMUX_PKG_MAINTAINER", "TERMUX_PKG_VERSION", "TERMUX_PKG_SRCURL",
        "TERMUX_PKG_SHA256", "TERMUX_PKG_DEPENDS", "TERMUX_PKG_BUILD_IN_SRC"
    };
    StringList errors;
    int argument;
    size_t index;
    list_init(&errors);
    for (argument = 2; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--fetch") == 0) {
            fetch = true;
        } else if (strcmp(argv[argument], "--official-dir") == 0 && argument + 1 < argc) {
            official_dir = argv[++argument];
        } else if (strncmp(argv[argument], "--official-dir=", 15U) == 0) {
            official_dir = argv[argument] + 15U;
        } else if (argv[argument][0] == '-') {
            (void)fprintf(stderr, "ERROR: unknown termux-recipe option: %s\n", argv[argument]);
            exit(EXIT_FAILURE);
        } else if (strcmp(recipe_path, "packaging/termux-packages/milena/build.sh") == 0) {
            recipe_path = argv[argument];
        } else {
            (void)fprintf(stderr, "ERROR: more than one recipe path provided\n");
            exit(EXIT_FAILURE);
        }
    }
    recipe_text = optional_read_file(recipe_path);
    if (recipe_text == NULL) {
        (void)fprintf(stderr, "ERROR: cannot read recipe: %s\n", recipe_path);
        exit(EXIT_FAILURE);
    }
    validate_termux_recipe_static(&errors, recipe_text);
    for (index = 0U; index < ARRAY_COUNT(required); ++index) {
        values[index] = capture_recipe_assignment(recipe_text, required[index], false);
    }
    if (fetch && errors.count == 0U) {
        char *url = recipe_source_url(values[5], values[4]);
        if (url == NULL) {
            list_add(&errors, "could not resolve TERMUX_PKG_SRCURL version token");
        } else {
            (void)fetch_recipe_digest(url, values[6], &errors);
            free(url);
        }
    }
    if (official_dir != NULL) {
        char *path = join_recipe_path(official_dir, "build-package.sh");
        struct stat file_status;
        if (path == NULL) {
            list_add(&errors, "could not allocate official build-package.sh path");
        } else if (stat(path, &file_status) != 0 || !S_ISREG(file_status.st_mode)) {
            list_addf(&errors, "official checkout missing %s", path);
        } else if (access(path, X_OK) != 0) {
            list_addf(&errors, "official build-package.sh is not executable: %s", path);
        }
        free(path);
    }
    for (index = 0U; index < ARRAY_COUNT(required); ++index) {
        free(values[index]);
    }
    free(recipe_text);
    if (errors.count != 0U) {
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&errors);
        exit(EXIT_FAILURE);
    }
    (void)printf("Termux candidate recipe: valid metadata and Termux paths\n");
    list_free(&errors);
}

static void validate_termux_recipe_static(StringList *errors, const char *recipe_text)
{
    static const char *const required[] = {
        "TERMUX_PKG_HOMEPAGE", "TERMUX_PKG_DESCRIPTION", "TERMUX_PKG_LICENSE",
        "TERMUX_PKG_MAINTAINER", "TERMUX_PKG_VERSION", "TERMUX_PKG_SRCURL",
        "TERMUX_PKG_SHA256", "TERMUX_PKG_DEPENDS", "TERMUX_PKG_BUILD_IN_SRC"
    };
    static const char *const forbidden[] = {"@REPLACE", "/usr/local", "apt-get", "dpkg-buildpackage", "glibc"};
    char *values[ARRAY_COUNT(required)];
    size_t index;
    bool any_missing = false;
    for (index = 0U; index < ARRAY_COUNT(required); ++index) {
        values[index] = capture_recipe_assignment(recipe_text, required[index], false);
        if (values[index] == NULL) {
            list_addf(errors, "missing %s", required[index]);
            any_missing = true;
        }
    }
    if (any_missing) {
        for (index = 0U; index < ARRAY_COUNT(required); ++index) {
            free(values[index]);
        }
        return;
    }
    if (!is_hex_string(values[6], 64U)) {
        list_add(errors, "TERMUX_PKG_SHA256 must be a verified 64-hex digest, never a placeholder");
    }
    if (!valid_recipe_version(values[4])) {
        list_add(errors, "TERMUX_PKG_VERSION is not a valid package version");
    }
    if (!contains(values[5], "${TERMUX_PKG_VERSION}") || !contains(values[5], "/archive/refs/tags/v")) {
        list_add(errors, "TERMUX_PKG_SRCURL must be a versioned upstream tag using TERMUX_PKG_VERSION");
    }
    if (strcmp(values[8], "true") != 0) {
        list_add(errors, "TERMUX_PKG_BUILD_IN_SRC must be true for this source tree");
    }
    if (!contains(recipe_text, "termux_step_make()") || !contains(recipe_text, "termux_step_make_install()")) {
        list_add(errors, "recipe must define official termux build/install steps");
    }
    if (!contains(recipe_text, "$TERMUX_PREFIX/bin/milena")) {
        list_add(errors, "install path must use TERMUX_PREFIX, not a Debian prefix");
    }
    for (index = 0U; index < ARRAY_COUNT(forbidden); ++index) {
        if (contains_case_insensitive_ascii(recipe_text, forbidden[index])) {
            list_addf(errors, "recipe contains forbidden non-Termux token: %s", forbidden[index]);
        }
    }
    if (recipe_has_debian_usr_bin(recipe_text)) {
        list_add(errors, "recipe contains a Debian /usr/bin path");
    }
    for (index = 0U; index < ARRAY_COUNT(required); ++index) {
        free(values[index]);
    }
}

static void check_termux_packaging(void)
{
    static const char *const active[] = {
        ".github/workflows/publish-apt.yml", ".github/workflows/termux-aarch64-contract.yml",
        "Makefile", "packaging/termux/build-local-deb.sh", "packaging/termux/generate-apt-repo.sh",
        "packaging/termux/README.md", "packaging/termux-packages/README.md",
        "packaging/termux-packages/milena/build.sh", "scripts/termux-install-smoke.sh",
        "scripts/termux-real-smoke.sh", "scripts/validate_termux_artifact.py",
        "tests/test_termux_packaging.c"
    };
    static const char *const workflow_required[] = {
        "workflow_dispatch:", "confirm_device:", "if: inputs.confirm_device == true",
        "runs-on: [self-hosted, termux, aarch64, milena]", "TERMUX_PACKAGES_DIR", "build-package.sh",
        "-I -f milena", "termux-runner-preflight.sh", "validate_termux_artifact.py", "termux-real-smoke.sh",
        "make tools/check_repository_contracts", "./tools/check_repository_contracts termux-recipe",
        "upload-artifact@v4", "if-no-files-found: error"
    };
    static const char *const builder_required[] = {"tools/validate_termux_elf", "README.md", "SOURCE_DATE_EPOCH", ".provenance.json", "TERMUX=1"};
    static const char *const builder_forbidden[] = {"cp -R examples", "tests/", "include/", "src/compiler.c", "src/ir.c", "src/vm.c", "src/bytecode_native.c"};
    static const char *const readmes[] = {"README.md", "packaging/README.md", "packaging/termux/README.md"};
    StringList errors;
    size_t index;
    char *makefile;
    char *recipe_text;
    char *common;
    char *workflow;
    char *builder;
    char *recipe_version;
    char *runtime_version;
    list_init(&errors);
    for (index = 0U; index < ARRAY_COUNT(active); ++index) {
        char *text;
        const char *path = active[index];
        if (!file_exists(path)) {
            list_addf(&errors, "missing active packaging file: %s", path);
            continue;
        }
        text = read_file(path);
        if (contains_case_insensitive_ascii(path, "mano") || contains_case_insensitive_ascii(text, "mano")) {
            list_addf(&errors, "unrelated historical package residue in %s", path);
        }
        free(text);
    }
    recipe_text = optional_read_file("packaging/termux-packages/milena/build.sh");
    if (recipe_text != NULL) {
        validate_termux_recipe_static(&errors, recipe_text);
    }
    makefile = optional_read_file("Makefile");
    recipe_version = recipe_text == NULL ? NULL : capture_recipe_termux_version(recipe_text);
    common = optional_read_file("include/common.h");
    runtime_version = common == NULL ? NULL : capture_runtime_version(common);
    if (recipe_version == NULL || runtime_version == NULL || strcmp(recipe_version, runtime_version) != 0) {
        list_add(&errors, "Termux package version must match MILENA_VERSION exactly");
    }
    if (makefile == NULL || !contains(makefile, "TERMUX=1") || !contains(makefile, "TERMUX_PREFIX")) {
        list_add(&errors, "Makefile lacks explicit Termux build/install variables");
    }
    if (makefile != NULL) {
        static const char *const experimental[] = {"compiler.c", "ir.c", "vm.c", "bytecode_native.c"};
        static const char *const forbidden_make[] = {"/usr/bin", "/usr/local", "apt-get", "__GLIBC__"};
        Span product_source_span = find_make_sources(makefile);
        for (index = 0U; index < ARRAY_COUNT(experimental); ++index) {
            if (make_has_source_token(product_source_span, experimental[index], true)) {
                list_addf(&errors, "experimental source enters canonical Makefile SOURCES: %s", experimental[index]);
            }
        }
        for (index = 0U; index < ARRAY_COUNT(forbidden_make); ++index) {
            if (contains_case_insensitive_ascii(makefile, forbidden_make[index])) {
                list_addf(&errors, "Makefile contains Debian/glibc path or dependency: %s", forbidden_make[index]);
            }
        }
    }
    workflow = optional_read_file(".github/workflows/termux-aarch64-contract.yml");
    if (workflow == NULL) {
        list_add(&errors, "missing required file: .github/workflows/termux-aarch64-contract.yml");
    } else {
        for (index = 0U; index < ARRAY_COUNT(workflow_required); ++index) {
            if (!contains(workflow, workflow_required[index])) {
                list_addf(&errors, "workflow missing required contract: %s", workflow_required[index]);
            }
        }
        {
            const char *marker = "  termux-aarch64-contract:";
            const char *device = strstr(workflow, marker);
            device = device == NULL ? workflow : device + strlen(marker);
            if (contains(device, "ubuntu-latest") || contains(device, "windows-latest")) {
                list_add(&errors, "Termux device contract cannot use a hosted generic runner");
            }
        }
        free(workflow);
    }
    builder = optional_read_file("packaging/termux/build-local-deb.sh");
    if (builder != NULL) {
        for (index = 0U; index < ARRAY_COUNT(builder_required); ++index) {
            if (!contains(builder, builder_required[index])) {
                list_addf(&errors, "local Termux builder missing: %s", builder_required[index]);
            }
        }
        for (index = 0U; index < ARRAY_COUNT(builder_forbidden); ++index) {
            if (contains(builder, builder_forbidden[index])) {
                list_addf(&errors, "local Termux package builder contains forbidden payload/path: %s", builder_forbidden[index]);
            }
        }
        free(builder);
    }
    for (index = 0U; index < ARRAY_COUNT(readmes); ++index) {
        char *text;
        if (!file_exists(readmes[index])) {
            continue;
        }
        text = read_file(readmes[index]);
        if (contains(text, "pkg install milena") &&
            !contains_case_insensitive_ascii(text, "todavía no") &&
            !contains_case_insensitive_ascii(text, "aún no") &&
            !contains_case_insensitive_ascii(text, "no existe")) {
            list_addf(&errors, "unqualified pkg install claim in %s", readmes[index]);
        }
        free(text);
    }
    free(recipe_text);
    free(makefile);
    free(common);
    free(recipe_version);
    free(runtime_version);
    if (errors.count != 0U) {
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&errors);
        exit(EXIT_FAILURE);
    }
    (void)printf("Termux packaging guardrails: OK\n");
    list_free(&errors);
}

static bool span_contains_case_insensitive_ascii(const char *begin, const char *end, const char *needle)
{
    size_t length = strlen(needle);
    const char *cursor;
    if (length == 0U) {
        return true;
    }
    if (end < begin || (size_t)(end - begin) < length) {
        return false;
    }
    for (cursor = begin; (size_t)(end - cursor) >= length; ++cursor) {
        if (ascii_equal_nocase(cursor, needle, length)) {
            return true;
        }
    }
    return false;
}

static bool documents_unregistered_runner(const char *text)
{
    const char *cursor = text;
    size_t text_length = strlen(text);
    while (*cursor != '\0') {
        if ((size_t)(text + text_length - cursor) >= strlen("no crea ni registra un") &&
            ascii_equal_nocase(cursor, "no crea ni registra un", strlen("no crea ni registra un"))) {
            const char *after = cursor + strlen("no crea ni registra un");
            while (*after != '\0' && isspace((unsigned char)*after) != 0) {
                ++after;
            }
            if (strlen(after) >= strlen("runner") && ascii_equal_nocase(after, "runner", strlen("runner"))) {
                return true;
            }
        }
        if ((size_t)(text + text_length - cursor) >= strlen("no se registró") &&
            ascii_equal_nocase(cursor, "no se registró", strlen("no se registró"))) {
            const char *line_end = strchr(cursor, '\n');
            if (line_end == NULL) {
                line_end = text + text_length;
            }
            if (span_contains_case_insensitive_ascii(cursor + strlen("no se registró"), line_end, "hardware")) {
                return true;
            }
        }
        ++cursor;
    }
    return false;
}

static void check_termux_runner_contract(void)
{
    static const char *const workflow_fragments[] = {
        "workflow_dispatch:", "confirm_device:", "if: inputs.confirm_device == true",
        "runs-on: [self-hosted, termux, aarch64, milena]", "TERMUX_PACKAGES_DIR", "build-package.sh",
        "-I -f milena", "termux-runner-preflight.sh", "make tools/check_repository_contracts",
        "tools/check_repository_contracts termux-recipe", "validate_termux_artifact.py",
        "tools/validate_termux_elf", "termux-real-smoke.sh",
        "pkg install/remove", "never run pkg upgrade", "upload-artifact@v4", "if-no-files-found: error"
    };
    static const char *const preflight_fragments[] = {
        "[[ \"$ARCH\" == aarch64 ]]", "[[ \"$DPKG_ARCH\" == aarch64 ]]",
        "[[ \"$PREFIX_DIR\" == */usr", "TERMUX_PACKAGES_DIR", "termux-info", "preflight.txt"
    };
    static const char *const documentation_labels[] = {"self-hosted", "termux", "aarch64", "milena", "build-package.sh"};
    StringList errors;
    size_t index;
    char *workflow;
    char *preflight;
    char *plan;
    list_init(&errors);
    workflow = optional_read_file(".github/workflows/termux-aarch64-contract.yml");
    if (workflow == NULL) {
        list_add(&errors, "missing manual Termux contract workflow");
    } else {
        for (index = 0U; index < ARRAY_COUNT(workflow_fragments); ++index) {
            if (!contains(workflow, workflow_fragments[index])) {
                list_addf(&errors, "workflow missing required contract: %s", workflow_fragments[index]);
            }
        }
        {
            const char *marker = "  termux-aarch64-contract:";
            const char *device = strstr(workflow, marker);
            device = device == NULL ? workflow + strlen(workflow) : device + strlen(marker);
            if (contains(device, "ubuntu-latest") || contains(device, "windows-latest")) {
                list_add(&errors, "Termux device contract cannot use a hosted generic runner");
            }
        }
        free(workflow);
    }
    preflight = optional_read_file("scripts/termux-runner-preflight.sh");
    if (preflight == NULL) {
        list_add(&errors, "missing Termux runner preflight");
    } else {
        for (index = 0U; index < ARRAY_COUNT(preflight_fragments); ++index) {
            if (!contains(preflight, preflight_fragments[index])) {
                list_addf(&errors, "preflight missing required check: %s", preflight_fragments[index]);
            }
        }
        free(preflight);
    }
    plan = optional_read_file("docs/TERMUX_VALIDATION_PLAN.md");
    if (plan == NULL) {
        list_add(&errors, "missing Termux validation documentation");
    } else {
        for (index = 0U; index < ARRAY_COUNT(documentation_labels); ++index) {
            if (!contains(plan, documentation_labels[index])) {
                list_addf(&errors, "documentation missing runner label/command: %s", documentation_labels[index]);
            }
        }
        if (!documents_unregistered_runner(plan)) {
            list_add(&errors, "documentation must state that no hardware was registered");
        }
    }
    free(plan);
    if (errors.count != 0U) {
        for (index = 0U; index < errors.count; ++index) {
            (void)fprintf(stderr, "ERROR: %s\n", errors.items[index]);
        }
        list_free(&errors);
        exit(EXIT_FAILURE);
    }
    (void)printf("Termux aarch64 runner contract: OK (manual, official build-package, non-emulated)\n");
    list_free(&errors);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        (void)fprintf(stderr, "usage: %s source-manifest|experimental-isolation|compiler-boundary|stream-architecture|markdown-links|termux-packaging|termux-recipe|termux-runner-contract|termux-industrial [paths...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "source-manifest") == 0) {
        check_source_manifest(argc, argv);
    } else if (strcmp(argv[1], "experimental-isolation") == 0) {
        check_experimental_isolation(argc, argv);
    } else if (strcmp(argv[1], "compiler-boundary") == 0) {
        check_compiler_boundary(argc, argv);
    } else if (strcmp(argv[1], "stream-architecture") == 0) {
        check_stream_architecture();
    } else if (strcmp(argv[1], "markdown-links") == 0) {
        check_markdown_links(argc, argv);
    } else if (strcmp(argv[1], "termux-packaging") == 0) {
        check_termux_packaging();
    } else if (strcmp(argv[1], "termux-recipe") == 0) {
        check_termux_recipe(argc, argv);
    } else if (strcmp(argv[1], "termux-runner-contract") == 0) {
        check_termux_runner_contract();
    } else if (strcmp(argv[1], "termux-industrial") == 0) {
        termux_industrial_check();
    } else {
        (void)fprintf(stderr, "unknown check mode: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
