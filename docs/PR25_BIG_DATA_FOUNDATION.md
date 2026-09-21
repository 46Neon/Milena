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

## Agrupación acotada (contrato PR25)

La sintaxis exacta es la forma canónica de compatibilidad del lenguaje:

```milena
dataset cargar flujo("datos/ventas.csv", 4096)
.agrupar { #por("grupo") #suma("importe") #media("importe") #conteo("importe") }
.exportar { ("reporte.json") }
```

`#por` identifica la columna de clave y cada métrica nombra una columna.
El runtime conserva estado por grupo y rechaza superar el límite duro de
**100.000 grupos** (el valor predeterminado del flujo es 1.000). El límite es
validado antes de crecer el estado; no se ofrece estado ilimitado. Una fila con
varias métricas puede aportar sus valores válidos de forma independiente: un
valor inválido incrementa `valores_invalidos` solo para esa métrica y la fila
se refleja en el conteo de malformadas. Una columna inexistente, una fila con
número incorrecto de columnas o un CSV inválido rechaza la operación completa.

`python3 benchmarks/grouped_stream_benchmark.py` genera fixtures deterministas
pequeño (100 filas/4 grupos) y mediano (10.000/32), con valores inválidos
repetibles. `--large-rows N` habilita explícitamente el fixture grande; no se
ejecuta en CI. El target bounded `make benchmark-stream-grouped` ejecuta solo
los dos casos pequeños y medianos, con conteo de grupos, valores inválidos,
resultado y tiempo de pared verificables. La metodología mide la ejecución del
programa completo por la ruta lexer → parser → AST → semántica → runtime →
backend de flujo; son observaciones reproducibles, no una promesa de latencia,
throughput o escala industrial.

La agrupación sigue siendo de un proceso y mantiene el estado de grupos en
memoria. Spill-to-disk, formatos columnares (Arrow/Parquet), paralelismo,
particionado y distribución son trabajo futuro explícito.


## Contrato canónico de ejecución (incremento de unificación)

El incremento posterior de PR25 define `MilenaExecutionPlan` y
`MilenaExecutionReport` en el runtime canónico. El plan tipado expresa fuente
(tabla materializada o CSV acotado), agrupación, agregaciones, sink y opciones
(incluidos límites de registro, columnas y grupos). `src/execution_contract.c`
es el único adaptador: traduce el mismo plan a `MilenaTable` o a `stream.c`;
no hay un parser, CLI o runtime Big Data paralelo. La sintaxis española sigue
entrando por lexer → parser → AST → semántica → runtime.

Las pruebas estructurales comprueban que el contrato está en el binario
oficial y que ambos backends existen detrás de él. La ruta materializada
conserva `milena_table_summarize`/`milena_table_group_by`; la ruta de flujo
conserva memoria acotada, el límite duro de grupos y sus contadores de filas
malformadas. La equivalencia de agregados en memoria y flujo se valida por los
contratos y fixtures de backend; las diferencias documentadas son que el CSV
rechaza filas con columnas incorrectas, cuenta números inválidos por métrica y
no garantiza orden de grupos, mientras la tabla preserva tipos nativos,
nullabilidad y orden de primera aparición.

Esto no es Big Data distribuido completo: no incluye spill-to-disk,
particionado, paralelismo, Arrow/Parquet ni un planificador remoto. Proyección y
filtros están reservados explícitamente en el contrato hasta tener semántica y
backend equivalentes.
