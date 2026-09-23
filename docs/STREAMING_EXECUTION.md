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
- También admite agrupación tipada en el mismo AST/runtime. Las claves emitidas
  se ordenan byte a byte y el número de grupos tiene un límite explícito; la
  especificación y sintaxis están en
  [`PR25_BIG_DATA_FOUNDATION.md`](PR25_BIG_DATA_FOUNDATION.md).
- Rechaza columnas inexistentes, demasiadas columnas (máximo 4096), registros
  que exceden el límite y CSV con comillas sin cerrar.
- Reporta filas leídas, filas válidas, filas malformadas, límite de registro,
  límite de columnas, pico de búfer y tiempo observado.
- No crea `Dataset`, `MilenaTable` ni una copia de todas las filas.

## Límites deliberados

La agrupación mantiene su tabla de grupos en memoria, hasta el límite
configurado (predeterminado 1.000, tope duro 100.000), con un presupuesto de
estado adicional de 64 MiB y claves de hasta 4 KiB. Si se alcanza cualquiera
de esos presupuestos, falla explícitamente; **no hay spill-to-disk**. El
contrato del siguiente incremento está documentado en
[`GROUPED_SPILL_CONTRACT.md`](GROUPED_SPILL_CONTRACT.md).

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
