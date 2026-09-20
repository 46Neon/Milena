#include "parser.h"
#include "interpreter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *src =
        "funcion factorial(n) { "
        "si (n == 0) { retornar 1; } "
        "sino { retornar n * factorial(n - 1); } } "
        "funcion signo(n) { si (n > 0) { retornar 1; } sino { retornar 0; } } "
        "variable resultado = factorial(5); variable positivo = signo(3);";
    Lexer l; Parser p; lexer_init(&l, src); parser_init(&p, &l);
    ASTNode *tree = parser_parse(&p);
    assert(tree && !p.has_error);
    Interpreter vm; assert(interpreter_init(&vm, tree));
    assert(interpreter_run(&vm));
    double result = 0.0, positive = 0.0;
    assert(interpreter_get_number(&vm, "resultado", &result) && result == 120.0);
    assert(interpreter_get_number(&vm, "positivo", &positive) && positive == 1.0);
    interpreter_destroy(&vm); ast_destroy(tree); parser_release(&p);
    puts("functions: factorial(5)=120, si/sino/comparisons OK");
    return 0;
}
