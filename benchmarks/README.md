# Benchmarks reproducibles de Milena

Estos benchmarks miden, sin prometer un umbral, dos operaciones observables:

1. **Compilación:** `make clean && make -B all`, empezando cada muestra con el
   árbol limpio.
2. **Ejecución:** `./milena run benchmarks/ejecucion.milena`, que recorre la
   ruta canónica lexer → parser → AST → semántica → runtime → MilenaTable.

La fixture es deliberadamente pequeña y determinista. No representa todos los
volúmenes ni todas las operaciones de análisis; los resultados no permiten
extrapolar rendimiento industrial, Termux, Android, aarch64 o cualquier otro
entorno no medido.

## Uso

Desde la raíz del repositorio:

```bash
python3 benchmarks/benchmark.py
# Más muestras y un archivo JSON reproducible para adjuntar al informe:
python3 benchmarks/benchmark.py --compile-repetitions 3 --run-repetitions 10 \
  --output benchmark-results.json
```

El resultado JSON incluye muestras, mínimo, mediana, media, máximo, fixture,
comandos, plataforma, arquitectura y commit si `GITHUB_SHA` está disponible.
El reloj es `time.perf_counter()` y mide tiempo de pared del proceso completo;
se deben conservar los resultados junto con el compilador, flags, carga del
sistema y commit. Los benchmarks no forman parte del binario ni del paquete
Termux.
