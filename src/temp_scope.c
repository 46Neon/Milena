#include "temp_scope.h"

bool milena_temp_scope_begin(MilenaTempScope *scope, size_t capacity) {
    if (!scope || !arena_init(&scope->arena, capacity)) return false;
    scope->active = true;
    return true;
}
void *milena_temp_scope_alloc(MilenaTempScope *scope, size_t size) {
    if (!scope || !scope->active) return NULL;
    return arena_alloc(&scope->arena, size);
}
void milena_temp_scope_end(MilenaTempScope *scope) {
    if (!scope || !scope->active) return;
    arena_destroy(&scope->arena);
    scope->active = false;
}
