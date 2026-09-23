# Spill local de agrupaciones en flujo

**Estado:** primera ejecución integrada y disponible mediante la API tipada C de `stream.c`; la sintaxis AST/runtime aún no expone política de spill. El flujo sigue siendo `lexer → parser → AST tipado → semántica → runtime → stream.c`; esta primera entrega solo amplía el backend agrupado y no incorpora parser, CLI, runtime ni comando alternativo.

## Comportamiento implementado

`MilenaStreamOptions.spill_enabled` habilita spill únicamente en `milena_stream_csv_grouped_with_options`. Se conserva `false` por omisión, así que el contrato anterior de fallar al superar `max_groups` no cambia. Al encontrar una clave nueva con el lote residente lleno, el backend ordena ese lote por clave decodificada, lo serializa en una corrida local `tmpfile()`, libera claves/acumuladores/tabla hash, y continúa leyendo el CSV secuencialmente. Al finalizar, escribe el lote restante y fusiona las corridas ordenadas, combinando claves duplicadas entre corridas. El merge mantiene un cursor acotado por archivo y escanea los cursores para elegir la siguiente clave; emite el mismo orden bytewise determinista que la ruta en memoria.

Cada corrida tiene magic, versión y número de métricas en una cabecera validada con FNV-1a de 32 bits. Los registros codifican longitud y clave, seis doubles, recuento válido e inválidos por métrica, con endianess little-endian explícita; cada registro lleva FNV-1a de 64 bits. El checksum es detección de daños, no autenticación. Se comprueba todo el contenido y los checksums en una primera pasada antes de abrir/sobrescribir el destino; la segunda pasada emite el JSON. Las reducciones combinan sumas compensadas y estadísticas Welford por corrida en orden fijo; diferencias flotantes pequeñas frente a una sola pasada son posibles.

La activación y límites son campos tipados de `MilenaStreamOptions`, no texto ni opciones parseadas del AST en esta entrega:

- `spill_enabled`: false por defecto.
- `max_spill_bytes`: 64 MiB por defecto, incluye cabeceras y checksums; tope duro 1 GiB.
- `max_spill_records`: 1.000.000 registros temporales por defecto; tope duro 10.000.000.
- `max_spill_files`: 64 corridas abiertas por defecto; tope duro 256.
- Un valor cero en cada límite selecciona su default. Las opciones solo habilitan spill cuando `spill_enabled` es true.

`MilenaStreamReport` devuelve `spilled`, `spill_runs`, `spill_bytes` y `spill_records`; el reporte JSON incluye `spill`, `corridas_spill` y `bytes_spill`. Si cualquier tope se excede o falla la lectura/escritura/checksum, la llamada devuelve error; no cambia silenciosamente a memoria sin límite. Los temporales provienen de `tmpfile()` y se cierran en todas las rutas de salida para que la biblioteca elimine sus recursos locales.

## Límites actuales y próximos pasos

La prueba fuerza spill con un conjunto pequeño, genera varias corridas con claves duplicadas entre ellas, compara agregados/recuentos contra el camino sin spill, comprueba la marca de reporte y valida un fallo cerrado por bytes temporales. La API pública permite activar spill desde callers C que formen opciones tipadas. El AST/runtime no expone aún estos límites, y el formato actual no es una interfaz persistente entre ejecuciones: se usa únicamente dentro de la misma invocación.

Esta fase no implementa una cola de prioridad (la selección de cursor es lineal en cantidad de corridas), publicación atómica del reporte ante fallas físicas durante la segunda pasada, recuperación tras reinicio, ni autenticación de datos temporales. No agrega red, clúster, cloud, Spark/Flink, Arrow/Parquet, ni procesamiento distribuido. El guard de arquitectura y `make check-grouped-spill` mantienen visible el límite de esta integración.
