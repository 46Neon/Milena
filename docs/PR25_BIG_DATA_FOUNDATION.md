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
