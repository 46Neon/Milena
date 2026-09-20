# Funciones definidas por la persona

La superficie ejecutable admite funciones en español:

```milena
función factorial(n) {
  si (n == 0) { retornar 1; }
  retornar n * factorial(n - 1);
}
variable resultado = factorial(5);
```

La forma actualmente implementada es `función` (también `funcion`), un nombre,
una lista de parámetros separados por comas y un bloque `{ ... }`. `retornar`
acepta una expresión numérica; una función sin `retornar` devuelve `0`. Las
expresiones admiten literales numéricos, `verdadero`/`falso`, variables, llamadas,
comparaciones (`==`, `!=`, `<`, `<=`, `>`, `>=`) y `+`, `-`, `*`, `/`. `si (...) { ... }`
puede llevar un bloque opcional `sino { ... }`.
Los parámetros y variables locales viven en un ámbito nuevo por invocación, y
las llamadas pueden ser recursivas. El parser conserva las definiciones en el
AST y el intérprete las resuelve en tiempo de ejecución, por lo que también se
permiten referencias hacia definiciones posteriores.

Limitaciones deliberadas de este bloque:
valores de texto/arreglos como parámetros, valores por defecto, funciones como
valores, ni comprobación estática completa de tipos. Las funciones estadísticas
existentes siguen siendo tokens y llamadas reconocibles por el parser, pero
sus kernels de arreglos permanecen fuera de este intérprete numérico mínimo.
