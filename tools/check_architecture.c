/*
 * ISO C17 port of the canonical-route and HIR AST coverage repository checks.
 * Keep this checker dependency-free: it runs as part of the compiler test suite.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    const char *begin;
    const char *end;
} Span;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringSet;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

static const char *failure_prefix = "architecture check failed";

static void fail(const char *format, ...)
{
    va_list arguments;
    (void)fprintf(stderr, "%s: ", failure_prefix);
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
    (void)fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void need(bool condition, const char *message)
{
    if (!condition) {
        fail("%s", message);
    }
}

static bool ascii_word(unsigned char c)
{
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
           (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
           (c >= (unsigned char)'0' && c <= (unsigned char)'9') ||
           c == (unsigned char)'_';
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    size_t bytes_read;
    char *contents;

    if (file == NULL) {
        fail("required route file is missing or unreadable: %s", path);
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        fail("cannot seek required route file: %s", path);
    }
    length = ftell(file);
    if (length < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        fail("cannot determine required route file size: %s", path);
    }
    if ((uintmax_t)length >= (uintmax_t)SIZE_MAX) {
        (void)fclose(file);
        fail("required route file is too large: %s", path);
    }
    contents = (char *)malloc((size_t)length + 1U);
    if (contents == NULL) {
        (void)fclose(file);
        fail("out of memory while reading: %s", path);
    }
    bytes_read = fread(contents, 1U, (size_t)length, file);
    if (bytes_read != (size_t)length || ferror(file) != 0) {
        free(contents);
        (void)fclose(file);
        fail("cannot read required route file: %s", path);
    }
    contents[bytes_read] = '\0';
    if (fclose(file) != 0) {
        free(contents);
        fail("cannot close required route file: %s", path);
    }
    return contents;
}

static Span whole_span(const char *text)
{
    Span result;
    result.begin = text;
    result.end = text + strlen(text);
    return result;
}

static Span span_between(const char *begin, const char *end)
{
    Span result;
    result.begin = begin;
    result.end = end;
    return result;
}

static const char *span_find(Span text, const char *needle)
{
    size_t needle_length = strlen(needle);
    const char *cursor;

    if (needle_length == 0U) {
        return text.begin;
    }
    if (text.end < text.begin || (size_t)(text.end - text.begin) < needle_length) {
        return NULL;
    }
    for (cursor = text.begin;
         cursor <= text.end - needle_length;
         ++cursor) {
        if (memcmp(cursor, needle, needle_length) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static bool span_has(Span text, const char *needle)
{
    return span_find(text, needle) != NULL;
}

static const char *find_required(const char *text, const char *marker,
                                 const char *message)
{
    const char *found = strstr(text, marker);
    if (found == NULL) {
        fail("%s", message);
    }
    return found;
}

static Span function_body(const char *source, const char *marker,
                          const char *label)
{
    const char *start = strstr(source, marker);
    const char *opening;
    const char *cursor;
    size_t depth = 0U;
    enum { CODE, LINE_COMMENT, BLOCK_COMMENT, STRING_LITERAL, CHAR_LITERAL } state = CODE;

    if (start == NULL) {
        fail("missing %s entrypoint: %s", label, marker);
    }
    opening = strchr(start, '{');
    if (opening == NULL) {
        fail("missing function body for %s", label);
    }
    for (cursor = opening; *cursor != '\0'; ++cursor) {
        char current = *cursor;
        char next = cursor[1];

        if (state == LINE_COMMENT) {
            if (current == '\n') {
                state = CODE;
            }
        } else if (state == BLOCK_COMMENT) {
            if (current == '*' && next == '/') {
                state = CODE;
                ++cursor;
            }
        } else if (state == STRING_LITERAL || state == CHAR_LITERAL) {
            if (current == '\\') {
                if (next != '\0') {
                    ++cursor;
                }
            } else if ((state == STRING_LITERAL && current == '"') ||
                       (state == CHAR_LITERAL && current == '\'')) {
                state = CODE;
            }
        } else if (current == '/' && next == '/') {
            state = LINE_COMMENT;
            ++cursor;
        } else if (current == '/' && next == '*') {
            state = BLOCK_COMMENT;
            ++cursor;
        } else if (current == '"') {
            state = STRING_LITERAL;
        } else if (current == '\'') {
            state = CHAR_LITERAL;
        } else if (current == '{') {
            ++depth;
        } else if (current == '}') {
            if (depth == 0U) {
                fail("invalid brace depth in %s", label);
            }
            --depth;
            if (depth == 0U) {
                return span_between(opening + 1, cursor);
            }
        }
    }
    fail("unterminated %s body", label);
    return span_between(source, source);
}

static void ordered(Span text, const char *const *stages, size_t stage_count,
                    const char *label)
{
    const char *cursor = text.begin;
    size_t index;

    for (index = 0U; index < stage_count; ++index) {
        Span remainder = span_between(cursor, text.end);
        const char *found = span_find(remainder, stages[index]);
        if (found == NULL) {
            fail("%s is missing or reorders stage %s", label, stages[index]);
        }
        cursor = found + strlen(stages[index]);
    }
}

static void list_init(StringList *list)
{
    list->items = NULL;
    list->count = 0U;
    list->capacity = 0U;
}

static void list_add_n(StringList *list, const char *value, size_t length)
{
    char *copy;
    if (list->count == list->capacity) {
        size_t next_capacity = list->capacity == 0U ? 8U : list->capacity * 2U;
        char **next_items;
        if (next_capacity < list->capacity ||
            next_capacity > SIZE_MAX / sizeof(*list->items)) {
            fail("list capacity overflow");
        }
        next_items = (char **)realloc(list->items, next_capacity * sizeof(*list->items));
        if (next_items == NULL) {
            fail("out of memory while collecting source paths");
        }
        list->items = next_items;
        list->capacity = next_capacity;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        fail("out of memory while collecting source paths");
    }
    memcpy(copy, value, length);
    copy[length] = '\\0';
    list->items[list->count] = copy;
    ++list->count;
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

static void set_init(StringSet *set)
{
    set->items = NULL;
    set->count = 0U;
    set->capacity = 0U;
}

static bool set_contains_n(const StringSet *set, const char *value, size_t length)
{
    size_t index;
    for (index = 0U; index < set->count; ++index) {
        if (strlen(set->items[index]) == length &&
            memcmp(set->items[index], value, length) == 0) {
            return true;
        }
    }
    return false;
}

static bool set_contains(const StringSet *set, const char *value)
{
    return set_contains_n(set, value, strlen(value));
}

static void set_add_n(StringSet *set, const char *value, size_t length)
{
    char *copy;
    if (length == 0U || set_contains_n(set, value, length)) {
        return;
    }
    if (set->count == set->capacity) {
        size_t next_capacity = set->capacity == 0U ? 8U : set->capacity * 2U;
        char **next_items;
        if (next_capacity < set->capacity ||
            next_capacity > SIZE_MAX / sizeof(*set->items)) {
            fail("set capacity overflow");
        }
        next_items = (char **)realloc(set->items, next_capacity * sizeof(*set->items));
        if (next_items == NULL) {
            fail("out of memory while collecting AST nodes");
        }
        set->items = next_items;
        set->capacity = next_capacity;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        fail("out of memory while collecting AST nodes");
    }
    memcpy(copy, value, length);
    copy[length] = '\0';
    set->items[set->count] = copy;
    ++set->count;
}

static void set_add(StringSet *set, const char *value)
{
    set_add_n(set, value, strlen(value));
}

static void set_free(StringSet *set)
{
    size_t index;
    for (index = 0U; index < set->count; ++index) {
        free(set->items[index]);
    }
    free(set->items);
    set_init(set);
}

static bool set_equal(const StringSet *left, const StringSet *right)
{
    size_t index;
    if (left->count != right->count) {
        return false;
    }
    for (index = 0U; index < left->count; ++index) {
        if (!set_contains(right, left->items[index])) {
            return false;
        }
    }
    return true;
}

static bool sets_intersect(const StringSet *left, const StringSet *right)
{
    size_t index;
    for (index = 0U; index < left->count; ++index) {
        if (set_contains(right, left->items[index])) {
            return true;
        }
    }
    return false;
}

static void set_difference(const StringSet *left, const StringSet *right,
                           StringSet *result)
{
    size_t index;
    set_init(result);
    for (index = 0U; index < left->count; ++index) {
        if (!set_contains(right, left->items[index])) {
            set_add(result, left->items[index]);
        }
    }
}

static void set_union(const StringSet *left, const StringSet *right,
                      StringSet *result)
{
    size_t index;
    set_init(result);
    for (index = 0U; index < left->count; ++index) {
        set_add(result, left->items[index]);
    }
    for (index = 0U; index < right->count; ++index) {
        set_add(result, right->items[index]);
    }
}

static char *set_description(const StringSet *set)
{
    size_t total = 1U;
    size_t index;
    char *description;
    char *cursor;

    for (index = 0U; index < set->count; ++index) {
        size_t length = strlen(set->items[index]);
        if (length > SIZE_MAX - total - 2U) {
            fail("set description is too large");
        }
        total += length + (index == 0U ? 0U : 2U);
    }
    description = (char *)malloc(total);
    if (description == NULL) {
        fail("out of memory while formatting AST inventory");
    }
    cursor = description;
    for (index = 0U; index < set->count; ++index) {
        size_t length = strlen(set->items[index]);
        if (index != 0U) {
            *cursor++ = ',';
            *cursor++ = ' ';
        }
        memcpy(cursor, set->items[index], length);
        cursor += length;
    }
    *cursor = '\0';
    return description;
}

static bool ast_token_start(const char *cursor, const char *begin)
{
    return cursor >= begin &&
           (cursor == begin || !ascii_word((unsigned char)cursor[-1])) &&
           memcmp(cursor, "AST_", 4U) == 0;
}

static bool ast_identifier_char(unsigned char c)
{
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
           (c >= (unsigned char)'0' && c <= (unsigned char)'9') ||
           c == (unsigned char)'_';
}

static void collect_ast_tokens(Span text, StringSet *set)
{
    const char *cursor;
    for (cursor = text.begin; cursor + 4 <= text.end; ++cursor) {
        const char *end;
        if (!ast_token_start(cursor, text.begin)) {
            continue;
        }
        end = cursor + 4;
        while (end < text.end && ast_identifier_char((unsigned char)*end)) {
            ++end;
        }
        if (end == text.end || !ascii_word((unsigned char)*end)) {
            set_add_n(set, cursor, (size_t)(end - cursor));
        }
        cursor = end - 1;
    }
}

static void collect_case_labels(Span body, StringSet *set)
{
    const char *cursor;
    for (cursor = body.begin; cursor < body.end; ++cursor) {
        const char *node;
        const char *identifier_end;
        const char *end;
        if ((size_t)(body.end - cursor) < 4U || memcmp(cursor, "case", 4U) != 0 ||
            (cursor != body.begin && ascii_word((unsigned char)cursor[-1])) ||
            (cursor + 4 < body.end && ascii_word((unsigned char)cursor[4]))) {
            continue;
        }
        node = cursor + 4;
        while (node < body.end && isspace((unsigned char)*node) != 0) {
            ++node;
        }
        if ((size_t)(body.end - node) < 4U || !ast_token_start(node, body.begin)) {
            continue;
        }
        end = node + 4;
        while (end < body.end && ast_identifier_char((unsigned char)*end)) {
            ++end;
        }
        if (end < body.end && ascii_word((unsigned char)*end)) {
            continue;
        }
        identifier_end = end;
        while (end < body.end && isspace((unsigned char)*end) != 0) {
            ++end;
        }
        if (end < body.end && *end == ':') {
            set_add_n(set, node, (size_t)(identifier_end - node));
        }
    }
}

/* Return the start of the first typedef enum whose declared name is wanted. */
static Span ast_enum_body(const char *header)
{
    const char *cursor = header;
    while ((cursor = strstr(cursor, "typedef")) != NULL) {
        const char *word = cursor + strlen("typedef");
        const char *opening;
        const char *closing;
        const char *name;
        const char *semicolon;
        while (*word != '\0' && isspace((unsigned char)*word) != 0) {
            ++word;
        }
        if (strncmp(word, "enum", 4U) != 0 || ascii_word((unsigned char)word[4])) {
            cursor += strlen("typedef");
            continue;
        }
        word += 4;
        while (*word != '\0' && isspace((unsigned char)*word) != 0) {
            ++word;
        }
        if (*word != '{') {
            cursor += strlen("typedef");
            continue;
        }
        opening = word;
        closing = strchr(opening + 1, '}');
        if (closing == NULL) {
            fail("could not locate ASTNodeType enum");
        }
        name = closing + 1;
        while (*name != '\0' && isspace((unsigned char)*name) != 0) {
            ++name;
        }
        if (strncmp(name, "ASTNodeType", strlen("ASTNodeType")) == 0 &&
            !ascii_word((unsigned char)name[strlen("ASTNodeType")])) {
            name += strlen("ASTNodeType");
            while (*name != '\0' && isspace((unsigned char)*name) != 0) {
                ++name;
            }
            semicolon = name;
            if (*semicolon == ';') {
                return span_between(opening + 1, closing);
            }
        }
        cursor += strlen("typedef");
    }
    fail("could not locate ASTNodeType enum");
    return span_between(header, header);
}

static const char *line_start(const char *source, const char *position)
{
    const char *cursor = position;
    while (cursor > source && cursor[-1] != '\n') {
        --cursor;
    }
    return cursor;
}

static const char *next_line(const char *position)
{
    const char *cursor = strchr(position, '\n');
    return cursor == NULL ? position + strlen(position) : cursor + 1;
}

static bool line_assignment(const char *line, const char *name,
                            const char **value_start)
{
    const char *cursor = line;
    size_t name_length = strlen(name);
    if (strncmp(cursor, name, name_length) != 0) {
        return false;
    }
    cursor += name_length;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r') {
        ++cursor;
    }
    if (*cursor != '=') {
        return false;
    }
    ++cursor;
    while (isspace((unsigned char)*cursor) != 0) {
        ++cursor;
    }
    *value_start = cursor;
    return true;
}

static Span make_sources_span(const char *makefile)
{
    const char *cursor = makefile;
    const char *value_start = NULL;
    const char *source_line = NULL;
    const char *end_line = NULL;

    while (*cursor != '\0') {
        if (line_assignment(cursor, "SOURCES", &value_start)) {
            source_line = cursor;
            break;
        }
        cursor = next_line(cursor);
    }
    if (source_line == NULL) {
        fail("cannot read canonical product source list");
    }
    cursor = next_line(source_line);
    while (*cursor != '\0') {
        const char *ignored;
        if (line_assignment(cursor, "OBJECTS", &ignored)) {
            end_line = cursor;
            break;
        }
        cursor = next_line(cursor);
    }
    if (end_line == NULL || end_line < value_start) {
        fail("cannot read canonical product source list");
    }
    return span_between(value_start, end_line);
}

static bool path_char(unsigned char c)
{
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
           (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
           (c >= (unsigned char)'0' && c <= (unsigned char)'9') ||
           c == (unsigned char)'_' || c == (unsigned char)'.' ||
           c == (unsigned char)'/' || c == (unsigned char)'-';
}

static bool span_starts_with(const char *cursor, const char *end, const char *prefix)
{
    size_t length = strlen(prefix);
    return (size_t)(end - cursor) >= length && memcmp(cursor, prefix, length) == 0;
}

static void collect_source_paths(Span text, StringList *sources)
{
    const char *cursor;
    for (cursor = text.begin; cursor < text.end; ++cursor) {
        const char *run_end;
        const char *candidate_end = NULL;
        if (!span_starts_with(cursor, text.end, "src/") &&
            !span_starts_with(cursor, text.end, "third_party/")) {
            continue;
        }
        if (cursor != text.begin && path_char((unsigned char)cursor[-1])) {
            continue;
        }
        run_end = cursor;
        while (run_end < text.end && path_char((unsigned char)*run_end)) {
            if ((size_t)(text.end - run_end) >= 2U && run_end[0] == '.' && run_end[1] == 'c') {
                candidate_end = run_end + 2;
            }
            ++run_end;
        }
        if (candidate_end != NULL) {
            list_add_n(sources, cursor, (size_t)(candidate_end - cursor));
        }
        cursor = run_end - 1;
    }
}

static void collect_object_paths(Span line, StringList *objects)
{
    const char *cursor;
    for (cursor = line.begin; cursor < line.end; ++cursor) {
        const char *end;
        const char *candidate_end = NULL;
        if (!span_starts_with(cursor, line.end, "src/")) {
            continue;
        }
        if (cursor != line.begin && path_char((unsigned char)cursor[-1])) {
            continue;
        }
        end = cursor;
        while (end < line.end && path_char((unsigned char)*end)) {
            if ((size_t)(line.end - end) >= 2U && end[0] == '.' && end[1] == 'o') {
                candidate_end = end + 2;
            }
            ++end;
        }
        if (candidate_end != NULL) {
            list_add_n(objects, cursor, (size_t)(candidate_end - cursor));
        }
        cursor = end - 1;
    }
}

static bool line_exact_target(const char *makefile, const char *target,
                              const char *dependency)
{
    const char *line = makefile;
    size_t target_length = strlen(target);
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *cursor;
        size_t length = end == NULL ? strlen(line) : (size_t)(end - line);
        if (length >= target_length && memcmp(line, target, target_length) == 0) {
            cursor = line + target_length;
            while (cursor < line + length && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) {
                ++cursor;
            }
            if (cursor < line + length && *cursor == ':') {
                ++cursor;
                while (cursor < line + length && isspace((unsigned char)*cursor) != 0) {
                    ++cursor;
                }
                if (span_starts_with(cursor, line + length, dependency)) {
                    cursor += strlen(dependency);
                    while (cursor < line + length && isspace((unsigned char)*cursor) != 0) {
                        ++cursor;
                    }
                    if (cursor == line + length) {
                        return true;
                    }
                }
            }
        }
        line = end == NULL ? line + length : end + 1;
    }
    return false;
}

static bool test_target_contains(const char *makefile, const char *dependency)
{
    const char *line = makefile;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t length = end == NULL ? strlen(line) : (size_t)(end - line);
        if (length >= 5U && memcmp(line, "test:", 5U) == 0 &&
            span_has(span_between(line, line + length), dependency)) {
            return true;
        }
        line = end == NULL ? line + length : end + 1;
    }
    return false;
}

static void check_architecture(void)
{
    static const char *const required_sources[] = {
        "src/main.c", "src/entrypoints.c", "src/script.c", "src/lexer.c",
        "src/parser.c", "src/ast.c", "src/language_semantic.c",
        "src/language_runtime.c", "src/canonical_compiler.c", "src/query_plan.c"
    };
    static const char *const frontend_stages[] = {
        "lexer_init(", "parser_init(", "parser_parse(", "milena_validate_ast("
    };
    static const char *const dataset_adapter_stages[] = {
        "cli_frontend(", "dataset_load_csv("
    };
    static const char *const script_stages[] = {
        "script_pipeline_for_source(", "SCRIPT_PIPELINE_PARSE_ERROR",
        "SCRIPT_PIPELINE_CANONICAL_ARRAY", "SCRIPT_PIPELINE_CANONICAL_DATASET",
        "Desde aquí comienza únicamente la ruta histórica de compatibilidad", "parse_schema("
    };
    static const char *const dataset_frontend_stages[] = {
        "lexer_init(", "parser_init(", "parser_parse(", "milena_validate_ast("
    };
    static const char *const data_hir_stages[] = {
        "milena_validate_ast(", "milena_canonical_program_parse(",
        "milena_canonical_program_bind_table(", "milena_canonical_compiler_input(",
        "milena_canonical_program_execute_data("
    };
    static const char *const typed_planner_tokens[] = {
        "milena_stream_execution_plan_build(", "milena_arrow_ipc_execution_plan_build(",
        "milena_sql_execution_plan_build(", "milena_canonical_program_parse(",
        "milena_canonical_program_bind_table(", "milena_canonical_compiler_input(",
        "milena_canonical_program_execute_data(", "milena_sql_run_plan(",
        "milena_arrow_ipc_stream_transform(", "run_stream_dataset_with_options("
    };
    static const char *const planner_validation_tokens[] = {
        "milena_stream_execution_plan_validate(plan, error)",
        "milena_arrow_ipc_execution_plan_validate(plan, error)",
        "milena_sql_execution_plan_validate(plan, error)", "MILENA_LOGICAL_CSV_SCAN",
        "MILENA_LOGICAL_JSON_REPORT"
    };
    static const char *const hir_tokens[] = {
        "milena_canonical_program_parse(", "data_hir_build(",
        "milena_canonical_program_execute_data(", "MILENA_ERR_UNSUPPORTED"
    };
    static const char *const contract_tokens[] = {
        "## Ruta canónica", "## Ramas tipadas actuales", "## Excepciones de compatibilidad",
        "## Límites que siguen abiertos", "## Tres reglas no negociables",
        "lexer → parser → AST tipado → semántica → HIR → plan físico → runtime/backend",
        "function_parser.c", "script.c", "entrypoints.c", "rehace el parseo", "HIR universal"
    };
    static const char *const main_forbidden[] = {
        "lexer_init(", "parser_init(", "parser_parse(", "dataset_load_csv(",
        "milena_run_dataset_program("
    };
    static const char *const cli_commands[] = {
        "analyze", "profile", "inspect", "run_script"
    };
    static const char *const script_tokens[] = {
        "script_pipeline_from_ast", "script_pipeline_for_source", "SCRIPT_PIPELINE_PARSE_ERROR",
        "script_has_canonical_marker", "milena_run_array_program", "milena_run_dataset_program",
        "run_canonical_functions", "run_legacy_numeric_functions",
        "Desde aquí comienza únicamente la ruta histórica de compatibilidad"
    };
    const char *makefile = read_file("Makefile");
    const char *main_source;
    const char *entry_source;
    const char *script_source;
    const char *runtime_source;
    const char *planner_source;
    const char *hir_source;
    const char *contract;
    const char *legacy_line = NULL;
    const char *legacy_value_start = NULL;
    const char *line;
    Span source_region;
    Span cli_frontend;
    Span cli_dataset;
    Span script_router;
    Span legacy_adapter;
    Span data_runtime;
    StringList sources;
    StringList entrypoints;
    StringList parsers;
    StringList legacy_objects;
    size_t index;
    size_t lexer_count = 0U;

    failure_prefix = "unification architecture check failed";
    list_init(&sources);
    list_init(&entrypoints);
    list_init(&parsers);
    list_init(&legacy_objects);
    source_region = make_sources_span(makefile);
    collect_source_paths(source_region, &sources);
    for (index = 0U; index < sources.count; ++index) {
        const char *path = sources.items[index];
        const char *base = strrchr(path, '/');
        const char *dot = strrchr(path, '.');
        size_t stem_length;
        if (strncmp(path, "src/", 4U) != 0) {
            continue;
        }
        base = base == NULL ? path : base + 1;
        dot = strrchr(base, '.');
        stem_length = dot == NULL ? strlen(base) : (size_t)(dot - base);
        if ((stem_length == 4U && memcmp(base, "main", 4U) == 0) ||
            (stem_length >= 5U && memcmp(base + stem_length - 5U, "_main", 5U) == 0)) {
            list_add_n(&entrypoints, path, strlen(path));
        }
        if (strlen(base) >= strlen("parser.c") &&
            strcmp(base + strlen(base) - strlen("parser.c"), "parser.c") == 0) {
            list_add_n(&parsers, path, strlen(path));
        }
        if (strcmp(path, "src/lexer.c") == 0) {
            ++lexer_count;
        }
    }
    need(entrypoints.count == 1U && strcmp(entrypoints.items[0], "src/main.c") == 0,
         "parallel product entrypoint found or canonical src/main.c missing");
    need(parsers.count == 1U && strcmp(parsers.items[0], "src/parser.c") == 0,
         "parallel language parser found or canonical src/parser.c missing");
    need(lexer_count == 1U, "product must compile exactly one canonical lexer");
    for (index = 0U; index < ARRAY_COUNT(required_sources); ++index) {
        size_t count = 0U;
        size_t source_index;
        for (source_index = 0U; source_index < sources.count; ++source_index) {
            if (strcmp(sources.items[source_index], required_sources[index]) == 0) {
                ++count;
            }
        }
        if (count != 1U) {
            fail("canonical pipeline source must occur once: %s", required_sources[index]);
        }
    }

    line = makefile;
    while (*line != '\0') {
        if (line_assignment(line, "FUNCTION_OBJECTS", &legacy_value_start)) {
            legacy_line = line;
            break;
        }
        line = next_line(line);
    }
    need(legacy_line != NULL, "legacy function-parser boundary disappeared");
    {
        const char *value_end = strchr(legacy_value_start, '\n');
        Span object_line = span_between(legacy_value_start,
                                        value_end == NULL ? legacy_value_start + strlen(legacy_value_start) : value_end);
        collect_object_paths(object_line, &legacy_objects);
    }
    need(legacy_objects.count == 2U &&
         strcmp(legacy_objects.items[0], "src/function_parser.o") == 0 &&
         strcmp(legacy_objects.items[1], "src/user_functions.o") == 0,
         "unexpected compatibility objects in FUNCTION_OBJECTS");

    main_source = read_file("src/main.c");
    for (index = 0U; index < ARRAY_COUNT(main_forbidden); ++index) {
        if (strstr(main_source, main_forbidden[index]) != NULL) {
            fail("main.c contains a parallel frontend/backend: %s", main_forbidden[index]);
        }
    }
    for (index = 0U; index < ARRAY_COUNT(cli_commands); ++index) {
        char token[64];
        int written = snprintf(token, sizeof(token), "milena_cli_%s", cli_commands[index]);
        if (written < 0 || (size_t)written >= sizeof(token)) {
            fail("cannot format CLI adapter name");
        }
        if (strstr(main_source, token) == NULL) {
            fail("CLI command bypasses its entrypoint adapter: %s", cli_commands[index]);
        }
    }

    entry_source = read_file("src/entrypoints.c");
    cli_frontend = function_body(entry_source, "cli_frontend(", "CLI frontend");
    ordered(cli_frontend, frontend_stages, ARRAY_COUNT(frontend_stages), "CLI frontend");
    cli_dataset = function_body(entry_source, "cli_load_dataset(", "CLI dataset adapter");
    ordered(cli_dataset, dataset_adapter_stages, ARRAY_COUNT(dataset_adapter_stages),
            "CLI dataset adapter");

    script_source = read_file("src/script.c");
    for (index = 0U; index < ARRAY_COUNT(script_tokens); ++index) {
        if (strstr(script_source, script_tokens[index]) == NULL) {
            fail("script router lost explicit boundary: %s", script_tokens[index]);
        }
    }
    script_router = function_body(script_source, "MilenaStatus milena_run_script(",
                                  "script runtime");
    ordered(script_router, script_stages, ARRAY_COUNT(script_stages), "script router");
    legacy_adapter = function_body(script_source,
                                   "static MilenaStatus run_legacy_numeric_functions(",
                                   "legacy function adapter");
    need(span_has(legacy_adapter, "milena_parse_numeric_functions("),
         "legacy parser escaped its compatibility adapter");

    runtime_source = read_file("src/language_runtime.c");
    data_runtime = function_body(runtime_source,
                                 "MilenaStatus milena_run_dataset_program(",
                                 "canonical dataset runtime");
    ordered(data_runtime, dataset_frontend_stages, "dataset frontend");
    for (index = 0U; index < ARRAY_COUNT(typed_planner_tokens); ++index) {
        if (!span_has(data_runtime, typed_planner_tokens[index])) {
            fail("canonical runtime bypasses a typed planner/runtime stage: %s",
                 typed_planner_tokens[index]);
        }
    }
    ordered(data_runtime, data_hir_stages, ARRAY_COUNT(data_hir_stages), "data-HIR runtime");

    planner_source = read_file("src/query_plan.c");
    for (index = 0U; index < ARRAY_COUNT(planner_validation_tokens); ++index) {
        if (strstr(planner_source, planner_validation_tokens[index]) == NULL) {
            fail("typed planner lacks validation/operator: %s",
                 planner_validation_tokens[index]);
        }
    }
    need(strstr(planner_source, "main(") == NULL,
         "query planner must not define another executable");
    hir_source = read_file("src/canonical_compiler.c");
    for (index = 0U; index < ARRAY_COUNT(hir_tokens); ++index) {
        if (strstr(hir_source, hir_tokens[index]) == NULL) {
            fail("canonical HIR boundary is incomplete: %s", hir_tokens[index]);
        }
    }

    need(line_exact_target(makefile, "check-unification-architecture",
                           "check-hir-ast-coverage"),
         "architecture check must depend on closed-HIR coverage");
    need(test_target_contains(makefile, "check-unification-architecture"),
         "make test must execute the architecture check");
    contract = read_file("docs/CANONICAL_PIPELINE_CONTRACT.md");
    for (index = 0U; index < ARRAY_COUNT(contract_tokens); ++index) {
        if (strstr(contract, contract_tokens[index]) == NULL) {
            fail("pipeline contract omits boundary: %s", contract_tokens[index]);
        }
    }

    (void)puts("OK: una entrada, un lexer/parser, HIR cerrado, planificadores tipados y compatibilidad explícita");
    list_free(&sources);
    list_free(&entrypoints);
    list_free(&parsers);
    list_free(&legacy_objects);
    free((void *)makefile);
    free((void *)main_source);
    free((void *)entry_source);
    free((void *)script_source);
    free((void *)runtime_source);
    free((void *)planner_source);
    free((void *)hir_source);
    free((void *)contract);
}

static Span required_section(const char *document, const char *start_marker,
                             const char *end_marker, const char *message)
{
    const char *start = strstr(document, start_marker);
    const char *end;
    if (start == NULL) {
        fail("%s", message);
    }
    end = strstr(start + strlen(start_marker), end_marker);
    if (end == NULL) {
        fail("%s", message);
    }
    return span_between(start + strlen(start_marker), end);
}

static void collect_ast_enum_nodes(Span body, StringSet *nodes)
{
    collect_ast_tokens(body, nodes);
    {
        StringSet without_count;
        size_t index;
        set_init(&without_count);
        for (index = 0U; index < nodes->count; ++index) {
            if (strcmp(nodes->items[index], "AST_NODE_TYPE_COUNT") != 0) {
                set_add(&without_count, nodes->items[index]);
            }
        }
        set_free(nodes);
        *nodes = without_count;
    }
}

static void require_equal_sets(const StringSet *expected, const StringSet *actual,
                               const char *message)
{
    if (!set_equal(expected, actual)) {
        StringSet missing;
        StringSet extra;
        char *missing_text;
        char *extra_text;
        set_difference(expected, actual, &missing);
        set_difference(actual, expected, &extra);
        missing_text = set_description(&missing);
        extra_text = set_description(&extra);
        fail("%s; missing=%s; extra=%s", message, missing_text, extra_text);
    }
}

static void check_hir_coverage(void)
{
    static const char scalar_start[] = "El subconjunto representado por el HIR escalar es:";
    static const char scalar_rejected[] = "Todos los demás tipos declarados en `ASTNodeType`";
    static const char scalar_end[] = "Esto cubre las familias";
    static const char data_start[] = "La matriz de nodos realmente recorridos por el builder de datos es:";
    static const char data_end[] = "Todo AST restante está fuera de la HIR de datos";
    const char *header;
    const char *implementation;
    const char *documentation;
    const char *represented_start;
    const char *rejected_start;
    const char *rejected_end;
    Span enum_body;
    Span represented_section;
    Span rejected_section;
    Span data_section;
    Span eligibility_body;
    Span data_eligibility_body;
    Span data_builder_body;
    Span expression_builder_body;
    Span statement_builder_body;
    Span hir_entry_body;
    StringSet ast_nodes;
    StringSet represented;
    StringSet rejected;
    StringSet union_nodes;
    StringSet eligibility_nodes;
    StringSet data_represented;
    StringSet data_eligibility_nodes;
    StringSet data_builder_nodes;
    StringSet expression_nodes;
    StringSet statement_nodes;
    StringSet expression_union;
    size_t index;
    StringSet unknown_data;

    failure_prefix = "HIR AST coverage check failed";
    set_init(&ast_nodes);
    set_init(&represented);
    set_init(&rejected);
    set_init(&union_nodes);
    set_init(&eligibility_nodes);
    set_init(&data_represented);
    set_init(&data_eligibility_nodes);
    set_init(&data_builder_nodes);
    set_init(&expression_nodes);
    set_init(&statement_nodes);
    set_init(&expression_union);
    set_init(&unknown_data);

    header = read_file("include/ast.h");
    implementation = read_file("src/canonical_compiler.c");
    documentation = read_file("docs/COMPILADOR_IR_PLAN.md");
    enum_body = ast_enum_body(header);
    collect_ast_enum_nodes(enum_body, &ast_nodes);

    represented_start = strstr(documentation, scalar_start);
    if (represented_start == NULL) {
        fail("documentation must keep explicit represented/rejected AST inventories");
    }
    rejected_start = strstr(represented_start + strlen(scalar_start), scalar_rejected);
    if (rejected_start == NULL) {
        fail("documentation must keep explicit represented/rejected AST inventories");
    }
    rejected_end = strstr(rejected_start + strlen(scalar_rejected), scalar_end);
    if (rejected_end == NULL) {
        fail("documentation must keep explicit represented/rejected AST inventories");
    }
    represented_section = span_between(represented_start, rejected_start);
    rejected_section = span_between(rejected_start, rejected_end);
    collect_ast_tokens(represented_section, &represented);
    collect_ast_tokens(rejected_section, &rejected);
    need(!sets_intersect(&represented, &rejected),
         "AST nodes classified both represented and rejected");
    set_union(&represented, &rejected, &union_nodes);
    require_equal_sets(&ast_nodes, &union_nodes,
                       "documentation does not classify the exact AST enum");

    eligibility_body = function_body(implementation,
                                     "static bool hir_supports_ast_node(",
                                     "scalar HIR eligibility");
    collect_case_labels(eligibility_body, &eligibility_nodes);
    require_equal_sets(&represented, &eligibility_nodes,
                       "documented HIR subset differs from explicit eligibility allowlist");
    need(span_has(eligibility_body, "default:") &&
         span_has(eligibility_body, "return false;"),
         "unknown AST kinds must be rejected by the eligibility default");

    data_section = required_section(documentation, data_start, data_end,
                                    "documentation must list the exact data-HIR builder inventory");
    collect_ast_tokens(data_section, &data_represented);
    set_difference(&data_represented, &ast_nodes, &unknown_data);
    need(unknown_data.count == 0U,
         "data-HIR inventory contains unknown AST nodes");
    data_eligibility_body = function_body(implementation,
                                         "static bool hir_supports_data_ast_node(",
                                         "data HIR eligibility");
    collect_case_labels(data_eligibility_body, &data_eligibility_nodes);
    require_equal_sets(&data_represented, &data_eligibility_nodes,
                       "documented data-HIR subset differs from its strict-entry allowlist");
    need(span_has(data_eligibility_body, "default:") &&
         span_has(data_eligibility_body, "return false;"),
         "unknown data AST kinds must be rejected by the eligibility default");
    data_builder_body = function_body(implementation,
                                      "static HIRBuildResult data_hir_build(const ASTNode *ast, MilenaDataHIR **output) {",
                                      "data HIR builder");
    collect_ast_tokens(data_builder_body, &data_builder_nodes);
    require_equal_sets(&data_represented, &data_builder_nodes,
                       "documented data-HIR subset differs from AST nodes handled by the builder");

    expression_builder_body = function_body(implementation,
                                            "static MilenaHIRExpression *hir_build_expression(const ASTNode *node,",
                                            "scalar expression builder");
    statement_builder_body = function_body(implementation,
                                          "static MilenaHIRStatement *hir_build_statement(const ASTNode *node,\n                                               HIRBuildResult *result) {",
                                          "scalar statement builder");
    collect_case_labels(expression_builder_body, &expression_nodes);
    collect_case_labels(statement_builder_body, &statement_nodes);
    set_union(&expression_nodes, &statement_nodes, &expression_union);
    set_difference(&expression_union, &represented, &unknown_data);
    need(unknown_data.count == 0U,
         "an expression/statement builder case is absent from the represented inventory");
    need(strstr(implementation, "node->type != AST_DECLARACION_FUNCION") != NULL,
         "function declarations must remain handled by the scalar HIR builder");

    hir_entry_body = function_body(implementation,
                                   "MilenaStatus milena_canonical_hir_input(",
                                   "public HIR-only entry point");
    need(span_has(hir_entry_body, "MILENA_ERR_UNSUPPORTED") &&
         span_has(hir_entry_body, "hir_first_unsupported_node"),
         "the public HIR-only entry point must fail closed and locate an unsupported node");

    (void)printf("HIR AST coverage: scalar=%lu represented/%lu outside; data=%lu represented/%lu fail-closed; all ASTNodeType values classified in both closed subsets.\n",
                 (unsigned long)represented.count,
                 (unsigned long)rejected.count,
                 (unsigned long)data_represented.count,
                 (unsigned long)(ast_nodes.count - data_represented.count));

    set_free(&ast_nodes);
    set_free(&represented);
    set_free(&rejected);
    set_free(&union_nodes);
    set_free(&eligibility_nodes);
    set_free(&data_represented);
    set_free(&data_eligibility_nodes);
    set_free(&data_builder_nodes);
    set_free(&expression_nodes);
    set_free(&statement_nodes);
    set_free(&expression_union);
    set_free(&unknown_data);
    free((void *)header);
    free((void *)implementation);
    free((void *)documentation);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s hir|architecture\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "hir") == 0) {
        check_hir_coverage();
    } else if (strcmp(argv[1], "architecture") == 0) {
        check_architecture();
    } else {
        (void)fprintf(stderr, "unknown check mode: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
