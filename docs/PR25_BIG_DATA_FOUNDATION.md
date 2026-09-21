# PR25: base reproducible para Big Data

PR25 no crea un producto de datos separado. Formaliza el contrato interno
`MilenaStreamOptions`/`MilenaStreamReport` y lo ejecuta únicamente mediante
`lexer → parser → AST → semántica → runtime → stream.c`. La sintaxis sigue
siendo humana y española (`datos desde`, `resumir`, `suma de`, `contar de`).

## Contrato medido

El backend reutiliza un registro acotado, la cabecera y acumuladores tipados.
Los límites de registro y columnas se validan antes de reservar memoria. El
reporte conserva filas leídas, válidas y malformadas, bytes de entrada, tiempo
observado, filas/s, MB/s, lote y pico de registro. Una fila con número inválido
es malformada y no se incorpora al acumulador; el recorrido sigue siendo de una
pasada. Las cargas que caben en memoria deben compararse contra la ruta de
Dataset en pruebas posteriores; PR25 no inventa una segunda representación.

## Benchmarks

`python3 benchmarks/stream_benchmark.py` genera CSV deterministas de 100 y
10.000 filas y ejecuta un programa `.milena` canónico. `--large-rows N` es
opt-in para no hacer CI impredecible. Se registran plataforma, arquitectura,
compilador y `GITHUB_SHA`; no se prometen milisegundos, escalabilidad ni
rendimiento industrial.

## Alcance y siguientes fases

Incluido: contrato observable, fixtures reproducibles, métricas, límites de
propiedad/vida del búfer, errores de CSV y target CI `benchmark-stream`.
No incluido: agrupaciones streaming, estado ilimitado, spill-to-disk,
Arrow/Parquet ni paralelismo. Esas fases requieren contratos de partición,
orden, memoria, formatos y equivalencia antes de implementarse; no deben
bypassear el runtime canónico.


## Incremento de contrato de flujo (PR25)

El reporte distingue `pico_registro_bytes` (máximo payload observado, sin NUL) de
`capacidad_buffer_registro_bytes` (capacidad reservada y potencialmente mayor).
La capacidad configurada sigue siendo `limite_registro_bytes`.

Cada métrica se acumula de forma independiente: un valor numérico válido
contribuye a esa métrica aunque otra métrica de la misma fila sea inválida.
`filas_malformadas` cuenta filas con al menos un valor métrico inválido y cada
resultado incluye `valores_invalidos` para hacer visible la validez por métrica.
Las filas con columnas incorrectas siguen siendo descartadas para todas las
métricas. Esta semántica no implica tolerancia silenciosa: los contadores quedan
en el JSON determinista.

El benchmark rechaza `--large-rows` negativo; los casos grandes siguen siendo
opt-in. Los resultados son observaciones acotadas, no una promesa industrial.
La agrupación con estado acotado y spill-to-disk permanece como siguiente
incremento; aún no forma parte de este contrato publicado.
