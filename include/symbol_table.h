#ifndef MILENA_SYMBOL_TABLE_H
#define MILENA_SYMBOL_TABLE_H
#include "common.h"
typedef struct { char *name; int scope; } MilenaSymbol;
typedef struct { MilenaSymbol *items; size_t count, capacity; int scope; } MilenaSymbolTable;
void milena_symbols_init(MilenaSymbolTable *table);
void milena_symbols_release(MilenaSymbolTable *table);
void milena_symbols_enter_scope(MilenaSymbolTable *table);
void milena_symbols_leave_scope(MilenaSymbolTable *table);
MilenaStatus milena_symbols_declare(MilenaSymbolTable *table,const char *name,MilenaError *error);
bool milena_symbols_exists(const MilenaSymbolTable *table,const char *name);
#endif
