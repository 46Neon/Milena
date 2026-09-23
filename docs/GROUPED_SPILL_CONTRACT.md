# Contrato para spill-to-disk de agrupaciones en flujo

**Estado:** contrato general vigente; hay una primera integración vertical parcial en el `.agrupar` tabular canónico, no en `milena_stream_csv_grouped_with_options`. El backend de flujo aún conserva sus grupos en memoria y falla explícitamente al alcanzar sus límites. El adaptador de tablas usa `grouped_aggregate.c`/`spill_store.c` existentes, pero todavía no satisface todos los requisitos de aislamiento seguro de temporales, multi-métrica, clave compuesta ni procesamiento de entrada CSV realmente streaming; no anunciar el spill como capacidad industrial general.

## Límite arquitectónico

La única ruta de producto seguirá siendo `lexer → parser → AST tipado → semántica → runtime → stream.c`. La sintaxis humana expresará las políticas de spill en el AST; el runtime validará los límites antes de ejecutar y pasará opciones tipadas al backend. El spill no tendrá parser, CLI, runtime ni comando externo propios. El manifiesto de fuentes debe clasificar cada módulo nuevo como producto; `Makefile` debe incorporarlo a las fuentes oficiales. El verificador existente debe fallar ante fuentes C presentes pero no clasificadas: no se permite resolver diferencias excluyendo archivos o agregando módulos inexistentes.

## Corte vertical implementado en #agrupar tabular

La sintaxis canónica admite `#spill("ruta-nueva", memoria_bytes, cuota_spill_bytes, max_key_bytes, max_grupos)` dentro de `.agrupar dataset { ... }`. Los límites numéricos son enteros tipados en el AST y se validan antes de ejecutar: memoria 4 KiB–512 MiB, cuota 1 B–4 GiB, clave 2 B–1 MiB, grupos de salida 1–1,000,000; la ruta scratch queda limitada a 220 bytes. La política es opt-in; sin ella sigue el camino histórico de `milena_table_group_by`. Una solicitud explícita que no esté soportada falla y nunca vuelve silenciosamente al backend en memoria.

El adaptador usa una clave STRING (una clave únicamente) y una métrica; `conteo` acepta cualquier columna y las otras cuatro métricas requieren FLOAT64. La clave nula no colisiona con texto vacío; la clave vacía observada sí es válida. Valores métricos nulos conservan grupos y producen null (o conteo cero); la entrada numérica que el cargador clasifica inválida sigue la semántica de null canónica. La salida es una `MilenaTable` materializada con tope de grupos y sale en orden lexicográfico binario; el backend histórico conserva orden de primera aparición. La suma/media con reducer mergeable puede diferir por redondeo del backend anterior; la equivalencia numérica se comprueba con tolerancia, no bit a bit.

Límites aún abiertos frente a este contrato: el input `Dataset` y la tabla canónica previa ya residen en RAM, y la tabla resultado también se materializa; no es aún un adaptador de `stream.c`. La API recibe una ruta final proporcionada por el programa, comprueba que no exista y la elimina al cerrar, pero todavía no asigna nombres aleatorios exclusivos ni asegura permisos privados; escritores concurrentes sobre una ruta no están soportados. El fan-in actual de fusión es fijo de dos vías y no hay límite AST independiente para tiempo/filas/runs. Estos son pendientes explícitos, no afirmaciones de cumplimiento total.

## Semántica y determinismo

- Procesar el CSV con el lector actual, que reconoce registros entrecomillados y saltos de línea internos; no particionar por offsets arbitrarios que puedan cortar registros.
- Al alcanzar el umbral residente, ordenar el lote de grupos por los bytes decodificados de la clave y escribir una corrida temporal tipada. Las métricas válidas/inválidas, el contador, los acumuladores de suma/media/varianza y las estadísticas min/max deben conservar semántica equivalente al camino sin spill.
- Fusionar las corridas con una cola de prioridad acotada, con un orden fijo por clave y ordinal de corrida. El orden reduce flotantes debe ser determinista para los mismos datos, opciones y versión del formato. La prueba de equivalencia compara valores numéricos con tolerancia documentada; no promete identidad bit a bit frente al orden de acumulación de una sola pasada.
- Emitir grupos en orden lexicográfico de bytes, idéntico al contrato actual. No crear un grupo para claves no observadas; la clave vacía sí es válida.

## Formato persistente

Cada corrida tendrá una cabecera explícita con magic, versión, endianess, identificador de ejecución, esquema/operaciones de métricas, número de filas y registros. Cada registro tendrá longitud de clave y valores enteros/floating de ancho fijo con encoding definido, nunca `fwrite` de structs C con padding o ABI dependiente. Bloques y cabecera tendrán checksums para detectar truncamiento/corrupción; checksum no significa autenticidad criptográfica. Rechazar versiones, tamaños, métricas, checksum o datos numéricos inválidos antes de combinar.

## Recursos y fallos

Todos los límites tendrán defaults conservadores, validación y topes duros: memoria residente, tamaño máximo de clave/registro, número de corridas/archivos abiertos, bytes temporales totales, filas/tiempo y grupos totales. Cualquier suma/multiplicación de tamaños debe comprobar overflow. La falta de espacio, límite excedido, error de lectura/escritura, cancelación o corrupción devuelve error explícito; nunca cae silenciosamente a agregación ilimitada en RAM.

Crear temporales exclusivos con nombres impredecibles y permisos privados en un directorio permitido; registrar cada recurso para limpieza en cualquier camino de salida. No truncar ni reemplazar el destino final hasta completar la fusión y validar el reporte. Publicar mediante archivo temporal de salida y renombrado atómico cuando el sistema de archivos lo permita. Ningún error debe dejar corridas o un reporte parcial como resultado válido.

## Puerta de aceptación

1. Pruebas unitarias del formato: round-trip, versión no admitida, checksum corrupto, truncamiento, longitudes desbordadas y limpieza.
2. Pruebas del backend con spill forzado por umbral pequeño: una corrida y varias; claves duplicadas entre corridas; claves vacías; valores inválidos; todas las operaciones admitidas; límite de bytes/runs y fallos de E/S. Verificar que los temporales se eliminan en éxito y error.
3. E2E desde `.analisis` que compare el JSON de spill con el camino en memoria para orden, recuentos y valores dentro de tolerancia; demostrar que el runtime entrega la configuración AST tipada.
4. Guardas de arquitectura, manifiesto y Makefile; sanitizers; suite completa y CI del head final. Documentar limits/defaults reales y benchmark reproducible con medición de pico RAM/bytes temporales.
5. Mantener explícito que este contrato es local: no introduce red, clúster, cloud, Spark, Flink, Arrow ni Parquet.

Hasta superar toda esta puerta, la afirmación correcta sigue siendo: «la agrupación en flujo tiene estado acotado en memoria y falla al exceder sus límites; spill-to-disk está pendiente».
