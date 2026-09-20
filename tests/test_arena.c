#include "arena.h"
#include "temp_scope.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    Arena arena; assert(arena_init(&arena,64)); int *x=arena_alloc(&arena,sizeof(*x)); assert(x); *x=7; assert(*x==7); arena_reset(&arena); assert(arena_used(&arena)==0); arena_destroy(&arena);
    MilenaTempScope scope; assert(milena_temp_scope_begin(&scope,64)); int *temporary=milena_temp_scope_alloc(&scope,sizeof(*temporary)); assert(temporary); *temporary=11; assert(*temporary==11); milena_temp_scope_end(&scope); assert(!scope.active); milena_temp_scope_end(&scope);
    puts("OK: Milena temporary arena and scope"); return 0;
}
