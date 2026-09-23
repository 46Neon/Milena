# Benchmarks reproducibles de Milena

Estos benchmarks miden, sin prometer un umbral, dos operaciones observables:

1. **Compilación:** `make clean && make -B all`, empezando cada muestra con el
   árbol limpio.
2. **Ejecución:** `./milena run benchmarks/ejecucion.milena`, que recorre la
   ruta canónica lexer → parser → AST → semántica → runtime. Esta fixture mide
   el programa de arreglos; no sustituye una medición de tablas o datasets.

La fixture de arrays es deliberadamente pequeña. Para CSV, PR25 ofrece
`stream_benchmark.py` (resumen global) y `grouped_stream_benchmark.py`
(agrupación acotada), ambos con cargas deterministas `small` (100 filas) y
`medium` (10.000 filas). La carga `large` se habilita explícitamente con
`--large-rows N` (hasta 1.000.000 filas; timeout de 180 s). Cada muestra se
ejecuta con `milena run` y la sintaxis española del AST; no hay un ejecutable
de datos paralelo. Se validan filas, grupos, datos inválidos y bytes, y se
reportan tiempo de pared, filas/s, MB/s, límites de estado, búfer, plataforma
y commit. No hay umbrales ni tiempos fijos: los resultados no permiten
extrapolar rendimiento industrial, Termux, Android, aarch64 o un entorno no
medido. La agrupación conserva estado en memoria; spill-to-disk y particionado
CSV todavía no están implementados ni se miden aquí.

## Uso

Desde la raíz del repositorio:

```bash
python3 benchmarks/benchmark.py
# Más muestras y un archivo JSON reproducible para adjuntar al informe:
python3 benchmarks/benchmark.py --compile-repetitions 3 --run-repetitions 10 \
  --output benchmark-results.json
# Resumen global CSV (small + medium; CI usa este camino):
make benchmark-stream
# Agrupación streaming, memoria acotada (small + medium; CI la valida):
make benchmark-stream-grouped
# Cargas grandes explícitas y opt-in:
python3 benchmarks/stream_benchmark.py --large-rows 1000000 --output stream-results.json
python3 benchmarks/grouped_stream_benchmark.py --large-rows 1000000 --output grouped-results.json
```

El resultado JSON incluye muestras, mínimo, mediana, media, máximo, fixture,
comandos, plataforma, arquitectura y commit si `GITHUB_SHA` está disponible.
El reloj es `time.perf_counter()` y mide tiempo de pared del proceso completo;
se deben conservar los resultados junto con el compilador, flags, carga del
sistema y commit. Los benchmarks no forman parte del binario ni del paquete
Termux.

## Hito de un millón de filas (opt-in)

`make scale-million-row` genera un CSV determinista de exactamente 1.000.000
filas y ejecuta un resumen global mediante `milena run` y la sintaxis española
canónica. Valida filas leídas/válidas/malformadas, suma/media/conteo, bytes de
entrada y límites de registro/búfer; comprueba además que un límite de filas
infractor falla sin producir un reporte parcial de éxito. Informa throughput,
tiempo, capacidad y
pico del búfer y RSS pico portable cuando Python/el sistema lo soportan. Los
conteos son exactos; los agregados de punto flotante se comparan con tolerancia
numérica estrecha. El resultado es una observación de ese workload/build/hardware,
no un umbral, una latencia garantizada ni evidencia de Big Data arbitrario,
spill, ejecución distribuida, cloud o ML.

Este target no se incorpora a `make test`. El workflow independiente
`.github/workflows/million-row-scale.yml` lo ejecuta en PRs que cambien los
componentes de escala, bajo demanda y semanalmente, y publica el JSON como
artefacto. La especificación de fases y dependencias está
en [`MILLION_ROW_SCALE_ROADMAP_PR29.md`](../docs/MILLION_ROW_SCALE_ROADMAP_PR29.md).
