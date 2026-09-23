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
3. **Spill-to-disk** — serializar estados parciales y limitar espacio temporal.
4. **Formatos masivos** — Parquet/Arrow, row groups, compresión y pushdown.
5. **Planner físico de datos** — hash/range partitioning, joins, skew y costos.
6. **Coordinador** — leases, heartbeats, reintentos, checkpoints y cancelación.
7. **Transporte** — adaptar el mismo protocolo a IPC y red autenticada.
8. **SLO medidos** — benchmarks con p50/p95/p99, memoria, shuffle y fallos.

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

PR #27 deja la base local: planner, particiones, workers, presupuestos,
cancelación, reducción e IPC POSIX. PR #28 comienza la interoperabilidad del
resultado. Todavía no existe procesamiento entre máquinas, Parquet, shuffle,
spill-to-disk ni un SLO de latencia fija. Cada una requiere implementación y
medición propia.

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
