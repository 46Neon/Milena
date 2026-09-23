# PR25: base reproducible para Big Data

PR25 no crea un producto de datos separado. Formaliza el contrato interno
`MilenaStreamOptions`/`MilenaStreamReport` y lo ejecuta únicamente mediante
`lexer → parser → AST → semántica → runtime → stream.c`. La sintaxis sigue
siendo humana y española (`datos desde`, `resumir`, `suma de`, `contar de`).

## Contrato medido

El backend reutiliza un registro acotado, la cabecera y acumuladores tipados.
Los límites de registro y columnas se validan antes de reservar memoria. El
reporte conserva filas leídas, válidas y malformadas, bytes de entrada, tiempo
observado, filas/s, MB/s, lote y pico de registro. Una fila con número inválido
es malformada y no se incorpora al acumulador; el recorrido sigue siendo de una
pasada. Las cargas que caben en memoria deben compararse contra la ruta de
Dataset en pruebas posteriores; PR25 no inventa una segunda representación.

## Benchmarks

`python3 benchmarks/stream_benchmark.py` genera CSV deterministas de 100 y
10.000 filas y ejecuta un programa `.milena` canónico. `--large-rows N` es
opt-in para no hacer CI impredecible. Se registran plataforma, arquitectura,
compilador y `GITHUB_SHA`; no se prometen milisegundos, escalabilidad ni
rendimiento industrial.


## Contrato canónico de streaming (PR25)

El flujo CSV es una capacidad del lenguaje, no un ejecutable C paralelo. La
forma humana canónica es:

```milena
datos desde "datos.csv" procesar por lotes de 4096 filas
resumir { suma de "importe"; media de "importe"; minimo de "importe";
          maximo de "importe"; contar de "importe"; varianza de "importe";
          desviacion_estandar de "importe"; }
guardar resultado en "resumen.json"
```

Lexer, parser y AST conservan la operación de cada métrica; la semántica
rechaza operaciones no registradas y el runtime ejecuta el plan tipado mediante
los acumuladores de una pasada. El contrato actual es un resumen **global
numérico** con `contar` en español (reportado como operación `conteo`) y memoria acotada por registro, cabecera y acumuladores: no agrupa,
no hace joins, no usa spill-to-disk y no distribuye la ejecución. La sintaxis
legacy `operacion:columna` se conserva solo por compatibilidad y no constituye
una ruta de lenguaje adicional. Agrupaciones streaming, spill-to-disk, joins,
formatos columnares y ejecución distribuida son fases futuras explícitas.

## Alcance y siguientes fases

Incluido: contrato observable, fixtures reproducibles, métricas, límites de
propiedad/vida del búfer, errores de CSV y target CI `benchmark-stream`.
No incluido: agrupaciones streaming, estado ilimitado, spill-to-disk,
Arrow/Parquet ni paralelismo. Esas fases requieren contratos de partición,
orden, memoria, formatos y equivalencia antes de implementarse; no deben
bypassear el runtime canónico.


## Incremento de contrato de flujo (PR25)

El reporte distingue `pico_registro_bytes` (máximo payload observado, sin NUL) de
`capacidad_buffer_registro_bytes` (capacidad reservada y potencialmente mayor).
La capacidad configurada sigue siendo `limite_registro_bytes`.

Cada métrica se acumula de forma independiente: un valor numérico válido
contribuye a esa métrica aunque otra métrica de la misma fila sea inválida.
`filas_malformadas` cuenta filas con al menos un valor métrico inválido y cada
resultado incluye `valores_invalidos` para hacer visible la validez por métrica.
Las filas con columnas incorrectas siguen siendo descartadas para todas las
métricas. Esta semántica no implica tolerancia silenciosa: los contadores quedan
en el JSON determinista.

El benchmark rechaza `--large-rows` negativo; los casos grandes siguen siendo
opt-in. Los resultados son observaciones acotadas, no una promesa industrial.
La agrupación con estado acotado y spill-to-disk permanece como siguiente
incremento; aún no forma parte de este contrato publicado.

## Límites nativos y Termux

El backend se compila como C17 nativo con el target `make termux-build` y el
toolchain Clang/Bionic de Termux; eso no implica acceso directo al kernel ni
una API que evada el sistema operativo. El resumen global acota memoria por
registro reutilizado (límite configurable de hasta 64 MiB), cabecera/columnas
(hasta 4096) y acumuladores fijos, sin materializar todas las filas. La API C
`MilenaStreamOptions` permite además fijar `max_rows` y
`max_elapsed_milliseconds`; `max_rows` limita filas procesadas y puede leer un
registro adicional acotado para detectar que se excedió el límite. El presupuesto
de tiempo se comprueba entre registros y puede rebasarse como máximo al terminar
de leer el registro actual (acotado por el límite de registro). La
sintaxis española actual solo expone los límites de registro/columnas y lote:
no se presenta como CPU-capped de forma predeterminada. Este PR no mide un
dispositivo Termux/aarch64 ni promete rendimiento próximo al máximo teórico del
hardware.

El benchmark de flujo limita la carga optativa a 1.000.000 de filas y cada
ejecución de `milena run` tiene un timeout de 180 segundos; CI solo ejecuta las
cargas small/medium. Estos son topes operativos reproducibles, no objetivos de
latencia.
