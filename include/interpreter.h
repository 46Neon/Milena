#ifndef MILENA_INTERPRETER_H
#define MILENA_INTERPRETER_H

#include "common.h"
#include "ast.h"
#include "symbol.h"
#include "dataset.h"

typedef struct Interpreter {
    ASTNode *ast;
    SymbolTable *symbols;
    Dataset *dataset;
    bool has_error;
    MilenaErrorInfo error;
    void *runtime;
} Interpreter;

bool interpreter_init(Interpreter *interpreter, ASTNode *ast);
bool interpreter_run(Interpreter *interpreter);
/* Read a numeric global after execution; useful for embedding and smoke tests. */
bool interpreter_get_number(const Interpreter *interpreter, const char *name, double *value);
void interpreter_destroy(Interpreter *interpreter);

#endif
