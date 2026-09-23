# Contrato para spill-to-disk de agrupaciones en flujo

**Estado:** hay dos cortes distintos. El `.agrupar` tabular canónico conserva su adaptador de tabla existente y sigue materializando la entrada/salida en RAM. La agrupación de CSV en el runtime streaming tiene un corte de spill directo `stream.c → grouped_aggregate.c`: no crea `Dataset`/`MilenaTable`, reutiliza el lector CSV existente y emite el JSON desde un callback ordenado a un staging file antes de publicarlo. El adaptador tabular conserva una clave y una métrica. La ruta canónica `.analisis` acepta ahora una o dos claves TEXT, esta última solo con `#spill`, y pasa ambas mediante `milena_stream_csv_grouped_spill_with_keys_and_options`; reutiliza el codec/reducer existentes. Las claves compuestas numéricas, el spill tabular compuesto, datos distribuidos y garantías industriales permanecen fuera del alcance.

## Límite arquitectónico

La única ruta de producto seguirá siendo `lexer → parser → AST tipado → semántica → runtime → stream.c`. La sintaxis humana expresará las políticas de spill en el AST; el runtime validará los límites antes de ejecutar y pasará opciones tipadas al backend. El spill no tendrá parser, CLI, runtime ni comando externo propios. El manifiesto de fuentes debe clasificar cada módulo nuevo como producto; `Makefile` debe incorporarlo a las fuentes oficiales. El verificador existente debe fallar ante fuentes C presentes pero no clasificadas: no se permite resolver diferencias excluyendo archivos o agregando módulos inexistentes.

## Corte vertical implementado en #agrupar tabular

La sintaxis del adaptador tabular admite `#spill("ruta-nueva", memoria_bytes, cuota_spill_bytes, max_key_bytes, max_grupos[, 0, max_runs])` dentro de `.agrupar dataset { ... }`. Los límites numéricos son enteros tipados en el AST: memoria 4 KiB–512 MiB, cuota 1 B–4 GiB, clave 2 B–1 MiB, grupos 1–1,000,000 y runs 1–65,536 (default 4096); la ruta scratch queda limitada a 220 bytes. En el sexto parámetro solo se admite `0` (sin cuota de reporte JSON para esta salida `MilenaTable`); un límite no nulo se rechaza en semántica porque el adaptador no produce un reporte JSON. La política es opt-in; sin ella sigue el camino histórico de `milena_table_group_by`. Una solicitud explícita que no esté soportada falla y nunca vuelve silenciosamente al backend en memoria.

El adaptador usa una clave STRING (una clave únicamente) y una métrica; `conteo` acepta cualquier columna, `suma` acepta FLOAT64 o INT64 y las otras tres métricas requieren FLOAT64. La suma INT64 mantiene un acumulador firmado exacto y rechaza desbordamiento; no convierte el resultado por binary64 (por lo que valores por encima de 2^53 conservan precisión). La clave nula no colisiona con texto vacío; la clave vacía observada sí es válida. El estado mergeable distingue y serializa observaciones válidas, nulas e inválidas; el JSON de CSV spill ahora expone `valores_validos`, `valores_nulos` y `valores_invalidos`, separando celdas vacías de texto no numérico. La salida es una `MilenaTable` materializada con tope de grupos y sale en orden lexicográfico binario; el backend histórico conserva orden de primera aparición. La suma/media con reducer mergeable puede diferir por redondeo del backend anterior; la equivalencia numérica se comprueba con tolerancia, no bit a bit.

Límites todavía abiertos del adaptador tabular: su input `Dataset` y la tabla previa ya residen en RAM, y la salida también se materializa. Scratch es una ruta provista por el programa y se crea exclusivamente para no sobrescribir un archivo existente; todavía no se garantizan nombres aleatorios privados ni writers concurrentes sobre una misma ruta. El fan-in es fijo de dos vías. La política AST incluye límites de filas/tiempo para CSV streaming y un máximo configurable de runs para ambas rutas.


## Primer corte de spill directo del CSV streaming

La sintaxis humana sigue el bloque `agrupar por ... resumir { ... }` del modo streaming, y añade la política AST ya tipada:

```milena
.analisis ejemplo {
  variable grupo texto
  variable importe numerica
  datos desde "entrada.csv" con filas hasta 10000000 con tiempo hasta 300000 ms
  agrupar por "grupo" #spill("scratch.bin", 262144, 1073741824, 4096, 100000, 104857600, 4096)
    resumir { suma de "importe"; media de "importe"; contar de "importe"; }
  guardar resultado en "reporte.json"
}
```

En esta sintaxis de flujo, `#spill` contiene memoria del reductor, cuota de
bytes del spill de entrada, bytes máximos de clave codificada y máximo de grupos.
El sexto argumento fija el máximo de bytes del reporte final (opcional; `0`
selecciona el default de 1 GiB), y el séptimo fija máximo de runs iniciales
(opcional; default 4096, hard cap 65,536). La clave codificada del reducer
incluye el tag de tipo, la clave textual, un separador y el índice de métrica;
por eso el límite de clave contabiliza ese framing interno. El adaptador tabular acepta el valor
cero como marcador al especificar solo el séptimo argumento y rechaza una cuota
JSON no nula. Los límites de filas y
tiempo deben ser explícitos en `datos desde`; el registro CSV/cantidad de
columnas conservan sus límites existentes. La política se valida en el AST y la semántica rechaza tipos de clave/métrica
incompatibles y métricas no admitidas antes del runtime.

El lector usa `stream_read_record` y `stream_split` compartidos, respetando
comillas, comas y saltos de línea incrustados. Cada fila alimenta hasta 64
estados de métrica codificados como claves distintas dentro del mismo reducer;
no hay un reducer por métrica y ninguna fila se agrega a Dataset/Table. El
límite de salida cuenta claves base distintas. El reporte agrupa las métricas
por clave y conserva su orden declarado. `contar` cuenta campos no vacíos,
incluso si no son números; cada otra métrica numérica contabiliza vacío como
nulo y texto no numérico como inválido. `filas_validas` cuenta filas con al
menos una métrica válida y `filas_malformadas` filas con al menos un valor nulo
o inválido; ambos contadores pueden solaparse, y el detalle por métrica se
informa separadamente. Al finalizar, el reducer invoca un callback por estado ordenado grupo-métrica.
El adaptador comprueba el orden/ausencia de duplicados, agrupa las métricas en
una sola fila JSON por clave y verifica incrementalmente la cuota restante
antes de escribir cada métrica al archivo staging hermano. La publicación sustituye el destino atómicamente en POSIX y
con `MoveFileEx` en Windows, solo cuando lectura, límites, reducción, escritura
y cierre finalizaron bien. En error se elimina staging, runs y scratch. La
prueba E2E compara el valor del corte spill con el agrupamiento de referencia
en memoria mediante tolerancia y compara los contadores de filas válidas,
nulas e inválidas por grupo. El reporte JSON incluye `bytes_spill`,
`registros_spill` y `runs_spill`: bytes/records son las escrituras append-only
contabilizadas por el `MilenaSpillStore` real (incluyen cabeceras/trailers del
formato), y runs es el número de corridas iniciales efectivamente materializadas
por el sorter del reducer. No se estiman ni extrapolan desde las opciones.
El wrapper copia estos contadores del estado del reducer antes de cerrarlo,
los publica solo tras completar el staging y dejan valor cero en el reporte API
si la operación falla. La validación reproducible de un millón de filas con
presupuesto de 4 KiB exige bytes y registros reales mayores que cero (y una
corrida ordenada); estos valores son observaciones, sin umbral de rendimiento.

El contrato de memoria distingue el búfer de registro CSV (capacidad limitada
por `max_record_bytes`), la copia acotada de cabecera, `max_columns` punteros,
el búfer stdio de 64 KiB, un buffer callback de hasta una clave máxima y el
presupuesto separado del reductor (mapa + sorting/merge). Los límites de caller
y libc/stdio no equivalen a una cota de RSS total. En disco, el archivo de
entrada append-only se limita por la cuota configurada y los runs ordenados
pueden consumir hasta dos cuotas adicionales (hasta 3× la cuota en total),
aparte del reporte staging. El reporte cuenta con una cuota independiente de
bytes configurada en AST; no se mezcla con la cuota de scratch.

La ruta `.analisis` integra una o dos claves `TEXT` con spill y hasta 64 métricas;
quedan fuera las claves numéricas compuestas, el `.agrupar dataset` compuesto,
uniones y ordenamiento general de filas, Parquet/Arrow y ejecución remota. Las
operaciones `Dataset` y `MilenaTable` siguen en memoria. El workflow de un millón
de filas compara la salida en memoria y con spill y registra el entorno y las
mediciones observadas; no mide RSS global ni establece SLO o garantía de
rendimiento. La ruta scratch la proporciona el programa y se crea en modo
exclusivo; si existe, se rechaza sin sobrescribir. No se prometen nombres
aleatorios privados ni concurrencia de writers sobre la misma ruta.

## Subfase backend: dos claves de texto tipadas

El backend CSV expone `milena_stream_csv_grouped_spill_with_keys_and_options`, que recibe un array de uno o dos descriptores `{name, type}`. En esta subfase ambos descriptores deben declarar `MILENA_STREAM_GROUP_KEY_TEXT`; tipos numéricos, más de dos claves y nombres de columna repetidos se rechazan. La API anterior de una sola clave permanece como adaptador fino a esta llamada, por lo que no hay copias del codec ni reducers paralelos. Ambas formas reutilizan el reducer agrupado, sus hasta 64 métricas, presupuestos de memoria/scratch/clave/salida/runs, telemetría, ordenamiento externo, staging atómico y limpieza existentes.

Las claves de dos partes usan el codec de producto `group_key_codec.c`: wire versionado MGK v1 con longitud prefijada, etiqueta de tipo y validez explícita por componente. El límite `max_key_bytes` incluye el codec y el framing grupo/métrica; el codec impone además un tope propio de 4096 bytes. Las claves se comparan como bytes completos del wire para reducir, de modo que separadores dentro de valores, longitudes distintas y claves con el mismo primer componente pero distinto segundo no colisionan. Las cadenas CSV válidas vacías se conservan como `valor:""` y son diferentes del componente inválido; bytes que no formen UTF-8 en una clave TEXT se normalizan al componente inválido (por tanto, esos valores inválidos se agrupan entre sí) y se reportan con `valor:null` y `valido:false`.

El JSON mantiene el esquema previo para una sola clave (`clave`). Para dos claves, la cabecera informa `columnas_grupo:[{"nombre":"...","tipo":"texto"}, ...]` y cada fila de `resultados` usa `claves:[{"nombre":"...","tipo":"texto","valor":"...","valido":true}, ...]` seguida por el mismo array `metricas`; el componente inválido conserva su nombre/tipo con `valor:null,"valido":false`. Los contadores de grupos, orden determinista por bytes del wire, métricas múltiples y atomicidad de publicación no cambian. La prueba backend directa cubre comas/comillas CSV, componente vacío e inválido, mismo primer componente/diferente segundo, dos métricas y fallo del límite de grupos sin salida parcial.

**Integración canónica:** el parser admite hasta dos nodos de clave; semántica exige que las dos columnas estén declaradas como `texto`, no sean iguales y que haya `#spill`; el planner lleva ambos nodos al runtime y el runtime invoca esta API, sin copiar codec/reducer. Una tercera clave se rechaza al parsear, y dos claves sin spill se rechazan antes de abrir fuente/scratch. La suite E2E valida composite spill y fallos/cleanup; el hito de un millón de filas sigue cubriendo únicamente las consultas de una clave previamente descritas, no valida cardinalidad compuesta arbitraria.

## Semántica y determinismo

- Procesar el CSV con el lector actual, que reconoce registros entrecomillados y saltos de línea internos; no particionar por offsets arbitrarios que puedan cortar registros.
- Al alcanzar el umbral residente, ordenar el lote de grupos por los bytes decodificados de la clave y escribir una corrida temporal tipada. Las métricas válidas/inválidas, el contador, los acumuladores de suma/media/varianza y las estadísticas min/max deben conservar semántica equivalente al camino sin spill.
- Fusionar las corridas con una cola de prioridad acotada, con un orden fijo por clave y ordinal de corrida. El orden reduce flotantes debe ser determinista para los mismos datos, opciones y versión del formato. La prueba de equivalencia compara valores numéricos con tolerancia documentada; no promete identidad bit a bit frente al orden de acumulación de una sola pasada.
- Emitir grupos en orden lexicográfico de bytes, idéntico al contrato actual. No crear un grupo para claves no observadas; la clave vacía sí es válida.

## Formato persistente

El estado de agregado actual usa wire v4 little-endian de 96 bytes: contadores de
válidos/nulos/inválidos, seis campos binary64 para la estadística mergeable, tipo
de agregado, suma `INT64` exacta y checksum. Rechaza mezcla FLOAT64/INT64,
versión o checksum inválido y overflow de suma exacta. El reducer trata las
claves como bytes opacos; MGK v1 enmarca claves de una o dos columnas `TEXT`, y
el adaptador de CSV admite hasta 64 métricas. El wire de agregación no es aún un
contrato persistente plenamente autocontenido: falta una cabecera de corrida
ligada al esquema, a las métricas y a un identificador de ejecución. Cada registro
usa longitudes y valores de ancho fijo, nunca `fwrite` de structs C con padding o
ABI dependiente. El checksum detecta corrupción, no autenticidad criptográfica.

## Recursos y fallos

Todos los límites tendrán defaults conservadores, validación y topes duros: memoria residente, tamaño máximo de clave/registro/reporte de salida, número de corridas/archivos abiertos, bytes temporales totales, filas/tiempo y grupos totales. Cualquier suma/multiplicación de tamaños debe comprobar overflow. La falta de espacio, límite excedido, error de lectura/escritura, cancelación o corrupción devuelve error explícito; nunca cae silenciosamente a agregación ilimitada en RAM.

Crear temporales exclusivos con nombres impredecibles y permisos privados en un directorio permitido; registrar cada recurso para limpieza en cualquier camino de salida. No truncar ni reemplazar el destino final hasta completar la fusión y validar el reporte. Publicar mediante archivo temporal de salida y renombrado atómico cuando el sistema de archivos lo permita. Ningún error debe dejar corridas o un reporte parcial como resultado válido.

## Puerta de aceptación

1. Pruebas unitarias del formato: round-trip, versión no admitida, checksum corrupto, truncamiento, longitudes desbordadas y limpieza.
2. Pruebas del backend con spill forzado por umbral pequeño: una corrida y varias; claves duplicadas entre corridas; claves vacías; valores inválidos; todas las operaciones admitidas; límite de bytes/runs y fallos de E/S. Verificar que los temporales se eliminan en éxito y error.
3. E2E desde `.analisis` que compare el JSON de spill con el camino en memoria para orden, recuentos y valores dentro de tolerancia; demostrar que el runtime entrega la configuración AST tipada.
4. Guardas de arquitectura, manifiesto y Makefile; sanitizers; suite completa y CI del head final. Documentar limits/defaults reales y benchmark reproducible con medición de pico RAM/bytes temporales.
5. Mantener explícito que este contrato es local: no introduce red, clúster, cloud, Spark, Flink, Arrow ni Parquet.

La afirmación correcta, una vez que la suite E2E y la CI del head pasan, es específica: «el corte opt-in de CSV streaming admite una o dos claves TEXT con #spill y hasta 64 métricas, transmite el CSV al reducer spillable y publica resultados ordenados sin materializar grupos; el agrupador de Dataset/Table sigue en memoria, las claves compuestas numéricas están pendientes y no hay soporte industrial/distribuido».
