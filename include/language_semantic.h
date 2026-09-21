#ifndef MILENA_LANGUAGE_SEMANTIC_H
#define MILENA_LANGUAGE_SEMANTIC_H

#include "ast.h"
#include "table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validación semántica mínima y común para todo AST oficial. */
MilenaStatus milena_validate_ast(const ASTNode *program, MilenaError *error);
MilenaStatus milena_validate_sst_table(const ASTNode *analysis,
                                       const MilenaTable *table,
                                       MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
