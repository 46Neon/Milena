# PR25: base reproducible para Big Data

PR25 amplía la ejecución de CSV sin crear un producto de datos separado. Tanto el resumen global como la agrupación se ejecutan por la ruta canónica del lenguaje: **lexer → parser → AST tipado → semántica → runtime → `stream.c`**. No hay un ejecutable, parser o runtime alternativo para análisis Big Data.

## Resumen global en flujo

`datos desde "archivo.csv"` selecciona el CSV; `resumir { ... }` expresa operaciones de flujo tipadas y `guardar resultado en "reporte.json"` define el destino. El lector reutiliza un búfer de registro, conserva solo la cabecera y acumuladores, y valida límites de registros, columnas, filas y tiempo antes de continuar. El reporte muestra filas leídas/válidas/malformadas, bytes, rendimiento observado, capacidad del búfer y valores inválidos por métrica. Un valor métrico inválido se omite solo de esa métrica; las demás métricas válidas de la misma fila sí se acumulan.

## Agrupación en flujo con estado acotado

Sintaxis humana y canónica:

```milena
.analisis ventas_agrupadas {
  datos desde "datos/ventas.csv"
    procesar por lotes de 4096 filas
    con grupos de 1000
    con filas hasta 1000000
    con tiempo hasta 30000 ms
  agrupar por "region" resumir {
    suma de "importe";
    media de "importe";
    contar de "referencia";
  }
  guardar resultado en "reporte.json"
}
```

`agrupar por` y cada operación de `resumir` quedan tipados en el AST. La clave se compara como texto CSV decodificado y los resultados se emiten en orden lexicográfico estable. El máximo predeterminado es 1.000 grupos; el lenguaje permite configurarlo hasta un tope duro de 100.000, y el backend impone además un presupuesto de estado de 64 MiB y claves de hasta 4 KiB. `con filas hasta N` aplica un máximo de filas (1–1.000.000.000) y `con tiempo hasta N ms` un límite de tiempo transcurrido (1–3.600.000 ms); ambos quedan en el AST y se validan antes de entrar al backend. Si cualquier límite se supera, la operación falla en lugar de crecer sin cota; no se entrega un resultado parcial como éxito.

Las métricas numéricas ignoran solo el valor no numérico de su columna, incrementan su propio contador de `valores_invalidos` y marcan la fila como malformada; otros valores válidos de esa misma fila todavía se agregan. `contar de` cuenta campos no vacíos, por lo que también admite columnas textuales. Una fila con cantidad de columnas incorrecta, CSV inválido, cabecera duplicada o columna solicitada inexistente rechaza toda la operación. No se crean grupos sin filas; un campo de clave vacío sí es una clave válida. La salida está ordenada por clave y la acumulación conserva el orden de lectura para una ejecución reproducible sobre la misma entrada.

`python3 benchmarks/grouped_stream_benchmark.py` genera casos deterministas de 100 filas/4 grupos y 10.000 filas/32 grupos, valida recuentos y métricas inválidas y mide la ejecución completa con `milena run`. Los casos grandes requieren `--large-rows N` explícito y tienen un máximo de 1.000.000 filas. `make benchmark-stream-grouped` ejecuta únicamente los casos acotados y forma parte de la ruta de pruebas Linux.

## Qué todavía no hace esta fase

La agrupación conserva sus acumuladores en memoria por omisión y falla al alcanzar `max_groups`. Como primer corte ejecutable, la API tipada C `milena_stream_csv_grouped_with_options` permite spill local explícito a corridas temporales acotadas, checksums y merge determinista de claves repetidas; la política aún no está expuesta por el AST/runtime. La rama `main` tiene un planificador/ejecutor local genérico que describe rangos contiguos de bytes y ejecuta trabajadores locales, pero sus rangos no reconocen límites de registros ni comillas CSV; no se usa para leer, derramar o combinar estos agregados. El ejecutor no equivale a un motor distribuido.

En este head, el spill reside en el módulo de producto existente `src/stream.c` y no crea fuentes paralelas `spill.c`/`spill_store.c`. El manifiesto mantiene clasificadas todas las fuentes C existentes; no se añaden módulos futuros ni experimentales al producto.

El estado, formato temporal y límites reales están en [`GROUPED_SPILL_CONTRACT.md`](GROUPED_SPILL_CONTRACT.md). La prueba fuerza varias corridas, compara agregados con memoria y verifica un fallo por límite de bytes; faltan el acceso desde AST/runtime y benchmarks específicos de alta cardinalidad/pico RSS. Esta primera API no habilita opciones desde `.analisis`. Arrow/Parquet, nube, Spark, Flink, red y ejecución distribuida tampoco forman parte de esta fase; no se simulan ni se prometen.

## Benchmarks y afirmaciones

`python3 benchmarks/stream_benchmark.py` y `python3 benchmarks/grouped_stream_benchmark.py` generan CSV deterministas y ejecutan programas `.milena` por la ruta canónica. `--large-rows` es opt-in. Las métricas de tiempo y throughput son observaciones del equipo/compilador/entorno y no fijan latencia, rendimiento industrial ni escalabilidad. Para Termux/Android/aarch64 hacen falta validaciones y mediciones reales en esos dispositivos.
