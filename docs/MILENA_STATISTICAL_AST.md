# AST y parser de operaciones estadísticas

Este cambio introduce **representación y análisis sintáctico**, no una nueva ruta de ejecución. El AST que produce `src/parser.c` todavía no está conectado al ejecutor de scripts de `src/script.c`. Las pruebas de este componente verifican tokens, estructura, validación y ownership del AST; no afirman que el AST sea evaluado.

## Alcance sintáctico

Las operaciones se escriben dentro de un bloque de análisis y solo pueden referirse a un arreglo declarado antes en el mismo bloque:

```milena
.analisis ejemplo {
    arreglo valores = [1, 2, 3, 4];

    suma(valores);
    media(valores, eje 0);
    minimo(valores);
    maximo(valores);
    varianza(valores);
    desviacion_estandar(valores);
    mediana(valores, eje 0, sin conservar dimensiones);
    percentil(valores, 90, eje 0, conservar dimensiones);
}
```

Operaciones representadas: `suma`, `media`, `minimo`, `maximo`, `varianza`, `desviacion_estandar`, `mediana` y `percentil`.

Reglas validadas por el parser:

- el primer argumento debe ser el nombre de un arreglo declarado previamente;
- `percentil` requiere un número entre 0 y 100, inclusive;
- `eje` debe ser un entero no negativo y puede aparecer una sola vez;
- `conservar dimensiones` y `sin conservar dimensiones` pueden aparecer una sola vez y requieren `eje`;
- una llamada termina en `)` y cada operación termina en `;`;
- argumentos, opciones o duplicados no reconocidos producen un error y el parser termina sin devolver un AST parcial.

La validez de un eje para el rango real del arreglo y la ejecución de la reducción pertenecen a una futura fase semántica/runtime. El AST conserva `axis`, `keepdims` y el valor del percentil para esa integración posterior.
