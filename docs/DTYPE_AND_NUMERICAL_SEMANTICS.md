# Semántica de dtypes y operaciones numéricas de MilenaArray

Este documento fija el contrato numérico implementado por el Trabajador 3. Aplica a `MilenaArray` y a las APIs públicas declaradas en `include/array.h`.

## 1. Dtypes soportados

| dtype | almacenamiento | cast real | aritmética | comparaciones | reducciones |
|---|---:|---:|---:|---:|---:|
| `bool` | sí | sí | sí | sí | sí |
| `int8`, `int16`, `int32`, `int64` | sí | sí | sí | sí | sí |
| `uint8`, `uint16`, `uint32`, `uint64` | sí | sí | sí | sí | sí |
| `float32`, `float64` | sí | sí | sí | sí | sí |
| `complex64`, `complex128` | sí | no | no | no | no |

`complex64` y `complex128` son **storage-only**. Se pueden crear, compartir, cortar, transponer, hacer reshape y copiar como bytes mediante las APIs de memoria/vistas. Casts y operaciones numéricas que los reciben devuelven uniformemente `MILENA_ERR_UNSUPPORTED`; no se simula soporte descartando la parte imaginaria.

## 2. Matriz central de promoción

`milena_dtype_promote(a, b, &out, error)` es pública, simétrica y es la única fuente de promoción usada por aritmética y comparaciones binarias.

Abreviaturas: `b=bool`, `i8=int8`, `i16=int16`, `i32=int32`, `i64=int64`, `u8=uint8`, `u16=uint16`, `u32=uint32`, `u64=uint64`, `f32=float32`, `f64=float64`.

| × | b | i8 | i16 | i32 | i64 | u8 | u16 | u32 | u64 | f32 | f64 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **b** | u8 | i16 | i16 | i32 | i64 | u8 | u16 | u32 | u64 | f32 | f64 |
| **i8** | i16 | i8 | i16 | i32 | i64 | i16 | i32 | i64 | f64 | f32 | f64 |
| **i16** | i16 | i16 | i16 | i32 | i64 | i16 | i32 | i64 | f64 | f32 | f64 |
| **i32** | i32 | i32 | i32 | i32 | i64 | i32 | i32 | i64 | f64 | f64 | f64 |
| **i64** | i64 | i64 | i64 | i64 | i64 | i64 | i64 | i64 | f64 | f64 | f64 |
| **u8** | u8 | i16 | i16 | i32 | i64 | u8 | u16 | u32 | u64 | f32 | f64 |
| **u16** | u16 | i32 | i32 | i32 | i64 | u16 | u16 | u32 | u64 | f32 | f64 |
| **u32** | u32 | i64 | i64 | i64 | i64 | u32 | u32 | u32 | u64 | f64 | f64 |
| **u64** | u64 | f64 | f64 | f64 | f64 | u64 | u64 | u64 | u64 | f64 | f64 |
| **f32** | f32 | f32 | f32 | f64 | f64 | f32 | f32 | f64 | f64 | f32 | f64 |
| **f64** | f64 | f64 | f64 | f64 | f64 | f64 | f64 | f64 | f64 | f64 | f64 |

Criterios:

1. `bool` participa como entero sin signo de 8 bits; `bool op bool` produce `uint8`, evitando representar `1 + 1` falsamente como booleano.
2. Enteros del mismo signo conservan el mayor ancho.
3. En mezcla signed/unsigned se elige un signed que pueda representar ambos dominios cuando existe.
4. La mezcla que incluye `uint64` y cualquier signed usa `float64`, porque no existe un entero real del ABI capaz de representar ambos dominios.
5. `float32` conserva `float32` con enteros de hasta 16 bits; con enteros de 32/64 bits promueve a `float64`. Cualquier `float64` produce `float64`.

La promoción define el dtype de salida, no una promesa de aritmética modular. Toda operación debe ser representable en ese dtype.

## 3. Casting

`milena_array_cast` recorre lógicamente `shape/strides`; funciona con arrays contiguos, transpose, slices con stride positivo o negativo y broadcast views con stride cero.

Reglas:

- entero → entero: conversión exacta; fuera de rango devuelve `MILENA_ERR_OVERFLOW`;
- entero → flotante: se permite el redondeo IEEE inherente al dtype de destino; desborde finito devuelve `MILENA_ERR_OVERFLOW`;
- flotante → entero: primero se rechaza NaN/±Inf con `MILENA_ERR_TYPE`, luego se trunca hacia cero y se verifica el rango; fuera de rango devuelve `MILENA_ERR_OVERFLOW`;
- flotante → flotante: NaN/±Inf se preservan; un finito que no cabe devuelve `MILENA_ERR_OVERFLOW`;
- cualquier real → `bool`: cero (incluido `-0.0`) es `false`; todo valor distinto de cero, incluido NaN, es `true`;
- complex en origen o destino: `MILENA_ERR_UNSUPPORTED`.

La implementación evita conversiones C fuera de rango. La salida se construye en un temporal y solo se publica al terminar: un error deja intacta una salida previa válida. Se mantienen las restricciones de aliasing de descriptores/storage del modelo W2.

## 4. Aritmética y dispatcher

`add`, `subtract`, `multiply` y `divide` comparten:

- la matriz central de promoción como punto de partida;
- el mismo dispatcher central `DTypeKernel`;
- el mismo recorrido broadcast lógico;
- los mismos helpers checked signed/unsigned;
- una única reserva temporal por salida y, como máximo, metadata/coordenadas por operación; nunca hay `malloc` por elemento.

Los enteros usan aritmética comprobada. No hay wraparound como semántica pública. Underflow unsigned, overflow signed/unsigned o un resultado que no cabe en el dtype promovido producen `MILENA_ERR_OVERFLOW`.

`milena_array_divide` implementa **división real**: cuando ambos operandos son enteros o `bool`, el dtype de salida es `float64` y no hay truncamiento (`-7 / 2 == -3.5`). Esto alinea array/array con la ruta array/escalar del lenguaje. No se expone división entera truncada y el operador `/` tiene una sola semántica. División por `+0`, `-0.0` o cero entero devuelve `MILENA_ERR_ARGUMENT`. Operaciones float conservan la propagación IEEE de NaN/Inf; si operandos finitos producen un resultado no finito o fuera del dtype de salida, se devuelve `MILENA_ERR_OVERFLOW`.

Los kernels de resultado `float32` y `float64` convierten primero ambos operandos a ese dtype y ejecutan allí la operación. No usan la precisión incidental de `long double`, por lo que los límites `2^53`, `INT64_MAX`, `2^63` y `UINT64_MAX` tienen la misma semántica en Linux y Windows. Las comparaciones entre dos dominios enteros son exactas (incluida la mezcla signed/unsigned) antes de producir `bool`; una promoción nominal a `float64` no colapsa enteros distintos.

El overflow de aritmética entera conserva el código `MILENA_ERR_OVERFLOW` y usa el diagnóstico estable `La operación entera está fuera de rango`. La frase `fuera de rango` forma parte del contrato de compatibilidad del diagnóstico E2E.

## 5. Broadcasting y strides

Las formas se alinean por la derecha. En cada eje, las dimensiones deben coincidir o una de ellas debe ser 1. El tamaño y la metadata se calculan con comprobación de overflow. Un eje expandido se recorre con coordenada cero/stride lógico cero.

Todos los kernels leen mediante `byte_offset + coordinates × strides`, respetando:

- vistas transpose;
- slices no contiguos;
- strides negativos;
- strides cero de broadcast readonly.

Las salidas numéricas siempre son buffers nuevos C-contiguos y escribibles. Nunca se escribe en una broadcast view readonly.

## 6. Reducciones

### Dtypes de salida

| operación | entrada | salida |
|---|---|---|
| `sum`, `prod` | cualquier real | signed → `int64`; bool/unsigned → `uint64`; float → `float64` |
| `mean`, `min`, `max`, `variance`, `std` | cualquier real | `float64` |
| `median`, `percentile` | cualquier real | `float64` |
| `argmin`, `argmax` | cualquier real | escalar `int64` |

`bool` pertenece al grupo unsigned para `sum`/`prod`.

### Acumulación

- `sum` y `prod` enteros usan acumuladores `int64`/`uint64` comprobados; overflow devuelve `MILENA_ERR_OVERFLOW`.
- `sum` flotante conserva acumulación compensada; `mean` usa una suma compensada escalada en `float64`, evitando overflow intermedio incluso cuando `long double == double`.
- `variance` usa Welford sobre valores escalados, política poblacional **ddof=0**. `std = sqrt(variance)`. Si inputs finitos producen un resultado no representable se devuelve `MILENA_ERR_OVERFLOW`; NaN/Inf de entrada se propagan separadamente y no se confunden con overflow de cálculo.
- `min` y `max` propagan NaN.
- NaN e Inf siguen propagación IEEE en sum/mean/variance/std. Una mezcla indeterminada como `+Inf + -Inf` produce NaN.
- `argmin`/`argmax` devuelven la primera posición C-order en empates y rechazan cualquier NaN con `MILENA_ERR_ARGUMENT`.
- Ninguna reducción muta el input.

### Ejes y keepdims

Por compatibilidad histórica, `axis == -1` significa reducción global. Ejes no negativos seleccionan el eje normal. Se añadieron ejes negativos `-2`, `-3`, … contando desde el final; así se conserva el sentinel histórico, aunque el último eje no puede expresarse como `-1` porque ese valor sigue significando global. Ejes fuera de rango devuelven `MILENA_ERR_ARGUMENT`.

`keepdims=true` conserva el número de dimensiones y reemplaza cada eje reducido por longitud 1. En reducción global conserva todos los ejes con longitud 1.

El ABI actual representa un solo `int axis`; tuples de ejes quedan fuera del ABI y no se simulan.

### Arrays vacíos

- `sum(empty) = 0` y `prod(empty) = 1`, con el dtype de acumulación indicado.
- `mean`, `min`, `max`, `variance`, `std`, `median`, `percentile`, `argmin` y `argmax` de un conjunto reducido vacío devuelven `MILENA_ERR_ARGUMENT`.
- Una reducción por eje cuyo eje tiene longitud no nula pero cuya salida contiene cero elementos produce correctamente una salida vacía.

## 7. Percentiles

El percentil debe ser finito y estar en `[0, 100]`. Se materializa cada segmento lógico strided en un buffer `double`, se ordena y se usa interpolación lineal:

`position = q/100 × (n-1)`

El resultado interpola entre `floor(position)` y el siguiente índice. `q=0` y `q=100` seleccionan los extremos. Para extremos finitos de signo opuesto se usa una combinación convexa que no evalúa `upper-lower`, evitando overflow en `[-DBL_MAX, DBL_MAX]`; para extremos del mismo signo se usa la diferencia segura. La presencia de cualquier NaN propaga NaN. Entre un finito y un infinito interior se devuelve ese infinito; entre `-Inf` y `+Inf` se devuelve NaN. Un segmento vacío es error.

## 8. APIs nuevas

- `milena_dtype_promote`
- `milena_array_prod`
- `milena_array_equal`, `milena_array_less`, `milena_array_greater`
- `milena_array_isnan`, `milena_array_isfinite`
- `milena_array_argmin`, `milena_array_argmax`

Las comparaciones binarias hacen broadcasting y devuelven `bool`. Con NaN, `equal`, `less` y `greater` devuelven `false`. `isnan` es siempre `false` para enteros/bool; `isfinite` es siempre `true` para enteros/bool.

`argmin`/`argmax` nuevos son globales. No se añadieron variantes por eje para evitar inflar el ABI sin una decisión coordinada sobre nombres y tuples de ejes.
