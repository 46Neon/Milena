# Benchmarks reproducibles de Milena

Estos benchmarks miden, sin prometer un umbral, dos operaciones observables:

1. **Compilación:** `make clean && make -B all`, empezando cada muestra con el
   árbol limpio.
2. **Ejecución:** `./milena run benchmarks/ejecucion.milena`, que recorre la
   ruta canónica lexer → parser → AST → semántica → runtime. Esta fixture mide
   el programa de arreglos; no sustituye una medición de tablas o datasets.

La fixture de arrays es deliberadamente pequeña. Para flujo CSV, PR25 añade
`stream_benchmark.py`, que genera de forma determinista cargas `small` (100
filas), `medium` (10.000) y una carga `large` configurable solo con
`--large-rows N`. Cada ejecución pasa por `milena run` y por la sintaxis
española del AST; no existe un ejecutable de datos paralelo. Se reportan filas,
filas válidas/malformadas, bytes, tiempo de pared, filas/s, MB/s, lote, búfer
observado, plataforma y commit. No hay umbrales ni tiempos fijos: los
resultados no permiten extrapolar rendimiento industrial, Termux, Android,
aarch64 o cualquier entorno no medido.

## Uso

Desde la raíz del repositorio:

```bash
python3 benchmarks/benchmark.py
# Más muestras y un archivo JSON reproducible para adjuntar al informe:
python3 benchmarks/benchmark.py --compile-repetitions 3 --run-repetitions 10 \
  --output benchmark-results.json
# Flujo CSV (small + medium; CI usa este camino):
make benchmark-stream
# Carga grande explícita y opt-in:
python3 benchmarks/stream_benchmark.py --large-rows 1000000 --output stream-results.json
```

El resultado JSON incluye muestras, mínimo, mediana, media, máximo, fixture,
comandos, plataforma, arquitectura y commit si `GITHUB_SHA` está disponible.
El reloj es `time.perf_counter()` y mide tiempo de pared del proceso completo;
se deben conservar los resultados junto con el compilador, flags, carga del
sistema y commit. Los benchmarks no forman parte del binario ni del paquete
Termux.
