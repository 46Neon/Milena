# Hoja de ruta de escala de Milena — PR #29

## Propósito y frontera arquitectónica

El objetivo de esta hoja de ruta es llevar el motor unificado de Milena a cargas
crecientes sin convertir Big Data en una herramienta interna independiente. Toda
capacidad debe recorrer el mismo contrato: **lexer → parser → AST →
semántica/recursos → runtime → backend**. Los backends son implementaciones
internas seleccionadas por el AST/plan tipado; no pueden introducir otro
lenguaje de scripts, parser, CLI de datos ni runtime paralelo.

PR #29 valida tres ejecuciones del lenguaje sobre un CSV determinista de
exactamente 1.000.000 de filas: resumen global, agrupación streaming de dos
claves y agrupación streaming con spill de 128 claves y tres métricas (suma,
media y contar), todas por `milena run` y `.analisis`. Se comprueban resultados exactos, errores de datos, conteos,
límites declarados y limpieza; la corrida spill se repite para verificar
salida semántica determinista. Se observa tiempo, bytes, buffers y RSS pico
cuando el sistema lo permite. El resultado prueba únicamente estas operaciones,
entrada, build y hardware: no prueba cardinalidad arbitraria, joins, ETL general,
Arrow/Parquet, nube, ejecución distribuida ni ML, y no establece latencia
universal ni una cota de RSS total.

## Relación honesta con PR #28

PR #29 está basado en el snapshot fijado `pr29-base/pr28-46b2366`
(commit `46b2366a09208bc1b2bf8f5f8426b4bfbeef1f32`), no en una punta móvil
ni en cambios nuevos de PR #28. El planner tipado de PR #29 reconoce la configuración AST de spill y el runtime
canónico delega a `milena_stream_csv_grouped_spill_with_options` del backend
existente, reutilizando su lector CSV, reducer versionado, ordenamiento/fusión externos,
cuotas y publicación transaccional; no hay copia paralela de almacenamiento o
reducer. La identidad de métrica se añade a la clave opaca del reducer único.
La política de AST incluye memoria, scratch, clave codificada, grupos, bytes del
reporte y máximo de runs. El adaptador tabular existente también se conserva.

PR #28 sigue abierto y no forma parte de `main`; PR #29 conserva como base el
snapshot fijado indicado arriba. La CI de PR #29 valida este corte sin cambiar
el head de PR #28. Los resultados del millón de filas no validan escala
arbitraria, plataformas no medidas ni cualquier otra operación de PR #28.

## Fases y criterios de aceptación

Cada fase conserva la ruta canónica y exige pruebas de equivalencia con la
semántica local, límites explícitos de recursos y errores deterministas. Una
fase no se considera terminada por compilar solamente.

### Fase 0 — Contratos y verificación de escala (este PR, parcial)

- Mantener una sola ruta fuente de verdad: lexer → parser → AST → semántica y
  validación de recursos → runtime → backend interno.
- Generar una entrada determinista de 1.000.000 de filas y ejecutarla mediante
  `milena run` y el lenguaje español existente; validar exactamente filas,
  filas válidas/malformadas, conteos de métrica y suma/media esperadas.
- Comprobar que el informe acredita lectura de bytes y que el búfer de registro
  observado está limitado y es menor que el archivo completo. Registrar
  throughput, tiempo de proceso, bytes, capacidad/pico del búfer y RSS máximo
  portable cuando esté disponible; no imponer una cifra de rendimiento/RSS
  universal.
- Añadir un target reproducible de Makefile y un workflow de escala separado,
  manual y programado, fuera de la CI ordinaria.
- Esta fase prueba una operación de agregación global, no una arquitectura
  general de un millón de filas. También ejecuta el mismo programa con el límite
  de filas un registro por debajo del fixture y exige un error sin reporte
  parcial de éxito.

### Fase 1 — Plan lógico y físico unificado

**Estado de PR #29:** se implementa un corte vertical limitado al streaming CSV
actual: `src/query_plan.c` deriva, después de semántica/recursos, una secuencia
lógica CSV-scan → aggregate global/agrupado → JSON-report y elige el backend
físico global o agrupado que consume el runtime canónico. El AST sigue siendo la
fuente de verdad para métricas, fuente y límites. Hay tests unitarios de plan
válido, ambiguo/no soportado y compatibilidad global legacy. Esto no es todavía
un planner general de operadores, esquema, costos, filtros o formatos.

- Modelar lectura, proyección, filtros, agregaciones y límites como nodos
  tipados del AST/plan canónico; resolver nombres y tipos en semántica antes de
  ejecutar.
- Separar explícitamente plan lógico de plan físico sin crear un parser,
  biblioteca de scripts o motor de consulta alterno.
- El plan físico debe elegir entre ejecución secuencial, streaming y operadores
  acotados según capacidades y presupuestos declarados; rechazar planes que no
  tengan soporte, sin fallback oculto.
- Aceptación: pruebas end-to-end vía `milena run` que inspeccionen/validen
  operadores elegidos, cobertura de tipos, errores de recursos y equivalencia
  exacta o tolerancia numérica documentada respecto del camino de referencia.

### Fase 2 — CSV streaming y spill integrados al plan

- Mantener el lector CSV incremental con soporte correcto de comillas y
  registros multilínea; llevar presupuestos de filas, tiempo, columnas,
  registro, memoria y scratch desde sintaxis/AST hasta el backend.
- Implementado en esta actualización: el plan físico canónico de `.analisis`
  conecta la agrupación CSV con el API de spill existente; usa una clave texto y
  hasta 64 métricas suma/media/mínimo/máximo/contar declaradas y tipadas, con
  identidad de métrica incluida en la clave opaca del mismo reductor. Respeta
  memoria, scratch, longitud de clave codificada, cardinalidad de grupos de
  salida, bytes del reporte y máximo de runs; no crea un reductor por métrica
  ni materializa filas de entrada. No duplica almacén, serialización, estados
  mergeables, sort ni reducer.
- Sigue pendiente ampliar fuentes/operadores y los contratos de key/métricas;
  mantener límites de lectura y fallos explícitos en cada backend.
- Aceptación continua: agregación global/agrupada y ejecución por lotes con
  resultados
  deterministas, cleanup transaccional y fallos por cuota explícitos. No
  materializar el input o todos los grupos para afirmar streaming.
- Aceptación: pruebas `milena run` de equivalencia con y sin spill para suma,
  media y contar, nulos/malformados por métrica, claves y tipos soportados,
  límites de memoria/scratch/reporte/grupos/runs, temporales limpios y un
  fixture de un millón de filas con tres métricas;
  benchmarks reproducibles reportan RSS/I/O/throughput sin prometer SLO no
  medidos.

### Fase 3 — Fuentes y formatos columnares

- Definir una abstracción de fuente interna que conserve la misma semántica y
  AST para rutas locales y fuentes remotas/nube; resolver autenticación mediante
  configuración segura, no literales de secretos en scripts/reportes.
- Añadir Arrow/Parquet de forma incremental: tipos y nulos explícitos, schema
  evolution controlada, row groups, compresión, selección de columnas y
  pushdown solo cuando haya equivalencia demostrada.
- Aceptación: fixtures multi-row-group, nulos, tipos, schema mismatch,
  compresión, pushdown equivalente, límites de memoria y casos de error. Medir
  localmente cada backend y documentar formatos/características realmente
  soportados; no presentar nube como disponible antes de su implementación.

### Fase 4 — Operadores ETL en la semántica del lenguaje

- Incorporar lectura/proyección/filtro/cast/normalización/deduplicación,
  agregación y escritura como nodos tipados del mismo AST/runtime. Definir
  claramente si cada operador es streaming, requiere estado acotado o necesita
  materialización/spill.
- Definir políticas explícitas de nulos, conversiones, errores, orden,
  determinismo y escritura atómica; presupuestar cardinalidad y estado.
- Aceptación: programas `.milena` end-to-end, pruebas de bordes y errores,
  equivalencia contra fixtures de referencia, pruebas de memoria/cuotas y
  benchmarks que identifiquen los operadores medidos.

### Fase 5 — Paralelismo local y evaluación distribuida

- Reutilizar planner, particionado, presupuesto, reducción/protocolo y spill
  existentes solo tras confirmar su estado en `main`; integrar operadores CSV
  conscientes de límites de registros, comillas, skew y determinismo.
- Diseñar interfaces de backend remotos sin cambiar la sintaxis, AST ni
  semántica de usuario. Evaluar Spark y Flink como posibles backends/adaptadores
  de ejecución, con decisión documentada basada en interoperabilidad, operación,
  costos, semántica, licencias y benchmarks; la evaluación no equivale a soporte.
- Aceptación local: igualdad determinista secuencial/paralelo, cancelación,
  presupuesto, cleanup, fallo/reintento probado y medición reproducible.
  Aceptación remota: protocolo versionado, autenticación, coordinación,
  reintentos/checkpoints y pruebas reales multiworker antes de llamarlo
  distribuido; establecer SLO solo con medidas p50/p95/p99 bajo carga definida.

### Fase 6 — Integración de ML

- Exponer preparación de features, particionado train/test, entrenamiento,
  evaluación e inferencia como capacidades tipadas del mismo lenguaje/runtime,
  integradas a los planes y fuentes anteriores; no como un `milena-ml` o
  pipeline separado.
- Especificar reproducibilidad/semillas, esquemas, tratamiento de nulos,
  límites de memoria, serialización/versionado de modelos y compatibilidad entre
  entrenamiento e inferencia.
- Aceptación: flujos `.milena` end-to-end, datasets de prueba deterministas,
  métrica y tolerancias documentadas, serialización/relectura de modelos,
  validación de presupuesto y benchmark comparativo. La fase no debe prometer
  modelos o escala no implementados y medidos.

## Estado de implementación de PR #29

- Implementados: planner lógico/físico tipado para resumen global, agrupación
  regular y agrupación con spill; validación end-to-end de un millón de filas
  para cada uno de esos tres workloads, con resultados por grupo y rechazo de
  límites de filas/grupos sin publicar reportes parciales.
- El caso A/B conserva la semántica de `contar` sobre celdas no vacías aunque
  otra métrica numérica las rechace; el caso spill verifica suma, media y contar,
  128 grupos acotados, datos numéricos inválidos, salida ordenada, cuotas AST y
  limpieza/repetición determinista. Esto no prueba cardinalidad arbitraria ni
  RSS global acotada.
- Las demás capacidades descritas en fases 2–6 —formatos/fuentes, ETL general,
  paralelismo, cloud, Arrow/Parquet, distribuido y ML— siguen pendientes y no
  se marcan completas por estar planificadas.

## Ejecución reproducible del hito de este PR

```sh
make clean all
make scale-million-row
# Alternativa con artefacto JSON:
python3 benchmarks/million_row_validation.py --output million-row-results.json
```

El target genera temporalmente el CSV; no deja una fixture masiva en el
repositorio. `make scale-million-row` no forma parte de `make test` y se ejecuta
en el workflow separado `Million-row scale validation` en PRs que cambien los
componentes de escala, manualmente o en su calendario semanal. El workflow
adjunta el JSON del resultado cuando el job termina correctamente. El proceso falla si no se validan los conteos y valores
esperados o si el informe no acredita el búfer acotado.


## Spill workload resource contract

The spill program declares its `grupo_spill` and `importe` columns in the AST. The opt-in benchmark uses 128 keys, 4 KiB reducer memory, a 128 MiB
scratch quota, 128-byte key cap, 16 MiB output cap, 4,096-run cap, one-million
row cap and 1 MiB record cap. It validates group sums and malformed rows,
sorted deterministic keys, cleanup and repeated semantic output. The small
end-to-end spill suite in `make test` covers row, time, scratch, group, report
bytes, run-count, unsupported type and multi-metric failures without partial
reports or stale files. These limits bound configured operator state/output,
not process-wide RSS. Timings remain observations without portable thresholds;
cloud/Arrow/Parquet/ETL general, distributed execution and ML remain future work.


## Reducer spill telemetry

The canonical grouped-spill JSON report now exposes `bytes_spill`, `registros_spill`, and `runs_spill`. The first two are actual append-store bytes (including framing) and records; the third is the initial sorted-run count written by the reducer, not a configured maximum or estimate. The values are snapshotted from reducer/store state before close and are only published with a fully successful staged report. The 1,000,000-row, 4 KiB-budget validation requires positive source-spill bytes and records (and an actual sort run), proving that this fixture exercised the reducer spill path. These counters and timing are workload observations, not performance thresholds or general scale guarantees. Arrow/Parquet, cloud, general ETL, network/distributed execution, and ML remain outside the implemented scope.


## Filtro CSV canónico (slice incremental)

El flujo canónico de `milena run` admite una sola condición tipada en `.analisis` con fuente en modo streaming: igualdad textual `filtrar "columna" == "texto";` (la columna se declara `texto`, igualdad exacta byte a byte sobre el campo CSV ya decodificado) o comparación numérica estricta `filtrar "columna" > 10;` (la columna se declara `numerica`, se acepta solo un literal numérico finito y los valores iguales al límite no pasan). El predicado vive en el AST y plan lógico entre lectura y agregación y se evalúa en el lector CSV común antes de agregado global, agrupación en memoria o spill. En `>`, campos vacíos/nulos, malformados, no finitos o con sufijos no numéricos son no-coincidencias, no errores ni coerciones; por tanto no alimentan el agregado. Las filas descartadas siguen contando en límites de filas/tiempo y métricas de lectura. Una columna no declarada con el tipo requerido produce error semántico; una columna declarada pero ausente en la cabecera produce error de datos, al igual que cabeceras duplicadas. Se admite exactamente un predicado con una de estas dos formas; `<`, `>=`, `<=`, `!=`, expresiones, condiciones múltiples, regex y coerción se rechazan explícitamente. Se reutilizan el lexer, parser, AST, semántica, planner, runtime y backends existentes; no se declara completada la hoja de ruta de fuentes columnares, cloud, ETL general, distribución ni ML.

Esta capacidad no implica ETL general, filtros de expresión arbitraria, Arrow/Parquet, cloud, ejecución distribuida ni ML; son fases futuras de la hoja de ruta.
