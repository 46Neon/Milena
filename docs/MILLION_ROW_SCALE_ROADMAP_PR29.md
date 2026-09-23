# Hoja de ruta de escala de Milena — PR #29

## Propósito y frontera arquitectónica

El objetivo de esta hoja de ruta es llevar el motor unificado de Milena a cargas
crecientes sin convertir Big Data en una herramienta interna independiente. Toda
capacidad debe recorrer el mismo contrato: **lexer → parser → AST →
semántica/recursos → runtime → backend**. Los backends son implementaciones
internas seleccionadas por el AST/plan tipado; no pueden introducir otro
lenguaje de scripts, parser, CLI de datos ni runtime paralelo.

PR #29 inicia el primer hito ejecutable en `main`: un resumen global de un CSV
determinista de exactamente 1.000.000 de filas por `milena run`, con sintaxis
`.analisis`, búfer de registro acotado y comprobaciones exactas de conteos y
agregados. El reporte observa tiempo, bytes, búfer y RSS pico cuando el sistema
lo permite. La pasada exitosa prueba únicamente esa operación, entrada, build y
hardware; no prueba Big Data arbitrario, agrupación con spill, joins, ETL,
Arrow/Parquet, nube, ejecución distribuida ni ML, y no establece una latencia
universal.

## Relación honesta con PR #28

PR #28 sigue abierto y no forma parte de la base `main` de este PR. No se copia
ni se rehace aquí su implementación. Su head incluye protocolo de resultados,
spill append-only y replay, agregados mergeables, ordenamiento externo y una
primera sección vertical de agrupación con spill conectada al `.agrupar`
existente (con pruebas `milena run` para los casos cubiertos). La propia rama
marca como pendientes optimizar/conectar la reducción externa agrupada al
planner, integrar el spill con la agrupación streaming y completar guardas y
benchmarks/SLO end-to-end. En la consulta de PR #29, la CI activa del head actual
de PR #28 está verde, pero la rama sigue abierta y no está en `main`; sus cambios
se deben revalidar tras cualquier actualización antes de depender de ellos. Esos cambios deben pasar revisión/CI y fusionarse antes de que PR
#29 pueda apoyarse en ellos. El millón de filas de este PR usa el resumen global
CSV streaming que ya existe en `main`; no reivindica cobertura del trabajo de
spill/particionado local de PR #28 ni usa sus APIs nuevas.

Después de fusionar PR #28, esta hoja deberá reconciliarse con el contrato y
las APIs efectivamente integradas; no se debe resolver la dependencia suponiendo
que el head abierto ya está en `main`.

## Fases y criterios de aceptación

Cada fase conserva la ruta canónica y exige pruebas de equivalencia con la
semántica local, límites explícitos de recursos y errores deterministas. Una
fase no se considera terminada por compilar solamente.

### Fase 0 — Contratos y verificación de escala (este PR, parcial)

- Mantener una sola ruta fuente de verdad: lexer → parser → AST → semántica y
  validación de recursos → runtime → backend interno.
- Generar una entrada determinista de 1.000.000 de filas y ejecutarla mediante
  `milena run` y el lenguaje español existente; validar exactamente filas,
  filas válidas/malformadas, conteos de métrica y suma/media esperadas.
- Comprobar que el informe acredita lectura de bytes y que el búfer de registro
  observado está limitado y es menor que el archivo completo. Registrar
  throughput, tiempo de proceso, bytes, capacidad/pico del búfer y RSS máximo
  portable cuando esté disponible; no imponer una cifra de rendimiento/RSS
  universal.
- Añadir un target reproducible de Makefile y un workflow de escala separado,
  manual y programado, fuera de la CI ordinaria.
- Esta fase prueba una operación de agregación global, no una arquitectura
  general de un millón de filas. También ejecuta el mismo programa con el límite
  de filas un registro por debajo del fixture y exige un error sin reporte
  parcial de éxito.

### Fase 1 — Plan lógico y físico unificado

**Estado de PR #29:** se implementa un corte vertical limitado al streaming CSV
actual: `src/query_plan.c` deriva, después de semántica/recursos, una secuencia
lógica CSV-scan → aggregate global/agrupado → JSON-report y elige el backend
físico global o agrupado que consume el runtime canónico. El AST sigue siendo la
fuente de verdad para métricas, fuente y límites. Hay tests unitarios de plan
válido, ambiguo/no soportado y compatibilidad global legacy. Esto no es todavía
un planner general de operadores, esquema, costos, filtros o formatos.

- Modelar lectura, proyección, filtros, agregaciones y límites como nodos
  tipados del AST/plan canónico; resolver nombres y tipos en semántica antes de
  ejecutar.
- Separar explícitamente plan lógico de plan físico sin crear un parser,
  biblioteca de scripts o motor de consulta alterno.
- El plan físico debe elegir entre ejecución secuencial, streaming y operadores
  acotados según capacidades y presupuestos declarados; rechazar planes que no
  tengan soporte, sin fallback oculto.
- Aceptación: pruebas end-to-end vía `milena run` que inspeccionen/validen
  operadores elegidos, cobertura de tipos, errores de recursos y equivalencia
  exacta o tolerancia numérica documentada respecto del camino de referencia.

### Fase 2 — CSV streaming y spill integrados al plan

- Mantener el lector CSV incremental con soporte correcto de comillas y
  registros multilínea; llevar presupuestos de filas, tiempo, columnas,
  registro, memoria y scratch desde sintaxis/AST hasta el backend.
- Integrar spill y merge externos en operaciones del lenguaje únicamente tras
  revisar las APIs y límites realmente fusionados de PR #28. Evitar duplicar su
  almacén, serialización, estados mergeables, sort o reducer.
- Conectar agregación global/agrupada y ejecución por lotes con resultados
  deterministas, cleanup transaccional y fallos por cuota explícitos. No
  materializar el input o todos los grupos para afirmar streaming.
- Aceptación: pruebas `milena run` de equivalencia con y sin spill, claves
  compuestas/tipos soportados según contrato, entradas inválidas y truncadas,
  cuotas de memoria/scratch, reintentos, temporales limpios y conteos de filas;
  benchmarks reproducibles reportan RSS/I/O/throughput sin prometer SLO no
  medidos.

### Fase 3 — Fuentes y formatos columnares

- Definir una abstracción de fuente interna que conserve la misma semántica y
  AST para rutas locales y fuentes remotas/nube; resolver autenticación mediante
  configuración segura, no literales de secretos en scripts/reportes.
- Añadir Arrow/Parquet de forma incremental: tipos y nulos explícitos, schema
  evolution controlada, row groups, compresión, selección de columnas y
  pushdown solo cuando haya equivalencia demostrada.
- Aceptación: fixtures multi-row-group, nulos, tipos, schema mismatch,
  compresión, pushdown equivalente, límites de memoria y casos de error. Medir
  localmente cada backend y documentar formatos/características realmente
  soportados; no presentar nube como disponible antes de su implementación.

### Fase 4 — Operadores ETL en la semántica del lenguaje

- Incorporar lectura/proyección/filtro/cast/normalización/deduplicación,
  agregación y escritura como nodos tipados del mismo AST/runtime. Definir
  claramente si cada operador es streaming, requiere estado acotado o necesita
  materialización/spill.
- Definir políticas explícitas de nulos, conversiones, errores, orden,
  determinismo y escritura atómica; presupuestar cardinalidad y estado.
- Aceptación: programas `.milena` end-to-end, pruebas de bordes y errores,
  equivalencia contra fixtures de referencia, pruebas de memoria/cuotas y
  benchmarks que identifiquen los operadores medidos.

### Fase 5 — Paralelismo local y evaluación distribuida

- Reutilizar planner, particionado, presupuesto, reducción/protocolo y spill
  existentes solo tras confirmar su estado en `main`; integrar operadores CSV
  conscientes de límites de registros, comillas, skew y determinismo.
- Diseñar interfaces de backend remotos sin cambiar la sintaxis, AST ni
  semántica de usuario. Evaluar Spark y Flink como posibles backends/adaptadores
  de ejecución, con decisión documentada basada en interoperabilidad, operación,
  costos, semántica, licencias y benchmarks; la evaluación no equivale a soporte.
- Aceptación local: igualdad determinista secuencial/paralelo, cancelación,
  presupuesto, cleanup, fallo/reintento probado y medición reproducible.
  Aceptación remota: protocolo versionado, autenticación, coordinación,
  reintentos/checkpoints y pruebas reales multiworker antes de llamarlo
  distribuido; establecer SLO solo con medidas p50/p95/p99 bajo carga definida.

### Fase 6 — Integración de ML

- Exponer preparación de features, particionado train/test, entrenamiento,
  evaluación e inferencia como capacidades tipadas del mismo lenguaje/runtime,
  integradas a los planes y fuentes anteriores; no como un `milena-ml` o
  pipeline separado.
- Especificar reproducibilidad/semillas, esquemas, tratamiento de nulos,
  límites de memoria, serialización/versionado de modelos y compatibilidad entre
  entrenamiento e inferencia.
- Aceptación: flujos `.milena` end-to-end, datasets de prueba deterministas,
  métrica y tolerancias documentadas, serialización/relectura de modelos,
  validación de presupuesto y benchmark comparativo. La fase no debe prometer
  modelos o escala no implementados y medidos.

## Ejecución reproducible del hito de este PR

```sh
make clean all
make scale-million-row
# Alternativa con artefacto JSON:
python3 benchmarks/million_row_validation.py --output million-row-results.json
```

El target genera temporalmente el CSV; no deja una fixture masiva en el
repositorio. `make scale-million-row` no forma parte de `make test` y se ejecuta
en el workflow separado `Million-row scale validation` en PRs que cambien los
componentes de escala, manualmente o en su calendario semanal. El workflow
adjunta el JSON del resultado cuando el job termina correctamente. El proceso falla si no se validan los conteos y valores
esperados o si el informe no acredita el búfer acotado.
