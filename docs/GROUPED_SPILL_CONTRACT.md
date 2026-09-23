# Contrato para spill-to-disk de agrupaciones en flujo

**Estado:** diseño acordado; no implementado. El backend actual `milena_stream_csv_grouped_with_options` guarda grupos en memoria y falla explícitamente al alcanzar `max_groups` o el presupuesto de estado. No existen `src/spill.c` ni `src/spill_store.c`. Este documento no anuncia capacidad disponible.

## Límite arquitectónico

La única ruta de producto seguirá siendo `lexer → parser → AST tipado → semántica → runtime → stream.c`. La sintaxis humana expresará las políticas de spill en el AST; el runtime validará los límites antes de ejecutar y pasará opciones tipadas al backend. El spill no tendrá parser, CLI, runtime ni comando externo propios. El manifiesto de fuentes debe clasificar cada módulo nuevo como producto; `Makefile` debe incorporarlo a las fuentes oficiales. El verificador existente debe fallar ante fuentes C presentes pero no clasificadas: no se permite resolver diferencias excluyendo archivos o agregando módulos inexistentes.

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
