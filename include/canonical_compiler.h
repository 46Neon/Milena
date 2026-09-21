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
typedef struct {
    const ASTNode *ast;
    const MilenaTable *table;
} MilenaCanonicalCompilerInput;

typedef struct {
    ASTNode *ast;
    const MilenaTable *table;
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

/* Export the only input shape a future compiler backend may consume. */
MilenaStatus milena_canonical_compiler_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
