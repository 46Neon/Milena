# Plan de escalabilidad masiva de Milena

PR #28 inicia la segunda fase después del planner y la ejecución local de PR
#27. El objetivo es que los resultados parciales puedan viajar entre procesos
ó máquinas mediante un contrato versionado, sin crear un parser ni un runtime
paralelo.

## Fases y criterios

1. **Protocolo de resultados** — implementado aquí: mensaje binario versionado,
   checksum, identificador de partición, estado, validez y valor numérico.
2. **Reducción distribuible** — consumir resultados decodificados en orden de
   partición, rechazar duplicados y conservar equivalencia monolítica.
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
