# Contrato para spill-to-disk de agrupaciones en flujo

**Estado:** hay dos cortes locales distintos. El `.agrupar` tabular canónico conserva su adaptador de tabla existente y sigue materializando la entrada/salida en RAM. La agrupación de CSV en el runtime streaming ahora tiene un primer corte de spill directo `stream.c → grouped_aggregate.c`: no crea `Dataset`/`MilenaTable`, reutiliza el lector CSV existente y emite el JSON desde callback ordenado a un staging file antes de publicarlo. En ambos casos se admite una sola clave de texto y una métrica por operación spill; no es una capacidad industrial general ni una ruta distribuida.

## Límite arquitectónico

La única ruta de producto seguirá siendo `lexer → parser → AST tipado → semántica → runtime → stream.c`. La sintaxis humana expresará las políticas de spill en el AST; el runtime validará los límites antes de ejecutar y pasará opciones tipadas al backend. El spill no tendrá parser, CLI, runtime ni comando externo propios. El manifiesto de fuentes debe clasificar cada módulo nuevo como producto; `Makefile` debe incorporarlo a las fuentes oficiales. El verificador existente debe fallar ante fuentes C presentes pero no clasificadas: no se permite resolver diferencias excluyendo archivos o agregando módulos inexistentes.

## Corte vertical implementado en #agrupar tabular

La sintaxis canónica admite `#spill("ruta-nueva", memoria_bytes, cuota_spill_bytes, max_key_bytes, max_grupos[, max_salida_bytes])` dentro de `.agrupar dataset { ... }`. Los límites numéricos son enteros tipados en el AST y se validan antes de ejecutar: memoria 4 KiB–512 MiB, cuota 1 B–4 GiB, clave 2 B–1 MiB, grupos de salida 1–1,000,000 y reporte 1 B–1 GiB (1 GiB por defecto); la ruta scratch queda limitada a 220 bytes. La política es opt-in; sin ella sigue el camino histórico de `milena_table_group_by`. Una solicitud explícita que no esté soportada falla y nunca vuelve silenciosamente al backend en memoria.

El adaptador usa una clave STRING (una clave únicamente) y una métrica; `conteo` acepta cualquier columna y las otras cuatro métricas requieren FLOAT64. La clave nula no colisiona con texto vacío; la clave vacía observada sí es válida. Valores métricos nulos conservan grupos y producen null (o conteo cero); la entrada numérica que el cargador clasifica inválida sigue la semántica de null canónica. La salida es una `MilenaTable` materializada con tope de grupos y sale en orden lexicográfico binario; el backend histórico conserva orden de primera aparición. La suma/media con reducer mergeable puede diferir por redondeo del backend anterior; la equivalencia numérica se comprueba con tolerancia, no bit a bit.

Límites todavía abiertos del adaptador tabular: su input `Dataset` y la tabla canónica previa ya residen en RAM, y la tabla resultado también se materializa. Para ambas rutas, la API recibe una ruta scratch proporcionada por el programa y no garantiza nombres aleatorios privados ni seguridad entre writers concurrentes. El fan-in del reducer actual es fijo de dos vías; la política de flujo sí tiene límites AST explícitos de filas/tiempo, mientras que el corte no tiene presupuesto AST independiente de runs. Estos pendientes no invalidan la ruta de CSV streaming ni son afirmaciones de cumplimiento industrial.


## Primer corte de spill directo del CSV streaming

La sintaxis humana sigue el bloque `agrupar por ... resumir { ... }` del modo streaming, y añade la política AST ya tipada:

```milena
.analisis ejemplo {
  variable grupo texto
  variable importe numerica
  datos desde "entrada.csv" con filas hasta 10000000 con tiempo hasta 300000 ms
  agrupar por "grupo" #spill("scratch.bin", 262144, 1073741824, 4096, 100000, 104857600)
    resumir { suma de "importe"; }
  guardar resultado en "reporte.json"
}
```

`#spill` contiene memoria del reductor, cuota de bytes del spill de entrada,
bytes máximos de clave codificada, máximo de grupos y un límite de bytes para el
reporte final (opcional; 1 GiB por defecto). Los límites de filas y
tiempo deben ser explícitos en `datos desde`; el registro CSV/cantidad de
columnas conservan sus límites existentes. La política se valida en el AST y
la semántica rechaza tipos de clave/métrica incompatibles, métricas no
admitidas y más de una métrica antes del runtime.

El lector usa `stream_read_record` y `stream_split` compartidos, respetando
comillas, comas y saltos de línea incrustados. Cada fila va directamente al
reductor; ninguna fila se agrega a Dataset/Table. Al finalizar, el reducer
invoca un callback por grupo de salida ya ordenado, que precomputa el tamaño
JSON del grupo y verifica la cuota restante antes de escribirlo al archivo
staging hermano. La publicación sustituye el destino atómicamente en POSIX y
con `MoveFileEx` en Windows, solo cuando lectura, límites, reducción, escritura
y cierre finalizaron bien. En error se elimina staging, runs y scratch. La
prueba E2E compara el valor del corte spill con el agrupamiento de referencia
en memoria mediante tolerancia.

El contrato de memoria distingue el búfer de registro CSV (capacidad limitada
por `max_record_bytes`), la copia acotada de cabecera, `max_columns` punteros,
el búfer stdio de 64 KiB, un buffer callback de hasta una clave máxima y el
presupuesto separado del reductor (mapa + sorting/merge). Los límites de caller
y libc/stdio no equivalen a una cota de RSS total. En disco, el archivo de
entrada append-only se limita por la cuota configurada y los runs ordenados
pueden consumir hasta dos cuotas adicionales (hasta 3× la cuota en total),
aparte del reporte staging. El reporte cuenta con una cuota independiente de
bytes configurada en AST; no se mezcla con la cuota de scratch.

No se agregan varias claves/métricas spill, clave compuesta, unión, ordenamiento
de filas, Parquet/Arrow ni workers/red. Operaciones `Dataset` y `MilenaTable`
siguen en memoria. Los nombres scratch son dados por el programa y la ruta debe
ser nueva; esta fase no promete nombres aleatorios privados ni seguridad para
writers concurrentes. No se ha medido RSS global ni se promete latencia.

## Semántica y determinismo

- Procesar el CSV con el lector actual, que reconoce registros entrecomillados y saltos de línea internos; no particionar por offsets arbitrarios que puedan cortar registros.
- Al alcanzar el umbral residente, ordenar el lote de grupos por los bytes decodificados de la clave y escribir una corrida temporal tipada. Las métricas válidas/inválidas, el contador, los acumuladores de suma/media/varianza y las estadísticas min/max deben conservar semántica equivalente al camino sin spill.
- Fusionar las corridas con una cola de prioridad acotada, con un orden fijo por clave y ordinal de corrida. El orden reduce flotantes debe ser determinista para los mismos datos, opciones y versión del formato. La prueba de equivalencia compara valores numéricos con tolerancia documentada; no promete identidad bit a bit frente al orden de acumulación de una sola pasada.
- Emitir grupos en orden lexicográfico de bytes, idéntico al contrato actual. No crear un grupo para claves no observadas; la clave vacía sí es válida.

## Formato persistente

Cada corrida tendrá una cabecera explícita con magic, versión, endianess, identificador de ejecución, esquema/operaciones de métricas, número de filas y registros. Cada registro tendrá longitud de clave y valores enteros/floating de ancho fijo con encoding definido, nunca `fwrite` de structs C con padding o ABI dependiente. Bloques y cabecera tendrán checksums para detectar truncamiento/corrupción; checksum no significa autenticidad criptográfica. Rechazar versiones, tamaños, métricas, checksum o datos numéricos inválidos antes de combinar.

## Recursos y fallos

Todos los límites tendrán defaults conservadores, validación y topes duros: memoria residente, tamaño máximo de clave/registro/reporte de salida, número de corridas/archivos abiertos, bytes temporales totales, filas/tiempo y grupos totales. Cualquier suma/multiplicación de tamaños debe comprobar overflow. La falta de espacio, límite excedido, error de lectura/escritura, cancelación o corrupción devuelve error explícito; nunca cae silenciosamente a agregación ilimitada en RAM.

Crear temporales exclusivos con nombres impredecibles y permisos privados en un directorio permitido; registrar cada recurso para limpieza en cualquier camino de salida. No truncar ni reemplazar el destino final hasta completar la fusión y validar el reporte. Publicar mediante archivo temporal de salida y renombrado atómico cuando el sistema de archivos lo permita. Ningún error debe dejar corridas o un reporte parcial como resultado válido.

## Puerta de aceptación

1. Pruebas unitarias del formato: round-trip, versión no admitida, checksum corrupto, truncamiento, longitudes desbordadas y limpieza.
2. Pruebas del backend con spill forzado por umbral pequeño: una corrida y varias; claves duplicadas entre corridas; claves vacías; valores inválidos; todas las operaciones admitidas; límite de bytes/runs y fallos de E/S. Verificar que los temporales se eliminan en éxito y error.
3. E2E desde `.analisis` que compare el JSON de spill con el camino en memoria para orden, recuentos y valores dentro de tolerancia; demostrar que el runtime entrega la configuración AST tipada.
4. Guardas de arquitectura, manifiesto y Makefile; sanitizers; suite completa y CI del head final. Documentar limits/defaults reales y benchmark reproducible con medición de pico RAM/bytes temporales.
5. Mantener explícito que este contrato es local: no introduce red, clúster, cloud, Spark, Flink, Arrow ni Parquet.

Hasta superar toda esta puerta, la afirmación correcta es específica: «un corte opt-in de una clave y una métrica ya transmite el CSV streaming al reducer spillable y publica la salida ordenada sin materializar grupos; el agrupador de Dataset/Table sigue en memoria y no hay soporte industrial/distribuido».
