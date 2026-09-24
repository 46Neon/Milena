#ifndef MILENA_CANONICAL_COMPILER_H
#define MILENA_CANONICAL_COMPILER_H

#include "ast.h"
#include "table.h"

/* Provenance key stamped by the canonical loader before a data-HIR table is bound. */
#define MILENA_HIR_DATASET_PATH_METADATA "milena.hir.dataset.path"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable input boundary for a future compiler backend.  In the language
 * contract, MilenaTable is the canonical dataframe representation.
 *
 * This header intentionally does not include compiler.h, ir.h or vm.h.  The
 * official executable can therefore validate the canonical AST/table contract
 * without linking the unfinished compiler stack.  `table` is borrowed and
 * remains owned by the caller.
 */
/*
 * Owned typed HIRs for the validated scalar-function subset and the closed
 * table subset documented below. HIR storage owns names, children and operation
 * vectors; input tables remain borrowed. Data column references retain resolved
 * schema identity/type/nullability/shape and source spans when available.
 */
typedef enum {
    MILENA_HIR_NUMBER,
    MILENA_HIR_BOOLEAN
} MilenaHIRValueType;

typedef enum {
    MILENA_HIR_EXPR_LITERAL,
    MILENA_HIR_EXPR_VARIABLE,
    MILENA_HIR_EXPR_BINARY,
    MILENA_HIR_EXPR_CALL
} MilenaHIRExpressionKind;

typedef struct MilenaHIRExpression MilenaHIRExpression;
typedef struct MilenaHIRStatement MilenaHIRStatement;

typedef struct {
    size_t line, column, end_line, end_column;
    size_t start_offset, end_offset;
    bool has_source_span;
} MilenaHIRSourceSpan;

struct MilenaHIRExpression {
    MilenaHIRExpressionKind kind;
    MilenaHIRValueType value_type;
    size_t resolved_symbol_id;
    MilenaHIRSourceSpan span;
    union {
        double number;
        bool boolean;
        struct {
            ASTOperatorKind operation;
            MilenaHIRExpression *left;
            MilenaHIRExpression *right;
        } binary;
        struct {
            MilenaHIRExpression **arguments;
            size_t argument_count;
        } call;
    } as;
};

typedef enum {
    MILENA_HIR_STMT_DECLARE,
    MILENA_HIR_STMT_ASSIGN,
    MILENA_HIR_STMT_RETURN,
    MILENA_HIR_STMT_IF
} MilenaHIRStatementKind;

struct MilenaHIRStatement {
    MilenaHIRStatementKind kind;
    MilenaHIRValueType value_type;
    size_t resolved_symbol_id;
    char *name;
    MilenaHIRSourceSpan span;
    union {
        MilenaHIRExpression *expression;
        struct {
            MilenaHIRExpression *condition;
            MilenaHIRStatement **then_body;
            size_t then_count;
            MilenaHIRStatement **else_body;
            size_t else_count;
        } conditional;
    } as;
};

typedef struct {
    char *name;
    size_t resolved_symbol_id;
    MilenaHIRSourceSpan span;
    struct {
        char *name;
        size_t resolved_symbol_id;
    } *parameters;
    size_t parameter_count;
    MilenaHIRStatement **body;
    size_t body_count;
} MilenaHIRFunction;

typedef struct {
    MilenaHIRFunction *functions;
    size_t function_count;
    MilenaHIRStatement **statements;
    size_t statement_count;
} MilenaScalarHIR;

/* Typed table/data HIR. This deliberately closed subset has one program-local
 * dataset binding with loader-stamped path provenance; numeric product/filter,
 * column projection, grouping, summary, and a borrowed-output export boundary. */
typedef enum {
    MILENA_HIR_COLUMN_UNKNOWN,
    MILENA_HIR_COLUMN_NUMERIC,
    MILENA_HIR_COLUMN_BOOLEAN,
    MILENA_HIR_COLUMN_TEXT,
    MILENA_HIR_COLUMN_CATEGORICAL
} MilenaHIRColumnType;

typedef struct {
    char *name;
    size_t resolved_column_index;
    MilenaHIRColumnType type;
    MilenaHIRColumnType declared_type;
    MilenaDType dtype;
    bool nullable;
    size_t rank;
    size_t shape[1];
    MilenaHIRSourceSpan span;
} MilenaHIRColumnRef;

typedef struct {
    size_t max_input_rows;
    size_t max_output_rows;
    size_t max_columns;
} MilenaHIRResourcePolicy;

typedef struct {
    char *path;
    size_t resolved_dataset_id;
    bool streaming;
    size_t chunk_rows;
    size_t max_rows;
    size_t max_columns;
    size_t max_record_bytes;
    double max_elapsed_milliseconds;
    MilenaHIRSourceSpan span;
} MilenaHIRDatasetSource;

typedef struct {
    MilenaHIRColumnRef input;
    MilenaAggregateOp operation;
    char *output_name;
    double percentile;
    MilenaHIRSourceSpan span;
} MilenaHIRAggregate;

typedef enum {
    MILENA_HIR_DATA_PRODUCT,
    MILENA_HIR_DATA_FILTER_NUMERIC,
    MILENA_HIR_DATA_SELECT_COLUMNS,
    MILENA_HIR_DATA_GROUP,
    MILENA_HIR_DATA_SUMMARIZE,
    MILENA_HIR_DATA_JOIN,
    MILENA_HIR_DATA_SST,
    MILENA_HIR_DATA_EXPORT
} MilenaHIRDataOperationKind;

typedef struct {
    MilenaHIRDataOperationKind kind;
    size_t resolved_dataset_id;
    MilenaHIRSourceSpan span;
    union {
        struct { MilenaHIRColumnRef left, right; char *output_name; } product;
        struct { MilenaHIRColumnRef column; ASTOperatorKind operation; double threshold; } filter;
        struct { MilenaHIRColumnRef *columns; size_t count; } select;
        struct { MilenaHIRColumnRef key; MilenaHIRAggregate *aggregates; size_t aggregate_count; MilenaHIRResourcePolicy policy; } group;
        struct { MilenaHIRAggregate *aggregates; size_t aggregate_count; } summarize;
        struct { char *right_source; size_t right_dataset_id; MilenaHIRColumnRef left_key, right_key; MilenaJoinType join_type; MilenaHIRResourcePolicy policy; size_t memory_budget_bytes; } join;
        struct { char *name; MilenaHIRColumnRef *columns; size_t column_count; } sst;
        struct { char *path; } export_result;
    } as;
} MilenaHIRDataOperation;

typedef struct {
    MilenaHIRDatasetSource source;
    MilenaHIRColumnRef *declared_schema;
    size_t declared_column_count;
    MilenaHIRDataOperation *operations;
    size_t operation_count;
    char *export_path;
    MilenaHIRResourcePolicy resource_policy;
    bool schema_bound;
} MilenaDataHIR;

typedef struct {
    const ASTNode *ast;
    const MilenaTable *table;
    const MilenaTable *right_table; /* Borrowed second dataset for a typed join. */
    const MilenaScalarHIR *hir;
    const MilenaDataHIR *data_hir; /* NULL outside the typed data subset. */
} MilenaCanonicalCompilerInput;

typedef struct {
    ASTNode *ast;
    const MilenaTable *table;
    const MilenaTable *right_table; /* Borrowed second dataset when HIR has a join. */
    MilenaScalarHIR *hir; /* Owned scalar HIR, when the scalar subset applies. */
    MilenaDataHIR *data_hir; /* Owned data HIR, when the table subset applies. */
} MilenaCanonicalProgram;

void milena_canonical_program_init(MilenaCanonicalProgram *program);
void milena_canonical_program_release(MilenaCanonicalProgram *program);

/* Parse and validate through the official lexer -> parser -> semantic path. */
MilenaStatus milena_canonical_program_parse(MilenaCanonicalProgram *program,
                                             const char *source,
                                             MilenaError *error);

/* Bind a borrowed canonical dataframe and validate SST column contracts.
 * Data-HIR programs require MILENA_HIR_DATASET_PATH_METADATA to equal the
 * source path recorded by the HIR; loaders must stamp it only after resolving
 * and loading that same source. */
MilenaStatus milena_canonical_program_bind_table(MilenaCanonicalProgram *program,
                                                 const MilenaTable *table,
                                                 MilenaError *error);
/* Bind the primary source and, for HIR with one join, its explicitly resolved
 * right-side source. Both borrowed tables must carry matching path metadata. */
MilenaStatus milena_canonical_program_bind_tables(
    MilenaCanonicalProgram *program, const MilenaTable *table,
    const MilenaTable *right_table, MilenaError *error);

/* Execute the supported typed data-HIR subset against the borrowed bound table(s).
 * `output` is replaced transactionally on success and left unchanged on error.
 * A NULL policy uses safe bounds derived from the input table dimensions. */
MilenaStatus milena_canonical_program_execute_data(
    const MilenaCanonicalProgram *program,
    const MilenaHIRResourcePolicy *policy,
    MilenaTable *output,
    MilenaError *error);

/* Default compiler boundary. This is fail-closed: a backend cannot receive an
 * AST-only program as if it were compilable. */
MilenaStatus milena_canonical_compiler_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error);

/* Explicit compatibility-only AST/runtime view. Consumers of this function
 * must not treat a NULL HIR as compiler input. */
MilenaStatus milena_canonical_compatibility_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error);

/* Named strict alias retained for callers that want to make the HIR requirement
 * explicit at the call site. Rejects incomplete HIR with source diagnostics. */
MilenaStatus milena_canonical_hir_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
