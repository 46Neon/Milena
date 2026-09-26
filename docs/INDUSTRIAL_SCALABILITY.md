# Contrato de escalabilidad industrial

PR #26 añade presupuestos explícitos al backend de flujo canónico. El objetivo
no es prometer una latencia fija: es impedir que un proceso de datos consuma
recursos indefinidamente y permitir que un orquestador trate el límite como un
fallo controlado.

## Presupuestos

`MilenaStreamOptions` admite:

- `max_rows`: máximo de filas de datos que se pueden leer; `0` significa sin
  límite explícito.
- `max_elapsed_milliseconds`: presupuesto de tiempo de pared; `0` significa
  sin límite explícito.
- `max_record_bytes` y `max_columns`: límites físicos ya existentes.

Al superar un presupuesto, la operación termina con `MILENA_ERR_OVERFLOW` y un
mensaje en español. `MilenaStreamReport` conserva las filas procesadas, el
pico de registro, el tiempo observado y `resource_limit_reached`, incluso
cuando la operación termina por el límite. Esto permite reintentar, dividir el
trabajo o enviarlo a una cola sin interpretar un resultado parcial como éxito.

## Garantía actual

La memoria del resumen sigue acotada por:

```text
O(columnas + métricas + registro_máximo)
```

El backend realiza una pasada y no materializa `Dataset` ni `MilenaTable`.
Los presupuestos no convierten todavía a Milena en un motor distribuido: no
hay spill a disco, particionado, ejecución paralela ni reanudación. Esas
capacidades requieren contratos posteriores y benchmarks reproducibles.

## Uso recomendado

Un servicio debe fijar al menos `max_record_bytes`, `max_columns` y un
presupuesto de filas o tiempo antes de aceptar trabajo externo. Debe guardar
el reporte junto con la versión de Milena, el tamaño del archivo, la métrica,
el código de salida y la razón del límite.

La sintaxis del lenguaje sigue pasando por lexer → parser → AST → semántica →
runtime. Estos campos son parte del contrato de recursos del backend y no una
segunda ruta de ejecución.

## Carga CSV materializada

La fuente materializada heredada `dataset cargar datos("entrada.csv")` puede
fijar límites opcionales en esa misma declaración:

```milena
dataset cargar datos("entrada.csv") con filas hasta 100000 con columnas de 64 con registros de hasta 8 MiB con memoria hasta 256 MiB con entrada hasta 2048 MiB con tiempo hasta 30000 ms
```

`filas` acepta 1–1.000.000.000; `columnas`, 1–4096; `registros`, 5 KiB–64 MiB
en pasos de 1 KiB; `tiempo`, 1–3.600.000 ms; `memoria`, de 1 byte a 1024 MiB
con conversión exacta a bytes; y `entrada`, un entero positivo de MiB
representable como tamaño en bytes. Cada cláusula se admite una sola vez; los
valores cero y las repeticiones se rechazan. Si una cláusula se omite, se
conserva el default materializado previo: máximo de 5.000 filas, 70 columnas y
el límite de registro legado `max_field_bytes * columnas_físicas +
columnas_físicas` (`max_field_bytes` inicia en 1 MiB); no se activa timeout ni
se fija un límite total de entrada o presupuesto explícito de memoria. El límite
de registro acota el registro completo, no valida cada campo por separado. La
ruta `datos desde`/`dataset cargar flujo` permanece separada y no se cambia ni se
selecciona automáticamente como fallback.

Los límites de filas, columnas, bytes de registro y bytes totales de entrada se
aplican en el cargador: filas cuenta cada registro de datos leído tras el
encabezado, incluso líneas vacías o registros luego inválidos; los registros
están acotados antes de crecer su búfer, y el número de campos del encabezado se
cuenta antes de construir sus strings. `con entrada hasta N MiB` cuenta bytes
físicos crudos de todo el CSV desde el primer byte: incluye encabezado y cada
separador de registro, por lo que CRLF cuenta como dos bytes. Se acepta un
archivo exactamente del tamaño límite y se rechaza el byte cap+1 antes de
almacenarlo en el buffer de registro. Este máximo del archivo es distinto del
cap `registros`, que limita cada registro CSV lógico; el registro incluye bytes
CSV lógicos y saltos internos normalizados, pero excluye el NUL y su separador.
El límite de tiempo se consulta durante la lectura y entre fases de parseo de
registros; no interrumpe de forma preemptiva una llamada de I/O o una operación
individual de parseo ya iniciada. Si cualquier límite vence, el cargador
destruye su `Dataset` temporal, devuelve error y no sustituye el `Dataset`
previo; el runtime tampoco inicia publicación del reporte, por lo que un destino
ya existente permanece intacto.

`con memoria hasta N MiB` es un presupuesto independiente por fuente sobre la
suma de tamaños solicitados actualmente para las asignaciones retenidas por el
`Dataset` del cargador. Cuenta el string del nombre de archivo, las cadenas de
encabezado/celdas (incluida la capacidad solicitada y el NUL), la capacidad de
los vectores de campos del encabezado y de cada fila, y la capacidad del vector
de punteros a filas. Antes de cada `malloc`/`realloc` contabilizado se comprueba,
con aritmética de overflow, que reemplazar la asignación deje el total dentro
del límite; una solicitud que lo exceda produce error y no tiene fallback. Este
presupuesto cuenta asignaciones retenidas solicitadas, no RSS ni memoria
residente: excluye bookkeeping/fragmentación del allocator, el buffer crudo de
registro CSV, picos transitorios de `realloc`, conversión a `MilenaTable` y
memoria/copies de transformaciones y operadores. No limita el tamaño del
archivo fuente por bytes.

El límite `entrada` limita bytes del archivo, no asignaciones retenidas; el
límite `memoria` limita las asignaciones enumeradas, no bytes de archivo. Filas,
columnas, registro, entrada y memoria son políticas independientes; ninguna es
un techo de RSS/proceso ni evita el overcommit del sistema. El Dataset previo y
el reporte/destino exportado se conservan ante cualquier fallo de carga.
