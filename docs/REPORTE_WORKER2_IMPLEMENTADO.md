# Reporte de implementación — Trabajador 2

Fecha de verificación: 2026-09-17 (America/Caracas)

Base indicada para este árbol limpio: GitHub `main` SHA `9614988738bc034e2360aaffc115aad07587277e`.

> El entorno no tenía `git`, por lo que el SHA no pudo consultarse localmente; se trabajó exclusivamente en `milena_repo`, tal como fue solicitado, y no en `Milena_repo`.

## 1. Inventario entregado

Archivos modificados:

- `include/array.h`
- `src/array.c`

Archivos creados:

- `docs/ARRAY_MEMORY_MODEL.md`
- `docs/REPORTE_WORKER2_IMPLEMENTADO.md`
- `tests/test_array_worker2.c`

No se modificaron `common.*`, `main.c`, `Makefile`, `build.sh`, workflows, packaging, `script.c` ni kernels de otros dominios. No se dividió `array.c`.

## 2. APIs públicas añadidas

- `milena_array_init`
- `milena_array_validate`
- `milena_array_swap`
- `milena_array_share`
- `milena_array_mut_data`
- `MilenaSlice`
- `milena_array_slice_view_ex`
- `milena_array_broadcast_view`
- `MilenaArrayBufferDeleter`
- `milena_array_from_buffer`

Se conservaron las APIs anteriores. `milena_array_retain` permanece por ABI, marcado y documentado como deprecado porque solo retiene storage y no duplica metadata.

## 3. Implementación realizada

### Validador y aritmética segura

Se centralizó la validación de dtype/itemsize, metadata escalar y multidimensional, producto de shape, size, flags, storage, buffer, `byte_offset`, alineación, bounds alcanzables y último item. Los bounds consideran separadamente extensión positiva y negativa, aceptan strides positivos/negativos/cero y evitan overflow firmado. Se aplica la regla global stride cero => readonly.

### Ownership y vida útil

El storage privado usa `atomic_size_t` C17. El retain interno detecta saturación antes de incrementar. El release final ejecuta exactamente una vez el deleter y libera el storage. Todas las vistas y shares retienen storage; su vida ya no depende del descriptor padre. `milena_array_share` duplica shape y strides.

### Failure atomicity y aliasing

Todos los productores públicos de `array.c` construyen una salida temporal y hacen commit al final. Un error deja intacta una salida válida anterior. Se rechaza alias de descriptor entre `out` y entradas, además de una salida previa que comparta storage con una entrada. El reemplazo exitoso libera la salida previa.

### Mutabilidad

`milena_array_mut_data` valida y rechaza readonly dejando su salida en `NULL`. El wrapper heredado `milena_array_data` devuelve `NULL` para readonly o descriptor inválido. `milena_array_const_data` valida y evita aritmética sobre `NULL` en buffers vacíos. Readonly se hereda en vistas; broadcast siempre es readonly.

### Vistas

- slicing firmado con start/stop opcionales, índices negativos, step negativo, clipping estilo Python/NumPy y composición sobre strides negativos;
- wrapper legacy positivo conservado;
- transpose y reshape con metadata independiente y commit atómico;
- broadcast zero-copy alineado por la derecha, con strides cero y tamaño lógico comprobado;
- contigüidad segura para escalares, vacíos, singleton y límites de `ptrdiff_t`.

### Buffers externos

`milena_array_from_buffer` admite strides explícitos o contiguos, storage prestado (`deleter == NULL`) o adoptado, contexto de deleter, readonly, escalares y vacíos. La transferencia del deleter ocurre solo en éxito; en fallo no se invoca. Alineación y bounds se validan antes del commit.

### Kernels existentes de array

Se corrigió `where` para exigir condición bool, shapes idénticos y ramas con dtype/itemsize iguales; recorre correctamente vistas no contiguas. También se migraron cast, comparaciones, máscaras, nonzero, binarios, sumas y estadísticas del archivo a validación, recorrido por strides y salidas temporales. No se añadió promoción de dtype ni kernels nuevos.

## 4. Prueba específica

`tests/test_array_worker2.c` cubre:

- dtype incompatible en `where`;
- descriptor inválido;
- salida previa intacta tras errores;
- alias de descriptor y de storage;
- vida de vista después del padre;
- share y metadata independiente;
- readonly y accessor mutable;
- slices negativos, clipping y composición;
- `where` sobre tres vistas invertidas;
- broadcast zero-copy;
- buffers externos prestados/adoptados;
- deleter exactamente una vez;
- buffer desalineado y stride cero writable inválidos;
- buffer con stride negativo;
- arrays vacíos y escalares;
- contigüidad y no contigüidad;
- swap;
- overflows de shape.

## 5. Toolchain detectado

No estaban disponibles `cc`, `gcc`, `clang` independiente ni `make`. Sí estaba disponible Python. Se instaló temporalmente el paquete local `ziglang 0.16.0` y se usó `python3 -m ziglang cc`; su frontend reportó `clang version 21.1.0`. No se presenta esto como GCC ni como Clang instalado autónomamente.

El toolchain temporal no forma parte de la entrega ni del ZIP.

## 6. Comandos y resultados reales

### Suite Worker 2, warnings-as-errors exactos

```sh
python3 -m ziglang cc \
  -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -Iinclude src/common.c src/array.c tests/test_array_worker2.c \
  -lm -o tests/test_array_worker2
./tests/test_array_worker2
```

Resultado:

```text
OK: worker2 array memory model, views, buffers and atomic outputs
```

`src/array.c` y `src/common.c` también se compilaron por separado con los mismos warnings y `-Werror`: sin diagnósticos.

### ASan + UBSan

```sh
python3 -m ziglang cc \
  -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude src/common.c src/array.c tests/test_array_worker2.c \
  -lm -o tests/test_array_worker2_san
ASAN_OPTIONS=detect_leaks=1 ./tests/test_array_worker2_san
```

Resultado: `OK`, sin reporte de AddressSanitizer, UndefinedBehaviorSanitizer ni leak detector.

La suite heredada `tests/test_array.c` también pasó bajo ASan/UBSan.

### Suite heredada de array

El intento literal con todos los flags solicitados y `-Werror` encontró cuatro warnings preexistentes dentro de `tests/test_array.c` (conversiones implícitas `ptrdiff_t`/`size_t` en las líneas 124-125 y 132-133). Esos warnings están en el test heredado, no en los archivos del trabajador, y modificar ese test o sus expectativas no era necesario.

Se recompiló sin desactivar ningún warning de la implementación, añadiendo únicamente `-Wno-sign-conversion` para esas expresiones del test:

```sh
python3 -m ziglang cc \
  -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
  -Wno-sign-conversion -Werror \
  -Iinclude src/common.c src/array.c tests/test_array.c \
  -lm -o tests/test_array_w2
./tests/test_array_w2
```

Resultado:

```text
OK: MilenaArray creation, views, broadcasting and reductions
```

### Compatibilidad adicional

Se recompilaron y ejecutaron manualmente:

- `tests/test_table.c`: `OK: MilenaTable columns, validity and filtering`;
- `tests/test_forest.c`: `OK: Milena forest classifier`.

Ambos conservaron su comportamiento. Sus fuentes de test producen warnings preexistentes de variables no usadas, pero no hubo warnings nuevos atribuibles a `array.c`.

Se enlazó además el conjunto de fuentes usado por el target principal del Makefile con Zig cc. No hubo errores de enlace; apareció un warning preexistente de función no usada en `src/finance.c`.

## 7. Limitaciones y coordinación

1. **Suite heredada con flags exactos:** `tests/test_array.c` no puede compilar con `-Wconversion -Werror` sin corregir sus cuatro expresiones de aritmética de punteros. No se modificó porque la entrega exige una única prueba nueva autocontenida y conservar expectativas. La implementación y la prueba Worker 2 sí pasan exactamente esos flags.
2. **Tests `test_language_array.c` y `test_parser_array.c`:** se intentó compilarlos por llamarse suites de array, pero este SHA contiene incompatibilidades preexistentes entre lexer/parser y `common.h` (`MAX_TOKEN_LEN`, `MilenaErrorInfo` y API de error ausentes), además de includes faltantes en lexer. No enlazan `src/array.c`, quedan fuera del alcance autorizado y no se tocaron.
3. **ABI de `milena_array_data`:** la firma se conserva; el cambio semántico intencional es devolver `NULL` en readonly. Los módulos existentes solo lo usan sobre outputs propios mutables y pasaron sus pruebas.
4. **Sin estado mágico oculto:** no se añadió un cookie al struct público para evitar romper su layout. El estado cero se reconoce estructuralmente; el contrato exige inicializar `out`. `milena_array_validate` es la comprobación fiable de arrays vivos, mientras que el estado cero es liberable pero no un array válido.
5. **Thread-safety:** el refcount y deleter final son thread-safe entre descriptores independientes; metadata, el mismo descriptor y los elementos requieren sincronización del llamador, como se documenta.

No queda ningún criterio funcional del alcance sin implementar. Las únicas desviaciones de verificación son las advertencias/incompatibilidades preexistentes detalladas arriba y fuera de los archivos autorizados.
