# Reporte de implementación — Trabajador 3

## 1. Alcance ejecutado

Se convirtió la capa numérica de `MilenaArray` en un runtime coherente, autocontenido en `src/array.c`, sin crear `dtype.c`, sin tocar Makefile y sin modificar ownership/vistas más allá de consumir el modelo de memoria del Trabajador 2.

### Implementado

- dispatcher central por dtype real (`DTypeKernel` + loaders tipados);
- API pública de promoción `milena_dtype_promote` y matriz exacta 11×11;
- casts completos entre bool/signed/unsigned/float, con rango y failure atomicity;
- complex64/128 declarado y aplicado como storage-only;
- add/subtract/multiply/divide con promoción y broadcasting;
- aritmética integer checked, sin wraparound ni UB;
- recorrido shape/strides para arrays contiguos, transpose, slices positivos/negativos y broadcast stride cero;
- sum/prod globales y por eje;
- mean/min/max/variance/std globales y por eje;
- median/percentile globales y por eje;
- suma Neumaier y varianza Welford poblacional (`ddof=0`);
- percentil lineal `[0,100]`, strided y con propagación de NaN;
- nuevas APIs `prod`, `equal`, `less`, `greater`, `isnan`, `isfinite`, `argmin`, `argmax`;
- soporte `keepdims` y ejes negativos compatibles con el sentinel histórico;
- pruebas exhaustivas nuevas en un único `tests/test_array_worker3.c`;
- regresión real de Worker 2, array heredado, table y forest;
- ASan + UBSan + detección de leaks.

## 2. Matriz exacta de soporte

| familia/API | bool | signed | unsigned | float | complex |
|---|---:|---:|---:|---:|---:|
| creación/storage/vistas/copia de bytes | sí | sí | sí | sí | sí |
| cast numérico | sí | sí | sí | sí | no: `UNSUPPORTED` |
| add/subtract/multiply/divide | sí | sí | sí | sí | no: `UNSUPPORTED` |
| equal/less/greater | sí | sí | sí | sí | no: `UNSUPPORTED` |
| isnan/isfinite | sí | sí | sí | sí | no: `UNSUPPORTED` |
| sum/prod | sí→u64 | sí→i64 | sí→u64 | sí→f64 | no: `UNSUPPORTED` |
| mean/min/max/variance/std | sí→f64 | sí→f64 | sí→f64 | sí→f64 | no: `UNSUPPORTED` |
| median/percentile | sí→f64 | sí→f64 | sí→f64 | sí→f64 | no: `UNSUPPORTED` |
| argmin/argmax global | sí→i64 | sí→i64 | sí→i64 | sí→i64 | no: `UNSUPPORTED` |

La matriz completa dtype×dtype de promoción está en `docs/DTYPE_AND_NUMERICAL_SEMANTICS.md` y además está codificada como referencia 11×11 en el test C.

## 3. Contratos numéricos

- Mezclas signed/unsigned eligen un signed capaz de representar ambos dominios; si intervienen `uint64` y signed, el resultado es `float64`.
- Operaciones integer que no caben en el dtype promovido devuelven `MILENA_ERR_OVERFLOW`.
- División por cero integer o float devuelve `MILENA_ERR_ARGUMENT`.
- División integer trunca hacia cero.
- Float→integer trunca hacia cero; NaN/Inf devuelve `MILENA_ERR_TYPE`; fuera de rango devuelve `MILENA_ERR_OVERFLOW`.
- NaN/Inf se conservan en casts float→float y se propagan con reglas IEEE en estadísticas. Min/max/percentile propagan NaN.
- `argmin`/`argmax` rechazan NaN y seleccionan la primera posición C-order en empates.
- `sum(empty)=0`, `prod(empty)=1`; estadísticas y orden de un conjunto reducido vacío devuelven `MILENA_ERR_ARGUMENT`.
- Variance conserva política poblacional `ddof=0`.
- Percentiles usan interpolación lineal en `position=(q/100)*(n-1)`.

## 4. Compatibilidad con Worker 2

No se revirtió ni modificó el diseño de:

- atomic refcount;
- buffers externos y deleters;
- readonly;
- broadcast views readonly;
- slicing con step negativo;
- transpose/reshape/share;
- validador central;
- salida temporal y commit failure-atomic;
- prohibición de alias de descriptor/storage de salida.

Todos los kernels nuevos publican la salida solo tras completar con éxito. Un overflow, cast inválido, dtype no soportado o división por cero deja intacta una salida previa válida.

## 5. Archivos de entrega

Modificados respecto del baseline Worker 2:

1. `include/array.h`
2. `src/array.c`

Nuevos:

3. `docs/DTYPE_AND_NUMERICAL_SEMANTICS.md`
4. `docs/REPORTE_WORKER3_IMPLEMENTADO.md`
5. `tests/test_array_worker3.c`

No se modificaron `common.*`, `main.c`, Makefile, build scripts, script/frontend/parser, table/dataset, finance, SST, forest, workflows ni packaging.

## 6. Verificación real

### Toolchain detectado

No había `cc` ni `make` en PATH. Se instaló temporalmente el paquete `ziglang==0.16.0` y se compiló mediante:

```sh
python3 -m ziglang cc
```

Versión confirmada: `0.16.0`. Esto es el driver C incluido por Zig; no se afirma haber usado GCC o un Clang del sistema.

### Worker 3, warnings estrictos

```sh
python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  src/common.c src/array.c tests/test_array_worker3.c -lm \
  -o test_worker3
./test_worker3
```

Resultado:

```text
OK: worker3 dtype, kernels, broadcasting and reductions
```

### ASan + UBSan + leaks

```sh
python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  src/common.c src/array.c tests/test_array_worker3.c -lm \
  -o test_worker3_san
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 ./test_worker3_san
```

Resultado: test completo OK, sin diagnóstico de ASan, UBSan ni LeakSanitizer.

También se ejecutaron con los mismos sanitizadores `tests/test_array_worker2.c` y `tests/test_array.c`; ambos terminaron OK y sin diagnósticos.

### Regresión Worker 2

```sh
python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  src/common.c src/array.c tests/test_array_worker2.c -lm \
  -o test_worker2
./test_worker2
```

Resultado:

```text
OK: worker2 array memory model, views, buffers and atomic outputs
```

### Regresión array heredada

El test heredado tiene cuatro conversiones signed/unsigned preexistentes en aritmética de punteros (`size_t × ptrdiff_t`). La implementación `src/array.c` sí compila con todos los warnings y `-Werror`; únicamente para ese archivo de test heredado se añadió la supresión mínima:

```sh
-Wno-sign-conversion
```

Comando:

```sh
python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -Wno-sign-conversion \
  src/common.c src/array.c tests/test_array.c -lm \
  -o test_array_legacy
./test_array_legacy
```

Resultado:

```text
OK: MilenaArray creation, views, broadcasting and reductions
```

### Table y forest

Ambos compilaron sin supresiones bajo el set estricto completo y ejecutaron OK:

```text
OK: MilenaTable columns, validity and filtering
OK: Milena forest classifier
```

## 7. Cobertura de `tests/test_array_worker3.c`

- matriz completa 11×11 de promoción y simetría implícita;
- rechazo uniforme de complex;
- casts float→signed con truncamiento positivo/negativo;
- NaN/Inf→integer;
- NaN→bool;
- float64→float32 fuera de rango;
- uint64→int64 fuera de rango;
- failure atomicity de cast;
- signed/unsigned mixto;
- overflow int8;
- underflow uint8;
- división signed hacia cero y división por cero;
- promoción uint64+int64;
- broadcasting 2D+1D;
- transpose, slice negativo y broadcast stride cero;
- propiedades metamórficas `x+0=x` y suma invariante a transpose;
- comparaciones y semántica NaN;
- isnan/isfinite;
- sum/prod globales y por eje;
- keepdims;
- eje negativo `-2` preservando `-1` global;
- suma compensada sobre cancelación severa;
- min/max/variance/std;
- overflow de sum int64;
- identidades de vacíos y errores estadísticos de vacíos;
- min con NaN;
- median/percentile strided, límites de percentil y axis+keepdims;
- argmin/argmax C-order y rechazo de NaN.

No se usa NumPy ni otra dependencia de ejecución.

## 8. Desviaciones y coordinación pendiente

1. **Complex:** intencionalmente storage-only. Implementar aritmética complex correctamente requiere una política de promoción real↔complex, orden/comparaciones y estadísticas separada; no se fingió soporte.
2. **Axis tuples:** fuera del ABI actual (`int axis`). No se añadieron.
3. **Sentinel histórico:** `axis=-1` continúa significando global para no romper `tests/test_array.c` y consumidores existentes. Los negativos adicionales `-2`, `-3`, … cuentan desde el final. Conviene decidir en coordinación futura si una API v2 separa explícitamente “global/all axes” de los ejes negativos estilo NumPy.
4. **Arg extrema por eje:** las nuevas APIs `argmin`/`argmax` son globales. Variantes axis/keepdims se dejaron para una decisión coordinada de ABI.
5. **Array-escalar:** no se tocó `script.c`, según la restricción. Array-array quedó como base única coherente; la migración del adaptador escalar de script debe coordinarse con el propietario de frontend/runtime.
6. **Min/max integer:** conservan el resultado `float64` de las APIs estadísticas previas. Valores integer de 64 bits pueden redondearse al representarse en `float64`; cambiar el dtype de salida sería una ruptura observable y debe decidirse de forma coordinada.

No se declara implementado nada fuera de esta lista.
