# Reporte de implementación — Trabajador 3

## 1. Alcance ejecutado

Se convirtió la capa numérica de `MilenaArray` en un runtime coherente, autocontenido en `src/array.c`, sin crear `dtype.c` y sin modificar ownership/vistas más allá de consumir el modelo de memoria del Trabajador 2. La fase correctiva posterior unificó la división real, estabilizó el diagnóstico de overflow e integró las suites Worker 2 y Worker 3 en el Makefile.

### Implementado

- dispatcher central por dtype real (`DTypeKernel` + loaders tipados);
- API pública de promoción `milena_dtype_promote` y matriz exacta 11×11;
- casts completos entre bool/signed/unsigned/float, con rango y failure atomicity;
- complex64/128 declarado y aplicado como storage-only;
- add/subtract/multiply con promoción y broadcasting;
- `divide` como división real: dos operandos enteros/`bool` producen `float64`, sin truncamiento;
- aritmética integer checked, sin wraparound ni UB, con `MILENA_ERR_OVERFLOW` y diagnóstico estable que incluye `fuera de rango`;
- recorrido shape/strides para arrays contiguos, transpose, slices positivos/negativos y broadcast stride cero;
- sum/prod globales y por eje;
- mean/min/max/variance/std globales y por eje;
- median/percentile globales y por eje;
- suma Neumaier y varianza Welford poblacional (`ddof=0`);
- percentil lineal `[0,100]`, strided y con propagación de NaN;
- nuevas APIs `prod`, `equal`, `less`, `greater`, `isnan`, `isfinite`, `argmin`, `argmax`;
- soporte `keepdims` y ejes negativos compatibles con el sentinel histórico;
- pruebas numéricas en `tests/test_array_worker3.c` y regresión canónica de división en `tests/test_array.c`;
- regresión real de Worker 2, Worker 3 y array heredado;
- suites Worker 2 y Worker 3 integradas en los objetivos `test` y `clean` del Makefile;
- ASan + UBSan + detección de leaks para Worker 2 y Worker 3.

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
- `milena_array_divide` y el operador `/` hacen división real; dos operandos enteros/`bool` producen `float64` (`-7 / 2 == -3.5`). No existe una segunda semántica truncada bajo `/`.
- Float→integer en un cast explícito sí trunca hacia cero; NaN/Inf devuelve `MILENA_ERR_TYPE`; fuera de rango devuelve `MILENA_ERR_OVERFLOW`.
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

## 5. Archivos de la fase correctiva

Modificados respecto del head inicial `8dc6a2f32369d612fd444beae2a43d3af5f6240a`:

1. `include/array.h`
2. `src/array.c`
3. `tests/test_array_worker3.c`
4. `tests/test_array.c`
5. `tests/run_tests.sh`
6. `docs/DTYPE_AND_NUMERICAL_SEMANTICS.md`
7. `docs/REPORTE_WORKER3_IMPLEMENTADO.md`
8. `Makefile` (en un commit separado del cambio numérico)

No se modificaron `script.c`, `common.*`, `main.c`, frontend/parser, table/dataset, finance, SST, forest, workflows ni packaging.

## 6. Verificación real de la fase correctiva (2026-09-18)

### Toolchain detectado

No había `cc` ni `make` en PATH. Se instaló temporalmente `ziglang==0.16.0` y se usó `python3 -m ziglang cc` (versión confirmada `0.16.0`). No se afirma haber usado GCC/Clang independientes ni haber ejecutado `make test`. Las recetas pertinentes se ejecutaron manualmente.

### Worker 2 y Worker 3 con warnings estrictos

Ambas suites se compilaron con C17 y `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`:

```sh
python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  src/common.c src/array.c tests/test_array_worker2.c -lm -o test_worker2
./test_worker2

python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  src/common.c src/array.c tests/test_array_worker3.c -lm -o test_worker3
./test_worker3
```

Resultados:

```text
OK: worker2 array memory model, views, buffers and atomic outputs
OK: worker3 dtype, kernels, broadcasting and reductions
```

### Regresión array heredada

Se compiló con el mismo set estricto y la supresión mínima preexistente `-Wno-sign-conversion`, necesaria solo por conversiones signed/unsigned del propio test heredado:

```sh
python3 -m ziglang cc -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -Wno-sign-conversion src/common.c src/array.c tests/test_array.c \
  -lm -o test_array_legacy
./test_array_legacy
```

Resultado: `OK: MilenaArray creation, views, broadcasting and reductions`.

### ASan y UBSan

Worker 2 y Worker 3 se recompilaron con el set estricto más `-fsanitize=address,undefined -fno-omit-frame-pointer` y se ejecutaron con `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` y `UBSAN_OPTIONS=halt_on_error=1`. Ambas suites terminaron OK sin diagnósticos de ASan, UBSan ni LeakSanitizer.

### Producto completo y E2E

El producto completo se compiló manualmente con la lista `SOURCES` y flags por defecto equivalentes del Makefile (`-std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -Iinclude`, `-lm`). Terminó correctamente; persiste un warning heredado y fuera de alcance en `src/finance.c` por `decimal_negate` no usada.

Con ese binario se ejecutó realmente:

```sh
bash tests/run_tests.sh
```

Resultado: PASS y `OK: pruebas con datos sintéticos temporales completadas`. La salida verificó `Operacion enteros / otros: dtype=float64, shape=(3)` y la regresión de overflow exigió el código/mensaje completo `Milena [OVERFLOW]: La operación entera está fuera de rango`.

El Makefile quedó preparado para incluir Worker 2 y Worker 3 en `test` y sus binarios en `clean`; no se afirma ejecución de `make test` porque `make` no estaba disponible.

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
- división real signed (`-7/2 == -3.5`), dtype `float64` para integer/integer y división por cero;
- código `MILENA_ERR_OVERFLOW` y mensaje exacto estable para overflow entero;
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

1. **Complex:** continúa intencionalmente storage-only. Falta definir promoción real↔complex, comparaciones y estadísticas antes de implementar aritmética.
2. **Axis tuples:** el ABI actual recibe un solo `int axis`; no se añadieron tuples.
3. **`ddof` público:** variance/std mantienen `ddof=0`; falta una API pública para elegirlo.
4. **Arg extrema por eje:** `argmin`/`argmax` siguen siendo globales; faltan variantes axis/keepdims.
5. **Separación completa de `array.c`:** el dispatcher continúa autocontenido en este archivo; la partición física queda para un bloque posterior.
6. **Sentinel histórico:** `axis=-1` continúa significando global. Los negativos adicionales `-2`, `-3`, … cuentan desde el final; una API v2 debería separar explícitamente “todos los ejes” del último eje estilo NumPy.
7. **Min/max integer:** conservan salida `float64`; enteros de 64 bits pueden redondearse y cualquier cambio de dtype requiere una decisión coordinada.

La ruta array/escalar ya coincidía con división real. No fue necesario modificar `script.c`: al usar `milena_array_divide`, array/array adoptó la misma semántica canónica sin duplicar kernels.

No se declara implementado nada fuera de esta lista.
