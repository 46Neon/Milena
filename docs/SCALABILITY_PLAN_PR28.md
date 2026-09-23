# Plan de escalabilidad masiva de Milena

PR #28 inicia la segunda fase después del planner y la ejecución local de PR
#27. El objetivo es que los resultados parciales puedan viajar entre procesos
ó máquinas mediante un contrato versionado, sin crear un parser ni un runtime
paralelo.

## Fases y criterios

1. **Protocolo de resultados** — implementado aquí: mensaje binario versionado,
   checksum, identificador de partición, estado, validez y valor numérico.
2. **Reducción distribuible** — implementada en esta actualización: consumir
   resultados decodificados fuera de orden, rechazar duplicados/faltantes y
   conservar equivalencia monolítica.
3. **Spill-to-disk básico** — implementado en esta rama: registros limitados, validación y replay incremental; no equivale a un motor de spill completo.
4. **Agregaciones externas** — implementados estados globales mergeables, ordenamiento externo numérico y un primer agregador `GROUP BY` spillable con límites de memoria/scratch y salida determinista. En PR #28 el operador tiene cortes canónicos AST/semántica/runtime para tabla materializada y CSV streaming. El estado de agregado usa wire v4 little-endian de 96 bytes, con contadores separados de valores válidos/nulos/inválidos y suma `INT64` exacta; el CSV spill representa `COUNT` como entero JSON exacto. La fusión externa usa fan-in fijo de dos y sus límites no equivalen a una cota de RSS global. El contrato de corrida todavía no está ligado a un esquema completo y sigue limitado a una clave de texto y una métrica por operación spill.
5. **Formatos masivos** — Parquet/Arrow, row groups, compresión y pushdown.
6. **Planner físico de datos** — hash/range partitioning, joins, skew y costos.
7. **Coordinador** — leases, heartbeats, reintentos, checkpoints y cancelación.
8. **Transporte** — adaptar el mismo protocolo a IPC y red autenticada.
9. **SLO medidos** — el benchmark determinista incluye una validación de 1.000.000 de filas y 1.000 grupos con reducer configurado a 262.144 bytes. En una ejecución Linux x86_64 se observaron aproximadamente 0,460 s en memoria y 8,487 s con spill; es una única repetición, no un SLO. RSS, scratch máximo observado, p50/p95/p99, otras plataformas y pruebas de alta cardinalidad siguen pendientes de instrumentación.

Una fase no se considera terminada solo porque compile: necesita contrato,
prueba de integración, caso de error y comparación con el resultado local.

## Contrato del protocolo actual

El mensaje tiene tamaño fijo, versión y checksum FNV-1a. El decoder rechaza:

- longitud incorrecta;
- magic o versión desconocidos;
- checksum incorrecto;
- estados inválidos;
- identificadores que no caben en la plataforma;
- valores no finitos cuando están marcados como válidos.

El protocolo transporta resultados parciales, no memoria arbitraria ni texto
Milena. Esto permite que el transporte futuro sea IPC, TCP u otro canal sin
cambiar el frontend del lenguaje.

## Alcance honesto

PR #27 deja la base local: plan físico genérico por rangos contiguos de bytes,
particiones, workers, presupuestos, cancelación, reducción e IPC POSIX. PR #28
añade interoperabilidad del resultado, almacén spill append-only con replay
validado, runs externos numéricos y agrupación incremental con presupuesto
explícito, integrada a la ruta AST/semántica/runtime tanto para tablas como para
CSV streaming. No existe procesamiento entre máquinas, Parquet/Arrow, shuffle
distribuido, ni SLO medido de latencia o RSS global. El plan de rangos por bytes
no conoce fronteras de registros CSV y no está conectado a group/sort pushdown;
conectarlo al lector CSV sin un plan que preserve registros sería incorrecto.

## Reducción implementada

`partition_protocol_reduce.c` recibe mensajes de tamaño fijo en cualquier orden,
los coloca por identificador de partición y delega la combinación numérica al
reductor determinista del runtime. Rechaza particiones desconocidas, mensajes
duplicados, resultados faltantes y estados de worker distintos de `MILENA_OK`.
La prueba compara el resultado desordenado contra la suma local y cubre un
duplicado.


## Canonicalización de transporte

La versión 2 del protocolo serializa todos los enteros y el patrón IEEE-754 del
`double` explícitamente en little-endian. El checksum de 32 bits también se
serializa byte a byte. Esto evita depender del endianness o del layout nativo de
la máquina y deja el contrato preparado para workers heterogéneos. El cambio de
representación incrementa la versión del protocolo y obliga a rechazar mensajes
de versiones anteriores en lugar de interpretarlos ambiguamente.

## Spill-to-disk y recuperación

Esta fase añade un almacén append-only con registros autocontenidos: magic,
versión, longitud canónica y checksum FNV-1a. Tiene cuota total y tamaño máximo
por registro, rechaza escrituras que excedan el presupuesto, valida el prefijo
completo al abrir y recupera automáticamente un tail incompleto o corrupto
copiando el prefijo verificado a un temporal único y reemplazando el nombre sin
borrar antes el original (rename en POSIX; reemplazo del sistema en Windows).
Si el reemplazo falla, el original queda en su lugar y se devuelve error. Los
registros completos que exceden la cuota o el límite por registro se rechazan
con overflow y no se truncan, para evitar pérdida silenciosa al reabrir con
límites menores. La recuperación expone cuántos registros y bytes conserva y
cuánto descarta; no confunde recuperación parcial con datos válidos. El acceso
concurrente de varios escritores/recuperadores al mismo archivo no está
soportado. `fflush` confirma la descarga a la capa C, no persistencia ante corte
de energía; no se promete crash durability. Esto prepara ordenamiento externo,
agregaciones con spill y checkpoints, no tolerancia a fallos de clúster.

## Replay incremental

El almacén ahora ofrece recorrido validado en orden de escritura mediante un
callback, leyendo un registro por vez con memoria acotada por el tamaño máximo
configurado. Esto permite construir reducción de runs, merge sort externo y
replay de agregados sin materializar el spill completo en RAM. Los tests de
spill forman parte de `make test` y cubren recorrido, cuota, truncamiento y
corrupción de payload/checksum.
## Lector secuencial y merge sort externo acotado

Se añadió un lector incremental de spill que valida cabeceras, tamaño, cuota y
checksum por cada registro sin cargar el archivo entero. Sobre él, `external_merge`
fusiona runs ya ordenados de valores binary64 usando un heap de una cabeza por
run, preserva el orden por run para empates y rechaza runs mal ordenados,
valores no finitos, fan-in inválido, cuotas excedidas y salidas existentes.
La salida se construye en un temporal y solo se activa al completar la fusión;
la RAM queda acotada por el fan-in. La etapa de ordenamiento crea runs desde un callback de fuente con buffer
limitado por `run_capacity` y planifica pasadas hasta el fan-in final. Aplica
cuotas por archivo y una cuota agregada de scratch durante la creación, las
pasadas y la salida final. Aún no es un sort de tablas con claves o estabilidad
configurable.

## Estados de agregación mergeables

Esta fase incorpora un estado numérico mergeable con conteo, suma compensada
(Neumaier), media, M2 de Welford/Chan, mínimo y máximo. Puede actualizarse por
filas, combinarse en orden determinista entre particiones y serializarse con
versión, IEEE-754 binary64 canónico, endianness little-endian y checksum. La
versión 4 conserva la corrección de la suma FLOAT64 y añade tipo explícito más
suma INT64 exacta, junto con contadores de observaciones válidas, nulas e
inválidas, en cada spill/run para que las fusiones externas no pierdan bits
bajos ni la procedencia de valores faltantes. SUM INT64 rechaza mezcla de tipos
y overflow; pruebas incluyen valores mayores que 2^53, extremos con overflow,
serialización/merge y el adaptador de tabla canónico. El estado ocupa 96 bytes;
los campos se codifican explícitamente en little-endian
y el decoder rechaza overflow de contadores, versión desconocida o checksum
incorrecto. La compatibilidad con wire v2 no está soportada: se rechaza en vez
de reinterpretar estados antiguos. La equivalencia con el camino sin spill se
prueba con la tolerancia documentada. El finalizador expone conteo
exacto, suma compensada, media, varianza/desviación poblacional y muestral,
mínimo y máximo; para una muestra vacía o un único valor señala cuándo la
varianza muestral no está definida. Los estados parciales se almacenan como
registros del spill y se reproducen de uno en uno sin materializar los runs.

El estado global anterior puede combinar workers para un único agregado; no
resuelve por sí mismo una cardinalidad ilimitada de claves.

## Primer agregado agrupado con spill

`grouped_aggregate.c` implementa una API C spillable de producto para claves
opacas y estados mergeables, con mapa residente bajo presupuesto, cuota de spill,
runs externos ordenados y merge de dos vías. Sobre esa API hay ahora dos cortes
canónicos en el PR #28: `.agrupar dataset { ... #spill(...) ... }` y agrupación
de CSV streaming vía `stream.c`, ambos con AST tipado, validación semántica y
runtime común. Sin `#spill` se conserva la agrupación histórica en memoria.

La ruta tabular limita spill a una clave STRING y una métrica (conteo de
cualquier columna; SUM acepta FLOAT64/INT64 con acumulación exacta y detección
de overflow para INT64; otras métricas FLOAT64) y materializa entrada y resultado.
La ruta CSV streaming consume registros completos por el lector existente, sin
crear Dataset/MilenaTable, y emite el JSON por callback a un staging file;
también restringe la operación a una clave textual y una métrica. Nulos y
valores métricos inválidos conservan la clave/grupo y no contribuyen al valor;
la salida se ordena lexicográficamente y la equivalencia flotante se valida con
tolerancia. Los tipos/métricas no soportados y multi-métrica con spill se
rechazan; no hay fallback silencioso a RAM.

Las políticas AST fijan memoria del reducer, cuota de spill, tamaño de clave,
grupos, filas, tiempo y máximo de runs ordenadas (default 4096; máximo 65,536).
El merge es de dos vías y abre como máximo tres runs a la vez. Los runs externos
usan hasta dos cuotas adicionales sobre el spill de entrada. Las rutas scratch
se crean exclusivamente para evitar sobrescritura, pero las proporciona el
programa; no hay aún nombres privados aleatorios ni writers concurrentes sobre
la misma ruta. Los presupuestos excluyen el RSS total del proceso, buffers de
stdio/libc, staging y, en el corte tabular, las tablas materializadas.

Pendiente: el scratch nombrado por el programa ya se crea exclusivamente y se
limpia en los caminos validados, pero todavía no ofrece nombres privados
impredecibles ni coordinación de writers concurrentes. El plan físico disponible
solo describe rangos por bytes; para hacer pushdown de group/sort falta un plan
semántico conectado al lector/runtime que preserve los límites de registros y
el orden requerido. La auditoría del join canónico actual (`milena_table_join`)
confirma que materializa índices, vectores de pares y salida sin un presupuesto
AST configurable; acotarlo exige un contrato tipado de filas/memoria/scratch y
una estrategia de salida que preserve la semántica de join, no solo un cap
externo en el backend. El benchmark reproducible de spill añadido valida
resultados y tiempos de pared y corre como smoke de `make test`; aún faltan
mediciones instrumentadas de RSS/scratch y campañas repetidas para definir SLOs.
Arrow/Parquet y una capa coordinador/workers remotos siguen como fases futuras,
fuera de la evidencia implementada. Estos cortes locales no constituyen un
clúster distribuido ni validación de rendimiento industrial o de Termux/aarch64.

