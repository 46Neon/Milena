# Ejecución en flujo para grandes CSV

PR24 ofrece una ruta canónica y acotada para resumir CSV grandes sin
materializar el dataset completo. La entrada sigue el recorrido lexer → parser
→ AST → semántica → runtime; no hay un intérprete textual paralelo.

## Sintaxis humana recomendada

```milena
.analisis ventas_masivas {
    datos desde "datos/ventas.csv"
        procesar por lotes de 4096 filas
        con registros de hasta 8 MiB
        con columnas de 4096
        con filas hasta 1000000
        con tiempo hasta 30000 ms

    resumir {
        suma de "importe";
        media de "importe";
        minimo de "importe";
        maximo de "importe";
        contar de "importe";
        varianza de "importe";
        desviacion_estandar de "importe";
    }

    guardar resultado en "reporte_flujo.json"
}
```

`procesar por lotes de N filas` expresa el tamaño de lectura lógico. El motor
no conserva el lote completo: reutiliza un registro CSV, la cabecera, los
punteros de campos y un acumulador por métrica. `con registros de hasta N MiB`
y `con columnas de N` son opcionales; el límite duro del runtime es 64 MiB por
registro y 4096 columnas, y esos valores son los predeterminados. El tamaño
mínimo aceptado es 4096 bytes.
`con filas hasta N` y `con tiempo hasta N ms` fijan límites opcionales de 1 a
1.000.000.000 filas y de 1 a 3.600.000 ms de tiempo transcurrido. Se conservan
en el AST y se aplican tanto al resumen global como a la agrupación; superar
cualquiera devuelve error y no produce un reporte exitoso parcial. Estos límites
permiten fallar pronto con un diagnóstico explícito en lugar de reservar
memoria sin cota. La salida incluye los presupuestos aplicables, el número de
columnas de la cabecera y el pico de búfer observado.

La forma anterior de PR24 continúa funcionando solo como compatibilidad; no recibe operaciones nuevas:

```milena
dataset cargar flujo("datos/ventas.csv", 4096)
.resumir dataset { #suma("importe") #media("importe") #conteo("importe") }
.exportar { ("reporte_flujo.json") }
```

En la forma legacy, el segundo argumento es opcional y el valor predeterminado
es 4096 filas.

## Contrato arquitectónico

`stream.c` es un backend interno del runtime canónico. No tiene un ejecutable,
parser, lector de scripts ni ruta CLI propios: la única ruta de producto es
`lexer → parser → AST → semántica → milena_run_dataset_program → stream.c`.
La sintaxis humana conserva en el AST la operación, columna, tamaño de lote y
límites de registro, columnas, grupos, filas y tiempo; `stream.c` recibe
únicamente ese contrato tipado. Las pruebas directas del backend son pruebas unitarias, no una
segunda interfaz de usuario. SST, finanzas y análisis siguen siendo comandos
AST del mismo runtime (`AST_COMANDO_SST`, tablas y `finance.c`); el manifiesto
mantiene fuera del producto los módulos experimentales de VM/IR/GC.

## Qué hace

- Lee el CSV secuencialmente con un búfer de E/S de 64 KiB.
- Soporta registros entrecomillados, comillas escapadas y registros grandes
  hasta el límite configurado.
- Mantiene acumuladores de suma compensada, media, mínimo, máximo, conteo y
  varianza/desviación estándar en una pasada.
- También admite agrupación tipada en el mismo AST/runtime, tanto el mapa
  acotado en memoria como el corte spill de una clave de texto y una métrica.
  El modo spill emite en orden bytewise desde el callback y mantiene el
  presupuesto del reductor separado del registro CSV. El planner físico coloca
  explícitamente `SCAN_CSV_RECORDS → GROUPED_SPILL → REDUCE_PARTIAL_STATES →
  ORDER_BY_KEY → JSON_SINK`; la fusión externa combina estados parciales por
  clave y produce un orden global determinista. El escaneo permanece secuencial
  y solo consume registros CSV completos, por lo que las comillas/saltos de
  línea no se parten en fronteras arbitrarias.
- Rechaza columnas inexistentes, demasiadas columnas (máximo 4096), registros
  que exceden el límite y CSV con comillas sin cerrar.
- Reporta filas leídas, filas válidas, filas malformadas, límite de registro,
  límite de columnas, pico de búfer y tiempo observado.
- No crea `Dataset`, `MilenaTable` ni una copia de todas las filas.

## Límites deliberados

La agrupación normal conserva su mapa en memoria, sujeto a sus límites de
flujo. Para una entrada CSV que ya está en esta ruta canónica se admite también
un corte vertical de spill agrupado, sin crear `Dataset` ni `MilenaTable`:

```milena
.analisis ventas_spill {
    variable categoria texto
    variable importe numerica
    datos desde "datos/ventas.csv"
        con registros de hasta 8 MiB
        con columnas de 4096
        con filas hasta 10000000
        con tiempo hasta 300000 ms
    agrupar por "categoria"
        #spill("scratch-ventas.bin", 262144, 1073741824, 4096, 100000)
        resumir { suma de "importe"; }
    guardar resultado en "reporte_spill.json"
}
```

`#spill` reutiliza la política tipada existente en el AST: ruta scratch,
memoria del reductor (4 KiB–512 MiB), cuota del spill append-only (1 byte–4
GiB), tamaño máximo de clave (2 bytes–1 MiB) y grupos de salida (1–1.000.000).
Para esta forma de streaming también son obligatorios los límites AST de filas
y tiempo (`con filas hasta ...` y `con tiempo hasta ... ms`); todos se validan
antes de ejecutar. La clave debe declararse `texto`; `suma`, `media`, `minimo`
y `maximo` requieren una columna declarada `numerica` (valores CSV finitos
interpretados como FLOAT64). `contar` admite cualquier tipo declarado. Se
rechazan varias claves, varias métricas, claves categóricas/compuestas y las
operaciones no soportadas por el adaptador spill. Cada registro completo —con
comillas, comas y saltos de línea entrecomillados— pasa por el mismo parser CSV
acotado ya usado por el streaming; no existe un segundo parser.

El reporte se escribe por callback del reductor, en orden lexicográfico estable,
una clave a la vez; no se materializa el conjunto de grupos. El máximo de bytes
de clave incluye el byte de etiqueta interna usado para distinguir claves
textuales. `filas_malformadas` informa globalmente celdas vacías/numéricamente
inválidas; en esta versión de spill no se emite un contador de inválidos por
clave. Se publica mediante
un archivo hermano temporal solo después de terminar parsing, reducción y
cierre con éxito. Cualquier error de fila, tiempo, cuota, clave, grupo, escritura
o cierre elimina el scratch y el temporal y no publica un reporte parcial. Los
valores numéricos vacíos/no finitos/invalidables se omiten de la métrica y
aparecen como `null` cuando un grupo no tiene ningún valor válido.

Los topes de memoria son independientes, no una promesa de RSS global: el
registro CSV actual tiene como máximo `max_record_bytes` (capacidad asignada),
la copia de la cabecera está acotada por el registro de cabecera, los arrays de
columnas por `max_columns`, y el buffer stdio del lector es 64 KiB. El mapa y
los buffers de ordenamiento/fusión del reductor están acotados por su presupuesto
`memoria_reductor_bytes`, adicional a lo anterior, más como máximo una copia
transitoria de la clave emitida (`max_key_bytes`); las asignaciones del llamador
y los internals de libc/stdio quedan fuera de esa cifra. En disco, la entrada del
spill está limitada por `cuota_spill_bytes` y los runs de ordenamiento externo
por hasta dos cuotas adicionales (máximo documentado del reductor: 3× la cuota,
además del archivo temporal del reporte y metadatos del sistema de archivos).
El reporte está acotado por los límites de claves y grupos, pero no se incluye en
la cuota del scratch. No se midió RSS global, así que no se afirma ese límite.

Las operaciones `Dataset`/`MilenaTable` y el agrupador histórico permanecen en
memoria; esta nueva ruta aplica únicamente al agrupamiento tipado de CSV en
streaming. Multi-métrica con spill, más de una clave, joins, ordenamiento de
filas, Parquet/Arrow y ejecución distribuida siguen fuera de este corte.

No admite mediana, percentiles, joins, limpieza que necesite observar todo el
conjunto ni transformaciones materializadas. Tampoco ofrece procesamiento
distribuido, reanudación, compresión ni garantías de latencia fija.

El tiempo en milisegundos se mide y se informa únicamente como observabilidad;
no se promete una latencia fija. Leer un archivo grande cuesta en proporción a
sus filas y depende del disco, sistema operativo, tamaño de los registros y
hardware. La garantía práctica de esta ruta es memoria acotada por
`O(columnas + métricas + registro máximo)` y una sola pasada, no una cifra de
milisegundos.

## Contrato de rendimiento

Las mejoras posteriores deben conservar:

- cero copias del dataset completo;
- una pasada cuando la operación lo permita;
- acumuladores numéricamente estables;
- límites configurables con tope duro y errores accionables;
- mediciones reproducibles de filas, límites, tiempo y errores;
- pruebas con archivos pequeños, filas inválidas, comillas y registros que
  superen el límite.
