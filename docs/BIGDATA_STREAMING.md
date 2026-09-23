# Agrupación en flujo y spill local

`milena_stream_csv_grouped_with_options` ofrece agrupación local de CSV. Sin
spill, `max_groups` limita las claves en memoria y superarlo devuelve
`MILENA_ERR_OVERFLOW`. Con `spill_directory`, cada registro se escribe en una
de 1–64 particiones deterministas (FNV-1a de la clave), con límites
independientes de bytes y registros por partición. La recuperación ordena las
claves por bytes (`strcmp`) antes de emitir JSON, por lo que el resultado es
reproducible y coincide con la ejecución en memoria salvo por el marcador
`"derramado"`.

Los registros MLSP contienen la cabecera `MLSP`, la clave, una marca de validez
y el valor IEEE-754 de cada métrica, y un FNV-1a al final. Una cabecera,
registro incompleto o checksum que no coincida produce `MILENA_ERR_DATA`.
Valores métricos no numéricos no abortan la agrupación: incrementan
`valores_invalidos` y no contribuyen a esa métrica. Un registro CSV con número
de columnas incorrecto incrementa `filas_malformadas`.

El spill es **solo local y de un proceso**: usa archivos temporales en el
directorio indicado, mantiene en memoria los acumuladores finales y elimina los
archivos temporales al terminar o fallar. La capacidad máxima de estados finales
es `max_groups * spill_partitions`; no significa memoria constante ni
cardinalidad ilimitada. `spill_max_bytes` y `spill_max_records` se aplican a
cada partición.

Estas opciones están disponibles en la API C
`milena_stream_csv_grouped_with_options`; la sintaxis `.milena` todavía no
permite configurar spill y conserva la agrupación en memoria con su límite de
grupo. Las pruebas comparan la salida spill con la ruta en memoria y verifican
contadores de spill y limpieza de archivos. No hay coordinación entre procesos,
ejecución distribuida, Spark, Flink ni un motor industrial.
