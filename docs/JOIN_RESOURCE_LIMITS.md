# Límites de recursos para joins canónicos

El bloque `.unir` conserva el recorrido del lenguaje `lexer → parser → AST tipado → semántica → runtime → MilenaTable`. Su política opcional es:

```milena
.unir {
  #derecha("lookup.csv")
  #clave("id")
  #limites(268435456, 1000000)
}
```

Los dos argumentos son literales enteros: presupuesto estimado en bytes y máximo de filas de salida. Si se omite `#limites`, el AST tipado usa 256 MiB y 1.000.000 de filas. La política acepta presupuestos entre 4 KiB y 1 GiB, y entre 1 y 10.000.000 de filas; los límites superiores son caps del API acotado. Un resultado que excede el máximo de filas o la estimación de bytes se rechaza con `MILENA_ERR_OVERFLOW` antes de materializar la salida. La tabla de destino solo se reemplaza al completar el join, por lo que un fallo no publica un resultado parcial.

## Qué mide el presupuesto

Antes de crear los mapas de filas y la salida, el backend calcula una estimación conservadora para las asignaciones conocidas que controla el join: índices hash y auxiliares, pares de filas, columnas/metadata del resultado, valores, validez, diccionarios y buffers temporales que se usan al copiar columnas al resultado. Para estimar los datos considera la fila de mayor tamaño observada en cada tabla y el número previsto de pares; todos los cálculos de tamaño comprueban overflow.

Esta cifra es un presupuesto de preflight para el trabajo interno y la materialización del resultado, **no una garantía de RSS global ni un límite duro de memoria del proceso**. Quedan fuera las tablas de entrada y su carga/parseo CSV, una tabla de salida preexistente propiedad del llamador, el runtime y otras asignaciones ajenas al join, el overhead del allocator/libc y cualquier RSS compartida con otros procesos. Las asignaciones reales aún pueden fallar; en ese caso el join informa el error y no confirma la tabla temporal.

El C API `milena_table_join_with_limits` aplica esa política. El C API histórico `milena_table_join` conserva su contrato previo sin máximo de filas configurado; el runtime del lenguaje llama al API acotado con los límites tipados del AST.

## Límites funcionales

El runtime del lenguaje actualmente acepta una clave de join y carga la fuente derecha CSV a una tabla antes de ejecutar el join. El resultado sigue siendo una `MilenaTable` materializada en memoria: no hay join particionado, spillable, streaming ni pushdown del planner. No se deben usar rangos de partición por bytes para CSV: pueden cortar registros entrecomillados o multilínea. La seguridad de esos límites y la semántica de CSV requieren un diseño de lector consciente de registros antes de cualquier pushdown.

Los límites del join no establecen una cota de RSS del sistema, no demuestran un SLO ni validan Termux/aarch64. Arrow/Parquet, carga remota y ejecución distribuida quedan fuera de este contrato.
