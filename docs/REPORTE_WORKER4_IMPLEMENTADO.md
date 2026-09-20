# Reporte de implementación — Trabajador 4

Fecha de verificación: 2026-09-18  
Árbol exclusivo: `milena_worker4`

## Resumen

Se convirtió `MilenaTable` en una capa tipada y validable sobre `MilenaArray`, conservando el prefijo ABI/API existente. Se implementaron strings UTF-8 con ownership profundo, categoricals dictionary-encoded, metadata, clone profundo, semántica uniforme de null, salidas failure-atomic, filtro strided, sort estable `O(n log n)`, group-by hash determinista, cuatro joins hash y unpivot.

La desviación deliberada es **pivot**, que no se declara ni se finge implementado. Se priorizaron invariantes/ownership, failure atomicity, sort, group-by y joins conforme al orden indicado. Unpivot sí está completo para columnas de valor homogéneas.

## Archivos modificados/creados

1. `include/table.h` — ampliación compatible de structs, enums y APIs.
2. `src/table.c` — implementación completa en el módulo permitido; no se añadieron módulos `.c`.
3. `tests/test_table_worker4.c` — pruebas nuevas, incluida escala de 100,000 filas.
4. `docs/TABLE_SCHEMA_AND_RELATIONAL_SEMANTICS.md` — contrato semántico.
5. `docs/REPORTE_WORKER4_IMPLEMENTADO.md` — este reporte.

No se modificaron `array.*`, `common.*`, `main.c`, `Makefile`, `build.sh` ni los demás componentes excluidos.

## API pública añadida

### Tipos

- `MilenaColumnType`: `MILENA_COLUMN_ARRAY`, `MILENA_COLUMN_STRING`, `MILENA_COLUMN_CATEGORICAL`.
- `MilenaTableMetadata`.
- `MilenaJoinType`: inner, left, right, full.
- `MilenaSortKey`.
- `MilenaAggregateSpec`.
- `MilenaTableValue`.

### Ciclo de vida, schema y acceso

- `milena_table_swap`
- `milena_table_validate`
- `milena_table_clone`
- `milena_table_add_string_column_copy`
- `milena_table_add_categorical_column_copy`
- `milena_table_is_null`
- `milena_table_get_array_value`
- `milena_table_get_string`
- `milena_table_get_category`

### Metadata

- `milena_table_set_metadata`
- `milena_table_get_metadata`
- `milena_table_column_set_metadata`
- `milena_table_column_get_metadata`

### Null, sort y selección

- `milena_table_fill_null`
- `milena_table_drop_null_columns`
- `milena_table_sort_keys`

### Relacional

- `milena_table_group_by`
- `milena_table_join`
- `milena_table_unpivot`

Se conservaron las funciones heredadas, incluidos `milena_table_add_column_copy`, `milena_table_fill_null_f64`, `milena_table_sort` y `milena_table_group_by_aggregate`.

## Garantías implementadas

- Nombres UTF-8 no vacíos y únicos.
- Strings, diccionarios, metadata, validity y arrays con ownership profundo e inequívoco.
- Validación de UTF-8 (secuencias, sobrelongitud, surrogates y rango Unicode).
- Validación central de row count, tag lógico, storage, códigos categóricos, nullable y metadata.
- `init`/`destroy` repetibles; clone sin copias superficiales.
- Operaciones que producen tabla construyen un temporal y hacen commit al final; output válido previo se conserva ante error.
- Rechazo de `out == source/left/right`.
- Acceso 1-D por stride público; no se accede a `MilenaArrayStorage`.
- Filtro con máscara strided y tablas vacías.
- Null separado de NaN.
- Mergesort estable bottom-up sin `qsort` ni estado global.
- Group-by hash open-addressed, ordenado por primera aparición.
- Claves múltiples y agregaciones múltiples.
- Sumas enteras checked y COUNT checked; suma float compensada.
- Joins duplicate-aware con producto cartesiano determinista y resolución estable de colisiones de nombres.
- Unpivot con validez preservada y orden determinista.

## Complejidad

- Clone/filter/select/drop-null/unpivot: lineal en datos materializados.
- Sort: `O(n log n)` tiempo, `O(n)` índices auxiliares.
- Group-by: esperado `O(n·k + n·a)`, con `k` claves y `a` agregados; `O(n + g)` memoria.
- Join: esperado `O(left + right + output)` más comparación de claves; `O(right + output)` memoria.

## Verificación real

No había `cc`, GCC ni Clang del sistema disponible. Se instaló temporalmente `ziglang==0.16.0` y se usó **`python3 -m ziglang cc` (frontend Clang de Zig)**. El toolchain temporal y todos los binarios se eliminaron al terminar.

Compilación estricta ejecutada:

```sh
python3 -m ziglang cc -std=c17 \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -Iinclude src/common.c src/array.c src/table.c \
  tests/test_table_worker4.c -lm -o test_table_worker4
./test_table_worker4
```

Resultado:

```text
OK: worker4 typed tables, relational operations and 100k scale
```

Sanitizers ejecutados:

```sh
python3 -m ziglang cc -std=c17 \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude src/common.c src/array.c src/table.c \
  tests/test_table_worker4.c -lm -o test_table_worker4_san
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 ./test_table_worker4_san
```

Resultado: mismo `OK`, sin reporte ASan, UBSan ni leak detector.

Regresiones compiladas con las mismas flags estrictas y ejecutadas:

```text
OK: MilenaTable columns, validity and filtering
OK: worker3 dtype, kernels, broadcasting and reductions
OK: worker2 array memory model, views, buffers and atomic outputs
OK: Milena forest classifier
```

Corresponden a:

- `tests/test_table.c` con `common.c + array.c + table.c`;
- `tests/test_array_worker3.c` con `common.c + array.c`;
- `tests/test_array_worker2.c` con `common.c + array.c`;
- `tests/test_forest.c` con `common.c + array.c + forest.c`.

## Cobertura del test Worker 4

- columnas int/bool/string/categorical y null;
- validación, UTF-8, nombres duplicados y metadata;
- clone profundo;
- acceso seguro;
- failure atomicity con output previo;
- filtro y columna fuente strided;
- selección, fill-null y drop-null;
- sort estable, ascendente, multi-key, null y NaN;
- group-by multi-key y cinco agregaciones;
- overflow int8 sin modificar output;
- group-by de 100,000 filas/1,000 grupos (detector práctico contra comportamiento cuadrático);
- joins inner/left/right/full, duplicados, null keys y nombres en colisión;
- unpivot;
- tablas vacías y wrappers heredados.

## Pendiente y coordinación

1. Pivot general con agregación explícita queda pendiente; no existe símbolo parcial.
2. Si otro trabajador añade serialización/CSV de tablas, deberá decidir representación externa de string/categorical/null usando los getters públicos, no inspección de ownership interno.
3. Si se requiere collation Unicode lingüística, debe agregarse como política explícita; el contrato actual usa orden bytewise UTF-8 determinista.
4. Un posible trabajo futuro es coerción explícita de dtypes entre claves de join; actualmente se exige dtype numérico idéntico para evitar conversiones silenciosas.
