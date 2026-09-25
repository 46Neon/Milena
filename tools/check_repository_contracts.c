/* ISO C17 ports of repository source-boundary and streaming architecture checks. */
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    "analysis.c", "array.c", "arrow_ipc.c", "canonical_compiler.c", "common.c",
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
    "ir.c", "module.c", "semantic.c", "temp_scope.c", "vm.c"
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
    (void)printf("Compiler/IR/VM boundary: OK (experimental modules excluded from official SOURCES)\n");
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
        static const char *const forbidden_tokens[] = {"compiler.c", "ir.c", "vm.c", "gc.c", "arena.c"};
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        (void)fprintf(stderr, "usage: %s source-manifest|experimental-isolation|compiler-boundary|stream-architecture [src/*.c...]\n", argv[0]);
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
    } else {
        (void)fprintf(stderr, "unknown check mode: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
