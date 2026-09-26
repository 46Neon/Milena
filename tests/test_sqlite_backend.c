#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "sqlite_backend.h"
#include "query_plan.h"
#include "ast.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define DB_PATH "tests/sqlite-backend-test.db"

static void run(MilenaSqlConnection *db, const char *sql,
                const MilenaSqlValue *values, size_t count, MilenaTable *table,
                MilenaStatus expected) {
    MilenaError error;
    MilenaSqlLimits limits = milena_sql_default_limits();
    MilenaStatus status = milena_sql_execute(db, sql, values, count, &limits,
                                              table, &error);
    if (status != expected) {
        fprintf(stderr, "SQLite test: unexpected status %s (%s) for test query\n",
                milena_status_name(status), error.message);
        assert(status == expected);
    }
}

static size_t scalar_count(MilenaSqlConnection *db, const char *sql) {
    MilenaTable table;
    milena_table_init(&table);
    run(db, sql, NULL, 0, &table, MILENA_OK);
    assert(table.row_count == 1 && table.column_count == 1);
    const void *value = NULL;
    MilenaError error;
    assert(milena_table_get_array_value(&table, 0, 0, &value, &error) == MILENA_OK);
    int64_t result = *(const int64_t *)value;
    milena_table_destroy(&table);
    return (size_t)result;
}

static void check_typed_sql_plan(void) {
    ASTNode *program = ast_create_leaf(AST_SQL_PROGRAM, DB_PATH);
    ASTNode *query = ast_create_leaf(AST_SQL_QUERY,
        "SELECT id, note FROM records WHERE id=?");
    ASTNode *parameter = ast_create_leaf(AST_SQL_PARAMETER, "7");
    assert(program && query && parameter);
    parameter->type_name = milena_strdup("entero");
    program->sql_max_rows = 17; program->sql_max_bytes = 4096;
    program->sql_timeout_ms = 250; program->sql_limits_explicit = true;
    assert(parameter->type_name && ast_add_child(query, parameter) && ast_add_child(program, query));
    MilenaSqlExecutionPlan plan = {0}; MilenaError error;
    assert(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_OK);
    assert(milena_sql_execution_plan_validate(&plan, &error) == MILENA_OK);
    assert(strcmp(plan.connection_path, DB_PATH) == 0 && plan.explicit_limits);
    assert(plan.max_rows == 17 && plan.max_bytes == 4096 && plan.timeout_ms == 250);
    assert(plan.operation_count == 1 && plan.operations[0].kind == MILENA_SQL_PLAN_QUERY);
    const MilenaSqlPlanOperation *op = &plan.operations[0];
    assert(op->parameter_count == 1 && op->parameters[0].kind == MILENA_SQL_PLAN_INT64);
    assert(op->parameters[0].value.i64 == 7);
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(program);
}

static ASTNode *test_sql_parameter(const char *value, const char *type_name,
                                   ASTSqlType sql_type) {
    ASTNode *parameter = ast_create_leaf(AST_SQL_PARAMETER, value);
    if (!parameter) return NULL;
    parameter->type_name = milena_strdup(type_name);
    parameter->sql_type = sql_type;
    if (!parameter->type_name) { ast_destroy(parameter); return NULL; }
    return parameter;
}

static void check_typed_insert_execution(MilenaSqlConnection *db) {
    const char *payload = "typed data'); DROP TABLE records; --";
    ASTNode *program = ast_create_leaf(AST_SQL_PROGRAM, DB_PATH);
    ASTNode *schema = ast_create_leaf(AST_SQL_TABLE_SCHEMA, "records");
    ASTNode *id_schema = ast_create_leaf(AST_SQL_SCHEMA_COLUMN, "id");
    ASTNode *note_schema = ast_create_leaf(AST_SQL_SCHEMA_COLUMN, "note");
    ASTNode *maybe_schema = ast_create_leaf(AST_SQL_SCHEMA_COLUMN, "maybe");
    ASTNode *begin = ast_create(AST_SQL_BEGIN);
    ASTNode *insert = ast_create(AST_SQL_TYPED_INSERT);
    ASTNode *table = ast_create_leaf(AST_SQL_TABLE_REFERENCE, "records");
    ASTNode *columns = ast_create(AST_SQL_INSERT_COLUMN_LIST);
    ASTNode *id_column = ast_create_leaf(AST_SQL_INSERT_COLUMN, "id");
    ASTNode *note_column = ast_create_leaf(AST_SQL_INSERT_COLUMN, "note");
    ASTNode *values = ast_create(AST_SQL_INSERT_VALUE_LIST);
    ASTNode *id_value = test_sql_parameter("123", "entero", AST_SQL_TYPE_INTEGER);
    ASTNode *note_value = test_sql_parameter(payload, "texto", AST_SQL_TYPE_TEXT);
    ASTNode *commit = ast_create(AST_SQL_COMMIT);
    assert(program && schema && id_schema && note_schema && maybe_schema && begin && insert && table &&
           columns && id_column && note_column && values && id_value && note_value && commit);
    id_schema->type_name = milena_strdup("entero");
    note_schema->type_name = milena_strdup("texto");
    maybe_schema->type_name = milena_strdup("booleano");
    id_schema->sql_type = AST_SQL_TYPE_INTEGER;
    note_schema->sql_type = AST_SQL_TYPE_TEXT;
    maybe_schema->sql_type = AST_SQL_TYPE_BOOLEAN;
    assert(id_schema->type_name && note_schema->type_name && maybe_schema->type_name &&
           ast_add_child(schema, id_schema) && ast_add_child(schema, note_schema) &&
           ast_add_child(schema, maybe_schema) &&
           ast_add_child(program, schema) && ast_add_child(program, begin) &&
           ast_add_child(columns, id_column) && ast_add_child(columns, note_column) &&
           ast_add_child(values, id_value) && ast_add_child(values, note_value) &&
           ast_add_child(insert, table) && ast_add_child(insert, columns) &&
           ast_add_child(insert, values) && ast_add_child(program, insert) &&
           ast_add_child(program, commit));

    MilenaSqlExecutionPlan plan = {0};
    MilenaError error;
    assert(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_OK);
    assert(plan.operation_count == 4 &&
           plan.operations[2].kind == MILENA_SQL_PLAN_TYPED_INSERT &&
           plan.operations[2].insert_column_count == 2 &&
           plan.operations[2].insert_value_count == 2 &&
           plan.operations[2].parameter_count == 2 &&
           strcmp(plan.operations[2].statement,
                  "INSERT INTO \"records\" (\"id\", \"note\") VALUES (?, ?)") == 0 &&
           plan.operations[2].parameters[1].kind == MILENA_SQL_PLAN_TEXT);
    FILE *output = tmpfile();
    assert(output);
    assert(milena_sql_run_plan(&plan, output, &error) == MILENA_OK);
    fclose(output);
    assert(scalar_count(db, "SELECT count(*) FROM records") == 2);
    MilenaTable readback;
    milena_table_init(&readback);
    MilenaSqlValue id = {.type = MILENA_SQL_INT64, .as.i64 = 123};
    run(db, "SELECT note FROM records WHERE id=?", &id, 1,
        &readback, MILENA_OK);
    assert(readback.row_count == 1 && readback.column_count == 1);
    const char *stored = NULL;
    assert(milena_table_get_string(&readback, 0, 0, &stored, &error) == MILENA_OK);
    assert(strcmp(stored, payload) == 0);
    milena_table_destroy(&readback);
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(program);
}

static size_t run_typed_text_equality(const char *literal) {
    ASTNode *program = ast_create_leaf(AST_SQL_PROGRAM, DB_PATH);
    ASTNode *schema = ast_create_leaf(AST_SQL_TABLE_SCHEMA, "casefold");
    ASTNode *schema_column = ast_create_leaf(AST_SQL_SCHEMA_COLUMN, "note");
    ASTNode *select = ast_create(AST_SQL_TYPED_SELECT);
    ASTNode *table = ast_create_leaf(AST_SQL_TABLE_REFERENCE, "casefold");
    ASTNode *projection = ast_create(AST_SQL_PROJECTION_LIST);
    ASTNode *projected_column = ast_create_leaf(AST_SQL_PROJECTED_COLUMN, "note");
    ASTNode *filter = ast_create(AST_SQL_FILTER);
    ASTNode *filter_column = ast_create_leaf(AST_SQL_FILTER_COLUMN, "note");
    ASTNode *filter_operator = ast_create(AST_SQL_FILTER_OPERATOR);
    ASTNode *parameter = test_sql_parameter(literal, "texto", AST_SQL_TYPE_TEXT);
    assert(program && schema && schema_column && select && table && projection &&
           projected_column && filter && filter_column && filter_operator && parameter);
    schema_column->type_name = milena_strdup("texto");
    schema_column->sql_type = AST_SQL_TYPE_TEXT;
    filter_operator->sql_operator = AST_SQL_OPERATOR_EQUAL;
    assert(schema_column->type_name &&
           ast_add_child(schema, schema_column) && ast_add_child(program, schema) &&
           ast_add_child(projection, projected_column) &&
           ast_add_child(filter, filter_column) &&
           ast_add_child(filter, filter_operator) && ast_add_child(filter, parameter) &&
           ast_add_child(select, table) && ast_add_child(select, projection) &&
           ast_add_child(select, filter) && ast_add_child(program, select));

    MilenaSqlExecutionPlan plan = {0};
    MilenaError error;
    assert(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_OK);
    assert(plan.operation_count == 2 &&
           strcmp(plan.operations[1].statement,
                  "SELECT \"note\" FROM \"casefold\" WHERE \"note\" COLLATE BINARY = ?") == 0);
    FILE *output = tmpfile();
    assert(output && milena_sql_run_plan(&plan, output, &error) == MILENA_OK);
    rewind(output);
    char header[128];
    size_t rows = SIZE_MAX;
    assert(fgets(header, sizeof(header), output) &&
           sscanf(header, "Tabla Milena: %zu filas", &rows) == 1);
    fclose(output);
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(program);
    return rows;
}

static void check_typed_text_equality_ignores_physical_nocase(MilenaSqlConnection *db) {
    MilenaTable result;
    milena_table_init(&result);
    run(db, "CREATE TABLE casefold(note TEXT COLLATE NOCASE)", NULL, 0,
        &result, MILENA_OK);
    run(db, "INSERT INTO casefold(note) VALUES('Alice')", NULL, 0,
        &result, MILENA_OK);
    assert(run_typed_text_equality("alice") == 0);
    assert(run_typed_text_equality("Alice") == 1);
    milena_table_destroy(&result);
}

static void check_malformed_sql_plan_parameter(void) {
    ASTNode *program = ast_create_leaf(AST_SQL_PROGRAM, DB_PATH);
    ASTNode *query = ast_create_leaf(AST_SQL_QUERY, "SELECT ?");
    ASTNode *parameter = ast_create_leaf(AST_SQL_PARAMETER, "");
    assert(program && query && parameter);
    parameter->type_name = milena_strdup("entero");
    assert(parameter->type_name && ast_add_child(query, parameter) &&
           ast_add_child(program, query));
    MilenaSqlExecutionPlan plan = {0};
    MilenaError error;
    assert(milena_sql_execution_plan_build(program, &plan, &error) == MILENA_ERR_TYPE);
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(program);
}

static void check_plan_transaction_preflight(void) {
    const ASTNodeType invalid_sequences[][3] = {
        {AST_SQL_BEGIN, AST_SQL_BEGIN, AST_SQL_QUERY},
        {AST_SQL_COMMIT, AST_SQL_QUERY, AST_SQL_QUERY},
        {AST_SQL_BEGIN, AST_SQL_QUERY, AST_SQL_QUERY}
    };
    const size_t sequence_lengths[] = {3, 2, 2};
    for (size_t sequence = 0; sequence < sizeof(sequence_lengths) / sizeof(sequence_lengths[0]); ++sequence) {
        ASTNode *program = ast_create_leaf(AST_SQL_PROGRAM,
            "tests/nonexistent-sql-plan-directory/never-open.sqlite");
        assert(program);
        MilenaSqlPlanOperation operations[3] = {{0}};
        for (size_t i = 0; i < sequence_lengths[sequence]; ++i) {
            ASTNode *operation = invalid_sequences[sequence][i] == AST_SQL_QUERY ?
                ast_create_leaf(AST_SQL_QUERY, "SELECT 1") :
                ast_create(invalid_sequences[sequence][i]);
            assert(operation && ast_add_child(program, operation));
            operations[i].source = operation;
            operations[i].kind = operation->type == AST_SQL_BEGIN ? MILENA_SQL_PLAN_BEGIN :
                operation->type == AST_SQL_COMMIT ? MILENA_SQL_PLAN_COMMIT :
                operation->type == AST_SQL_ROLLBACK ? MILENA_SQL_PLAN_ROLLBACK :
                operation->type == AST_SQL_QUERY ? MILENA_SQL_PLAN_QUERY : MILENA_SQL_PLAN_EXECUTE;
            operations[i].statement = operation->value;
        }
        MilenaSqlLimits limits = milena_sql_default_limits();
        MilenaSqlExecutionPlan plan = {
            .source = program,
            .connection_path = program->value,
            .operations = operations,
            .operation_count = sequence_lengths[sequence],
            .max_rows = limits.max_rows,
            .max_bytes = limits.max_bytes,
            .timeout_ms = limits.timeout_ms
        };
        MilenaError error;
        assert(milena_sql_execution_plan_validate(&plan, &error) == MILENA_ERR_DATA);
        /* If preflight tried to open the deliberately missing path, this would
         * return an I/O error instead of the plan's transaction error. */
        assert(milena_sql_run_plan(&plan, NULL, &error) == MILENA_ERR_DATA);
        ast_destroy(program);
    }
}

#define SQL_CANCEL_TEST_PATH_A "tests/sqlite-cancel-test-a.db"
#define SQL_CANCEL_TEST_PATH_B "tests/sqlite-cancel-test-b.db"
#define SQL_CANCEL_TEST_QUERY \
    "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<5000000) " \
    "SELECT sum(x) AS total FROM n"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t ready;
    bool go;
} SqlStartGate;

typedef struct {
    SqlStartGate *gate;
    MilenaSqlConnection *connection;
    MilenaTable result;
    MilenaError error;
    atomic_bool started;
    MilenaStatus status;
} SqlConcurrentTask;

static void *run_concurrent_sql(void *context) {
    SqlConcurrentTask *task = context;
    (void)pthread_mutex_lock(&task->gate->mutex);
    ++task->gate->ready;
    (void)pthread_cond_broadcast(&task->gate->condition);
    while (!task->gate->go)
        (void)pthread_cond_wait(&task->gate->condition, &task->gate->mutex);
    (void)pthread_mutex_unlock(&task->gate->mutex);

    /* Publish immediately before entering the backend. The recursive query is
     * deliberately much longer than the cancellation delay below. */
    atomic_store_explicit(&task->started, true, memory_order_release);
    MilenaSqlLimits limits = {1, 4096, 10000};
    task->status = milena_sql_execute(task->connection, SQL_CANCEL_TEST_QUERY,
        NULL, 0, &limits, &task->result, &task->error);
    return NULL;
}

static void remove_sqlite_test_files(const char *path) {
    char sidecar[128];
    (void)remove(path);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-journal", path);
    (void)remove(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)remove(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)remove(sidecar);
}

static void short_pause_ms(long milliseconds) {
    struct timespec remaining = {milliseconds / 1000,
        (milliseconds % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
}

static bool table_integer(const MilenaTable *table, int64_t *value) {
    const void *cell = NULL;
    MilenaError error;
    if (!table || table->row_count != 1 || table->column_count != 1 ||
        milena_table_is_null(table, 0, 0) ||
        milena_table_get_array_value(table, 0, 0, &cell, &error) != MILENA_OK ||
        !cell || table->columns[0].values.dtype != MILENA_DTYPE_INT64)
        return false;
    *value = *(const int64_t *)cell;
    return true;
}

static bool check_cross_thread_cancel_and_connection_ownership(void) {
    const char *paths[2] = {SQL_CANCEL_TEST_PATH_A, SQL_CANCEL_TEST_PATH_B};
    MilenaSqlConnection *connections[2] = {NULL, NULL};
    SqlConcurrentTask tasks[2] = {{0}};
    pthread_t threads[2];
    size_t created = 0;
    bool gate_mutex_ready = false, gate_condition_ready = false;
    bool setup_ok = true, cancelled = false;
    bool reuse_ok = false;
    SqlStartGate gate = {0};
    MilenaTable reuse_result;
    milena_table_init(&reuse_result);

    for (size_t i = 0; i < 2; ++i) {
        remove_sqlite_test_files(paths[i]);
        if (milena_sql_open(&connections[i], paths[i], NULL) != MILENA_OK)
            setup_ok = false;
    }
    if (setup_ok) {
        gate_mutex_ready = pthread_mutex_init(&gate.mutex, NULL) == 0;
        gate_condition_ready = gate_mutex_ready &&
            pthread_cond_init(&gate.condition, NULL) == 0;
        if (!gate_condition_ready) setup_ok = false;
    }
    if (setup_ok) {
        for (size_t i = 0; i < 2; ++i) {
            tasks[i].gate = &gate;
            tasks[i].connection = connections[i];
            tasks[i].status = MILENA_ERR_INTERNAL;
            atomic_init(&tasks[i].started, false);
            milena_table_init(&tasks[i].result);
            milena_error_init(&tasks[i].error);
        }
        for (; created < 2; ++created) {
            if (pthread_create(&threads[created], NULL, run_concurrent_sql,
                               &tasks[created]) != 0) {
                setup_ok = false;
                break;
            }
        }
        (void)pthread_mutex_lock(&gate.mutex);
        if (created == 2) {
            while (gate.ready < 2)
                (void)pthread_cond_wait(&gate.condition, &gate.mutex);
        }
        /* Release any successfully created workers even on partial setup. */
        gate.go = true;
        (void)pthread_cond_broadcast(&gate.condition);
        (void)pthread_mutex_unlock(&gate.mutex);

        for (size_t attempt = 0; attempt < 5000; ++attempt) {
            bool all_started = true;
            for (size_t i = 0; i < created; ++i)
                if (!atomic_load_explicit(&tasks[i].started, memory_order_acquire))
                    all_started = false;
            if (all_started) break;
            short_pause_ms(1);
        }
        for (size_t i = 0; i < created; ++i)
            if (!atomic_load_explicit(&tasks[i].started, memory_order_acquire))
                setup_ok = false;

        if (setup_ok && created == 2) {
            /* Both independent recursive SELECTs have been released together;
             * this delay lets each enter SQLite before interrupting only A. */
            short_pause_ms(50);
            milena_sql_cancel(connections[0]);
            cancelled = true;
        } else {
            /* Bound cleanup after partial setup, including a worker that has
             * been released but has not yet reached its execute call. */
            for (size_t i = 0; i < created; ++i)
                milena_sql_cancel(connections[i]);
        }
        for (size_t i = 0; i < created; ++i)
            (void)pthread_join(threads[i], NULL);

        if (created != 2) setup_ok = false;
        if (created == 2) {
            int64_t sum = 0;
            if (tasks[0].status != MILENA_ERR_UNSUPPORTED ||
                tasks[0].error.code != MILENA_ERR_UNSUPPORTED ||
                strstr(tasks[0].error.message, "cancelada") == NULL)
                setup_ok = false;
            if (tasks[1].status != MILENA_OK ||
                !table_integer(&tasks[1].result, &sum) ||
                sum != INT64_C(12500002500000))
                setup_ok = false;
        }
    }

    /* No execute remains active after the joins. Reset only the cancelled
     * handle, then demonstrate that it accepts and completes new work. */
    if (connections[0]) {
        milena_sql_reset_cancel(connections[0]);
        MilenaSqlLimits limits = {1, 4096, 1000};
        MilenaError error;
        MilenaStatus status = milena_sql_execute(connections[0], "SELECT 6*7",
            NULL, 0, &limits, &reuse_result, &error);
        int64_t value = 0;
        reuse_ok = status == MILENA_OK && error.code == MILENA_OK &&
                   table_integer(&reuse_result, &value) && value == 42;
    }

    for (size_t i = 0; i < 2; ++i)
        milena_table_destroy(&tasks[i].result);
    milena_table_destroy(&reuse_result);
    if (gate_condition_ready) (void)pthread_cond_destroy(&gate.condition);
    if (gate_mutex_ready) (void)pthread_mutex_destroy(&gate.mutex);
    for (size_t i = 0; i < 2; ++i) {
        /* Closing is deliberately after every worker has joined. */
        milena_sql_close(connections[i]);
        remove_sqlite_test_files(paths[i]);
    }
    if (!(setup_ok && cancelled && reuse_ok)) {
        fprintf(stderr, "cross-thread SQL diagnostic: setup=%d cancelled=%d reuse=%d created=%zu A=%s (%s) B=%s (%s)\n",
            setup_ok, cancelled, reuse_ok, created,
            milena_status_name(tasks[0].status), tasks[0].error.message,
            milena_status_name(tasks[1].status), tasks[1].error.message);
    }
    return setup_ok && cancelled && reuse_ok;
}

int main(void) {
    check_typed_sql_plan();
    check_malformed_sql_plan_parameter();
    check_plan_transaction_preflight();
    if (!check_cross_thread_cancel_and_connection_ownership()) {
        fputs("SQLite cancellation/concurrency/ownership checks failed\n", stderr);
        return 1;
    }
    puts("SQLite cross-thread cancellation and independent-connection checks passed");
    (void)remove(DB_PATH);
    MilenaError error;
    MilenaSqlConnection *db = NULL;
    assert(milena_sql_open(&db, DB_PATH, &error) == MILENA_OK);
    MilenaTable table;
    milena_table_init(&table);
    run(db, "CREATE TABLE records(id INTEGER PRIMARY KEY, note TEXT, maybe INTEGER)",
        NULL, 0, &table, MILENA_OK);
    check_typed_text_equality_ignores_physical_nocase(db);

    const char payload[] = "x'); DROP TABLE records; --";
    MilenaSqlValue insert_values[3] = {
        {.type = MILENA_SQL_INT64, .as.i64 = INT64_C(9007199254740997)},
        {.type = MILENA_SQL_TEXT, .as.text = {payload, sizeof(payload) - 1}},
        {.type = MILENA_SQL_NULL}
    };
    run(db, "INSERT INTO records(id,note,maybe) VALUES(?,?,?)", insert_values, 3,
        &table, MILENA_OK);
    assert(scalar_count(db, "SELECT count(*) FROM records") == 1);

    MilenaSqlValue key = {.type = MILENA_SQL_INT64, .as.i64 = INT64_C(9007199254740997)};
    run(db, "SELECT id,note,maybe FROM records WHERE id=?", &key, 1, &table, MILENA_OK);
    assert(table.row_count == 1 && table.column_count == 3);
    const void *integer = NULL;
    assert(milena_table_get_array_value(&table, 0, 0, &integer, &error) == MILENA_OK);
    assert(*(const int64_t *)integer == INT64_C(9007199254740997));
    const char *text = NULL;
    assert(milena_table_get_string(&table, 1, 0, &text, &error) == MILENA_OK);
    assert(strcmp(text, payload) == 0);
    assert(milena_table_is_null(&table, 2, 0));

    const char utf8[] = "Milena — análisis";
    MilenaSqlValue utf8_value = {.type = MILENA_SQL_TEXT,
        .as.text = {utf8, sizeof(utf8) - 1}};
    run(db, "SELECT ? AS saludo", &utf8_value, 1, &table, MILENA_OK);
    assert(table.row_count == 1);
    assert(milena_table_get_string(&table, 0, 0, &text, &error) == MILENA_OK);
    assert(strcmp(text, utf8) == 0);

    run(db, "SELECT missing_column FROM records", NULL, 0, &table, MILENA_ERR_DATA);
    run(db, "SELECT FROM records", NULL, 0, &table, MILENA_ERR_DATA);
    run(db, "SELECT 1; SELECT 2", NULL, 0, &table, MILENA_ERR_ARGUMENT);
    run(db, "SELECT ?", NULL, 0, &table, MILENA_ERR_ARGUMENT);
    run(db, "BEGIN", NULL, 0, &table, MILENA_ERR_DATA);
    run(db, "SAVEPOINT hidden", NULL, 0, &table, MILENA_ERR_DATA);
    run(db, "SELECT 1; -- a trailing comment is not another statement", NULL, 0,
        &table, MILENA_OK);
    run(db, "PRAGMA table_info(records)", NULL, 0, &table, MILENA_ERR_DATA);

    assert(milena_sql_query(db, "DELETE FROM records", NULL, 0, NULL,
                            &table, &error) == MILENA_ERR_ARGUMENT);
    assert(scalar_count(db, "SELECT count(*) FROM records") == 1);

    MilenaSqlValue nonfinite = {.type = MILENA_SQL_FLOAT64, .as.f64 = INFINITY};
    run(db, "SELECT ?", &nonfinite, 1, &table, MILENA_ERR_ARGUMENT);
    const char malformed_utf8[] = {(char)0xc0, (char)0xaf};
    MilenaSqlValue invalid_text = {.type = MILENA_SQL_TEXT,
        .as.text = {malformed_utf8, sizeof(malformed_utf8)}};
    run(db, "SELECT ?", &invalid_text, 1, &table, MILENA_ERR_ARGUMENT);

    assert(milena_sql_begin(db, &error) == MILENA_OK);
    MilenaSqlValue transient = {.type = MILENA_SQL_INT64, .as.i64 = 10};
    run(db, "INSERT INTO records(id,note) VALUES(?, 'rollback')", &transient, 1,
        &table, MILENA_OK);
    run(db, "INSERT INTO no_such_table VALUES(1)", NULL, 0, &table, MILENA_ERR_DATA);
    /* A failed statement aborts and rolls back the complete active transaction. */
    assert(scalar_count(db, "SELECT count(*) FROM records") == 1);

    check_typed_insert_execution(db);

    MilenaSqlLimits row_limited = {1, 1024, 1000};
    assert(milena_sql_execute(db, "SELECT 1 UNION ALL SELECT 2", NULL, 0,
        &row_limited, &table, &error) == MILENA_ERR_DATA);
    MilenaSqlLimits byte_limited = {100, 4, 1000};
    assert(milena_sql_execute(db, "SELECT 'this is larger than four bytes'", NULL, 0,
        &byte_limited, &table, &error) == MILENA_ERR_DATA);
    MilenaSqlLimits time_limited = {100, 4096, 1};
    MilenaStatus timed = milena_sql_execute(db,
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000000) SELECT sum(x) FROM n",
        NULL, 0, &time_limited, &table, &error);
    assert(timed == MILENA_ERR_UNSUPPORTED);

    /* Closing an open transaction is a rollback, and the connection is reusable. */
    assert(milena_sql_begin(db, &error) == MILENA_OK);
    MilenaSqlValue last = {.type = MILENA_SQL_INT64, .as.i64 = 11};
    run(db, "INSERT INTO records(id,note) VALUES(?, 'close rollback')", &last, 1,
        &table, MILENA_OK);
    milena_table_destroy(&table);
    milena_sql_close(db);
    assert(remove(DB_PATH) == 0);
    puts("SQLite native backend checks passed");
    return 0;
}
