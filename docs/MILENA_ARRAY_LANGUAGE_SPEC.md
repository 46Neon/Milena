# Especificación inicial de arrays en `.milena`

Esta es la primera superficie del lenguaje para exponer `MilenaArray`. La implementación debe construir arrays mediante el runtime C17 existente y no mediante estructuras paralelas del parser.

## Sintaxis canónica

```milena
.analisis ejemplo {
    arreglo valores = [1, 2, 3, 4];
    suma(valores);
    media(valores, eje 0);
    mediana(valores);
    percentil(valores, 90, eje 0, conservar dimensiones);
}
```

La palabra `arreglo` y las operaciones estadísticas en español son la
superficie canónica. `array`, `sum`, `mean` y los demás nombres históricos
pueden mantenerse temporalmente como compatibilidad, pero las funcionalidades
nuevas deben incorporarse al vocabulario español.

## Literales 1-D

Un literal 1-D contiene solamente números:

```milena
array enteros = [1, 2, 3];
array reales = [1.5, 2.0, 3.25];
```

Reglas:

- una lista vacía es inválida en esta primera versión;
- todos los elementos deben ser numéricos;
- si todos son enteros, el dtype inicial es `int64`;
- si algún elemento tiene parte decimal, el dtype inicial es `float64`;
- la longitud de la lista es su primera dimensión;
- no se acepta una segunda dimensión hasta implementar literales multidimensionales.

## `ceros`

El constructor español acepta una o varias dimensiones positivas:

```milena
.analisis dimensiones {
    arreglo vector = ceros(5);
    arreglo matriz = ceros(2, 3);
    media(matriz, eje 0);
}
```

Devuelve un arreglo `float64` inicializado en cero. Las dimensiones deben ser
enteros positivos y no se admite una coma final.

## Operaciones estadísticas disponibles

La superficie formal actual expone:

```milena
suma(arreglo)
media(arreglo)
minimo(arreglo)
maximo(arreglo)
varianza(arreglo)
desviacion_estandar(arreglo)
mediana(arreglo)
percentil(arreglo, 90)

media(arreglo, eje 0)
media(arreglo, eje 0, conservar dimensiones)
media(arreglo, eje 0, sin conservar dimensiones)
```

Las reducciones usan el motor `MilenaArray`, conservan el dtype cuando es
seguro y devuelven un arreglo con la forma correspondiente al eje. La
aritmética y el broadcasting siguen siendo capacidades del motor numérico y
se irán exponiendo mediante AST/runtime, no mediante un segundo parser.

El resultado de una operación aritmética es un nuevo `MilenaArray`, salvo que una operación futura declare explícitamente una salida reutilizable.

## Errores obligatorios

El runtime debe producir errores para:

- lista vacía;
- tokens no numéricos dentro de un literal;
- paréntesis o corchetes sin cerrar;
- coma final no soportada en la primera versión;
- shape incompatible;
- overflow durante la conversión o la operación;
- uso de un identificador no definido;
- llamada a función con cantidad incorrecta de argumentos;
- intento de modificar una vista de solo lectura.

## Ownership

Los arrays creados por una declaración pertenecen al entorno de ejecución. Una expresión que crea un resultado entrega ownership al valor resultante. Las vistas futuras conservarán una referencia al almacenamiento propietario.

No se debe guardar un `MilenaArray` dentro del AST. El AST contiene la descripción de la expresión; la VM/interpreter crea y libera el valor en tiempo de ejecución.

## Orden de implementación

1. Mantener un único lexer y parser para la sintaxis canónica;
2. ampliar el AST para tablas y transformaciones;
3. conectar el runtime de arrays con el modelo común de valores;
4. migrar datasets y tablas sin duplicar kernels;
5. exponer aritmética y broadcasting mediante AST/runtime;
6. ejecutar todos los ejemplos desde `milena run`;
7. integrar VM y compiler solo después de lograr paridad con el intérprete.
