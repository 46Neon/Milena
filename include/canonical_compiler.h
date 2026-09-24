#ifndef MILENA_CANONICAL_COMPILER_H
#define MILENA_CANONICAL_COMPILER_H

#include "ast.h"
#include "table.h"

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
 * Owned typed HIR for the canonical scalar-function subset only.  The current
 * HIR is intentionally unavailable for analysis/data-operation programs; those
 * continue to use the validated AST until their typed data-operation HIR and
 * column bindings are implemented.  All text and child storage in this HIR is
 * independently owned, and every node carries the originating AST binding ID
 * and source span.
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

typedef struct {
    const ASTNode *ast;
    const MilenaTable *table;
    const MilenaScalarHIR *hir; /* NULL when this AST is outside scalar HIR. */
} MilenaCanonicalCompilerInput;

typedef struct {
    ASTNode *ast;
    const MilenaTable *table;
    MilenaScalarHIR *hir; /* Owned; present only for the scalar subset above. */
} MilenaCanonicalProgram;

void milena_canonical_program_init(MilenaCanonicalProgram *program);
void milena_canonical_program_release(MilenaCanonicalProgram *program);

/* Parse and validate through the official lexer -> parser -> semantic path. */
MilenaStatus milena_canonical_program_parse(MilenaCanonicalProgram *program,
                                             const char *source,
                                             MilenaError *error);

/* Bind a borrowed canonical dataframe and validate SST column contracts. */
MilenaStatus milena_canonical_program_bind_table(MilenaCanonicalProgram *program,
                                                 const MilenaTable *table,
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
