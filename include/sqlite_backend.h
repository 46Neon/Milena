#ifndef MILENA_SQLITE_BACKEND_H
#define MILENA_SQLITE_BACKEND_H

#include "table.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MILENA_SQL_DEFAULT_MAX_ROWS 10000u
#define MILENA_SQL_HARD_MAX_ROWS 100000u
#define MILENA_SQL_DEFAULT_MAX_BYTES (8u * 1024u * 1024u)
#define MILENA_SQL_HARD_MAX_BYTES (64u * 1024u * 1024u)
#define MILENA_SQL_DEFAULT_TIMEOUT_MS 2000u
#define MILENA_SQL_HARD_TIMEOUT_MS 30000u
#define MILENA_SQL_MAX_PARAMETERS 999u

typedef struct MilenaSqlConnection MilenaSqlConnection;
struct ASTNode;
struct MilenaSqlExecutionPlan;

typedef struct {
    size_t max_rows;
    size_t max_bytes;
    unsigned timeout_ms;
} MilenaSqlLimits;

typedef enum {
    MILENA_SQL_NULL = 0,
    MILENA_SQL_INT64,
    MILENA_SQL_FLOAT64,
    MILENA_SQL_TEXT
} MilenaSqlValueType;

typedef struct {
    MilenaSqlValueType type;
    union {
        int64_t i64;
        double f64;
        struct { const char *data; size_t length; } text;
    } as;
} MilenaSqlValue;

MilenaSqlLimits milena_sql_default_limits(void);
MilenaStatus milena_sql_open(MilenaSqlConnection **out, const char *path,
                             MilenaError *error);
/* Normal connection operations require exclusive ownership: do not overlap
 * execute/query, transaction operations, or close on one handle. Close rolls
 * back an open transaction. */
void milena_sql_close(MilenaSqlConnection *connection);
/* The sole supported concurrent operation: safe from another thread while
 * execute/query is active. Do not close until that execution has stopped. */
void milena_sql_cancel(MilenaSqlConnection *connection);
/* Call only when no execute/query is active, before reusing the handle. */
void milena_sql_reset_cancel(MilenaSqlConnection *connection);
MilenaStatus milena_sql_begin(MilenaSqlConnection *connection,
                              MilenaError *error);
MilenaStatus milena_sql_commit(MilenaSqlConnection *connection,
                               MilenaError *error);
MilenaStatus milena_sql_rollback(MilenaSqlConnection *connection,
                                 MilenaError *error);
/* Exactly one prepared SQL statement is accepted. Values are always bound. */
MilenaStatus milena_sql_query(MilenaSqlConnection *connection,
                              const char *sql,
                              const MilenaSqlValue *parameters,
                              size_t parameter_count,
                              const MilenaSqlLimits *limits,
                              MilenaTable *result,
                              MilenaError *error);
MilenaStatus milena_sql_run_plan(const struct MilenaSqlExecutionPlan *plan,
                                 FILE *output, MilenaError *error);
/* Compatibility entry point; builds and validates the same canonical plan. */
MilenaStatus milena_sql_run_program(const struct ASTNode *program,
                                    FILE *output, MilenaError *error);
MilenaStatus milena_sql_execute(MilenaSqlConnection *connection,
                                const char *sql,
                                const MilenaSqlValue *parameters,
                                size_t parameter_count,
                                const MilenaSqlLimits *limits,
                                MilenaTable *result,
                                MilenaError *error);

#ifdef __cplusplus
}
#endif
#endif
