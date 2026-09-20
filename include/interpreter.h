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
} Interpreter;

bool interpreter_init(Interpreter *interpreter, ASTNode *ast);
bool interpreter_run(Interpreter *interpreter);
void interpreter_destroy(Interpreter *interpreter);

#endif
