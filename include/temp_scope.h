#ifndef MILENA_TEMP_SCOPE_H
#define MILENA_TEMP_SCOPE_H
#include "arena.h"

typedef struct { Arena arena; bool active; } MilenaTempScope;
bool milena_temp_scope_begin(MilenaTempScope *scope, size_t capacity);
void *milena_temp_scope_alloc(MilenaTempScope *scope, size_t size);
void milena_temp_scope_end(MilenaTempScope *scope);
#endif
