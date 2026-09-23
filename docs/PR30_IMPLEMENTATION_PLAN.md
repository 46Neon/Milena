# Plan de trabajo — PR #30

## Propósito y reglas de ejecución

PR #30 desarrollará capacidades de datos grandes de Milena sin crear un runtime, parser o CLI paralelo y sin depender de Python o R. Toda funcionalidad de producto debe recorrer la ruta canónica:

`lexer → parser → AST tipado → semántica y límites → planner lógico/físico → runtime Milena → API C del motor → backend`

Se trabajará en la misma rama de PR #30, en el orden indicado. Cada etapa debe tener una vertical completa, pruebas reproducibles, límites de recursos y documentación antes de declarar su salida o avanzar a la siguiente. Los prototipos de investigación no cuentan como producto. La PR permanece abierta y sin fusionar hasta completar validaciones, resolver conversaciones y obtener las aprobaciones requeridas; no se deshabilitan protecciones ni se fuerza el historial.

## Secuencia y estado

| Orden | Etapa | Resultado que debe quedar en PR #30 | Estado |
|---|---|---|---|
| 0 | Integración y auditoría de PR #29 sobre `main` | Planner único, contratos preservados, CI del head exacto aprobada y límites registrados con evidencia. | CI del head remoto exacto `b0d3de8bfc001f46728b2ec0526d548368535603` reportado como 12/12 checks aprobados; el gate físico Termux/AArch64 y la revisión/aprobación del colaborador siguen pendientes. No equivale a cerrar o fusionar la PR. |
| 1 | Fase 3 — fuentes columnares y ORM SQL | Una lectura/escritura columnar local end-to-end y una vertical SQL tipada end-to-end, ambas desde el lenguaje y con pruebas. | Arrow IPC STREAM tiene una implementación candidata en la ruta canónica; compilación local con Zig, pruebas del backend/runtime, E2E de CLI, ASan/UBSan y lectura independiente PyArrow pasaron. Esto no sustituye GCC/Clang CI sobre el SHA publicado, pruebas de Windows ni Termux/AArch64 físico. La cota de bytes decodificados se valida después del decode síncrono (no limita estrictamente RSS); no hay preempción de decode ni señal de cancelación conectada al CLI. SQL/ORM sigue pendiente, por lo que Fase 3 no está completa ni se declara soporte Arrow. |
| 2 | Fase 4 — ETL, OLAP y búsqueda | Pipeline de operadores tipados que procesa una fuente real, produce resultados reproducibles y aplica presupuestos/backpressure. | Pendiente de cerrar Fase 3. |
| 3 | Fase 5 — paralelismo local y sockets | Ejecución paralela acotada con particiones seguras y una comunicación real entre procesos por sockets. | Pendiente de cerrar Fase 4. |
| 4 | Fase 6 — ML integrado | Entrenamiento y predicción desde Milena, modelo serializado con esquema y controles de recursos. | Pendiente de cerrar Fase 5. |

Las fechas, resultados y límites se actualizarán con el SHA exacto y el enlace de CI que los demuestre; no se inferirán desde otra rama o commit.

## Etapa 0 — Integración y auditoría

### Trabajo

- Mantener una sola declaración y autoridad de `MilenaStreamExecutionPlan`; integrar el plan físico de CSV en el planner derivado del AST, sin conservar un ejecutor alternativo.
- Integrar deliberadamente ambos lados de los conflictos y conservar las salvaguardas vigentes de `main`, incluyendo límites del join, flush terminal sin reintento duplicador, scratch/output transaccional y offsets de archivo de 64 bits.
- Mantener lectura secuencial por registros CSV completos; no dividir CSV entrecomillado o multilínea por rangos arbitrarios de bytes.
- Ejecutar CI sobre el SHA final: GCC, Clang, ASan/UBSan, paquete Linux, Windows y las pruebas de paridad/escala aplicables. El gate real de 10 GB de Linux y Windows debe aprobar con el archivo completamente leído y spill real observado.
- Ejecutar el gate real de Termux/AArch64 en dispositivo o runner físico habilitado; si no existe acceso, registrarlo como bloqueador pendiente sin simularlo.

### Salida

La Etapa 0 no sale por compilar solamente: necesita suite heredada aprobada, pruebas de la integración y reporte de escala del SHA exacto. No se declara aprobado un gate por resultados del main anterior o del head histórico de PR #29.

## Fase 3 — formatos columnares y ORM SQL

### 3A. Auditoría de contratos y dependencias nativas

- Revisar la API C existente (`MilenaTable`, arrays, schema, `MilenaSourceReader`, runtime, Makefile, empaquetado Windows/Linux/Termux) antes de fijar formatos y sintaxis.
- Separar interfaz pública de fuente columnar, batches/record batches y driver SQL de la implementación del backend.
- Evaluar solo dependencias nativas de C/C++ para estándares de formato; prohibido delegar el motor o la ejecución a Python/R. Fijar versiones, licencias, enlaces estáticos/dinámicos y disponibilidad en los tres targets antes de incorporar cualquier biblioteca.
- Si una dependencia no puede construirse y probarse en las plataformas objetivo, mantener esa capacidad pendiente: no dejar un stub con apariencia de soporte.

### 3B. Vertical columnar local

- Elegir explícitamente el primer formato soportado (Arrow IPC o Parquet) a partir de la auditoría, documentando por qué y qué versión/perfil se admite. Extender después a otro formato solo tras aprobar el primero.
- Añadir nodos y atributos AST tipados para fuente, esquema/proyección y límites de bytes, filas, columnas, batch y tiempo; validarlos antes de abrir el archivo.
- Implementar una vertical de lenguaje completa: abrir archivo columnar local → leer batches/row groups → proyectar/filtrar o agregar con semántica Milena → escribir resultado o reporte con publicación atómica.
- Definir ownership, nulls/validity, tipos enteros y overflow, conversión decimal/floating-point, UTF-8, metadata/versiones, errores de corrupción y cierre de recursos.
- Probar archivos vacíos, esquemas incompatibles, columnas ausentes, nulls, int64 >2^53, overflow, multi-batch/multi-row-group, fallos de I/O, cancelación, límites y limpieza. Probar offsets mayores de 2 GiB cuando el formato/plataforma lo permita.

### 3C. Vertical SQL/ORM local

- Definir un AST/plan tipado de conexión, tabla, proyección, filtros, parámetros y límites; no construir SQL concatenando valores del usuario.
- Implementar el primer driver local (SQLite si la auditoría de dependencias y empaquetado lo permite), usando prepared statements y bindings tipados dentro del backend Milena.
- Completar una vertical end-to-end: esquema tipado → consulta parametrizada → resultados como batches/tabla Milena → actualización/insert con transacción y rollback ante error.
- Fijar límites por consulta/filas/bytes/tiempo, timeout y cancelación; mantener secretos fuera de logs y diagnósticos.
- Probar injection por parámetros, NULL, int64, texto/UTF-8, columnas desconocidas, transacción incompleta, concurrencia, timeout, cancelación, rollback y ownership/cierre del driver.

### Gate de Fase 3

Ambas verticales deben estar conectadas al AST/planner/runtime canónicos, tener al menos una prueba E2E ejecutable desde la CLI del lenguaje, pruebas de límites y errores, integración de build/paquetes y documentación de capacidades/limitaciones. No se acepta solo una API C, una demo, lectura metadata-only ni archivos fixture falsos.

## Fase 4 — ETL, OLAP y búsqueda

- Diseñar operadores tipados para transformar, proyectar, filtrar, agrupar y combinar fuentes sin materializar todo salvo que la semántica lo requiera explícitamente.
- Construir una vertical OLAP y una vertical de búsqueda sobre datos reales y el planner común; identificar claramente cuáles operadores son streaming y cuáles retienen estado.
- Añadir presupuesto por fuente/consulta/operador, scratch, cola y salida; backpressure; cancelación; staging y publicación atómica. Distinguir memoria residente de scratch en disco.
- Rechazar expresiones/tipos no soportados durante semántica/planning, antes de abrir fuentes o publicar resultados.
- Probar equivalencia de resultados, orden determinista cuando aplique, nulls, cardinalidades, límites, spill, I/O, concurrencia, cancelación y limpieza. Medir sin prometer aceleración universal.

### Gate de Fase 4

Cada operación anunciada debe atravesar sintaxis española → AST → plan → ejecución real y tener una vertical E2E, contrato de recursos, tests de fallo y documentación. No se anuncia una suite ETL/OLAP/search completa por implementar un solo operador.

## Fase 5 — paralelismo local y sockets reales

- Introducir un pool local con máximo explícito de workers, cola acotada y backpressure. Mantener determinismo, ownership y cancelación/fallo cooperativo.
- Particionar solo en límites semánticamente seguros: row groups/batches columnar u otra estrategia con pruebas. CSV permanece secuencial por registros completos mientras no exista un particionador CSV-aware probado.
- Implementar una vertical de comunicación real entre procesos por sockets, con framing versionado, tamaños máximos, timeouts, errores de desconexión y cleanup; no simular sockets con llamadas in-process.
- Limitar conexiones, colas, bytes en tránsito, workers y scratch. En fallo/cancelación, limpiar recursos, impedir publicar salidas parciales y no reintentar una escritura parcial como si fuera idempotente.
- Probar condiciones de carrera, concurrencia, backpressure, disconnect, payload inválido, overflow, límites, cancelación, resultados deterministas y cierre de sockets.

### Gate de Fase 5

Prueba E2E de trabajo repartido en procesos reales y sockets, además de pool local; repetición bajo carga y sanitizers; límites y diagnóstico documentados. Una opción `--workers` sin trabajo concurrente probado no cuenta.

## Fase 6 — ML integrado

- Auditar el código nativo de ML existente y su estado en el build; integrar o completar algoritmos detrás de APIs tipadas del runtime, sin puente Python/R.
- Implementar como mínimo una vertical completa de carga/preparación → entrenamiento → evaluación → predicción → persistencia versionada del modelo y uso posterior desde Milena.
- Definir tipos/esquemas de features, tratamiento de nulls, overflow numérico, partición/split determinista, límites de filas/columnas/bytes/tiempo/memoria/threads y compatibilidad de modelo.
- Probar dataset pequeño de referencia, estabilidad determinista, datos inválidos, modelo corrupto/incompatible, cancelación, límites, ownership y limpieza.
- Documentar métricas de calidad observadas por dataset/seed; no prometer precisión ni rendimiento general.

### Gate de Fase 6

Entrenamiento y predicción funcionales desde el lenguaje y la API C canónica, modelo recuperable con versionado/schema, pruebas de calidad y fallos, documentación de límites y CI por plataforma. Un clasificador aislado que no pase por el runtime no cuenta.

## Contrato transversal de recursos

Cada fuente y operador nuevo debe especificar presupuestos de bytes/filas/columnas/batches/keys/filas de salida/tiempo; estado residente frente a scratch; ownership; comportamiento bajo overflow; backpressure; diagnóstico; cancelación; fallo de I/O; publicación atómica; cierre y limpieza. Las pruebas deben forzar límites, cancelación, error durante escritura, concurrencia y limpieza. Todo valor de escala publicado debe indicar SHA, plataforma, workload y artefacto de CI.

## Estado de entrega y cierre

- La PR #30 permanece draft y no se fusiona antes de completar las etapas, validar el SHA final, resolver comentarios y obtener aprobación requerida.
- No crear PRs auxiliares ni mover la base sin motivo; no hacer force-push, desactivar protecciones ni presentar trabajos parciales como capacidad industrial.
- Mantener registradas las validaciones pendientes (en especial firma de commits requerida por `main` y gate físico Termux/AArch64) sin ocultarlas ni sustituirlas por emulación.
- Al terminar, entregar resumen de capacidades realmente implementadas, pruebas y SHA/artefactos que las respaldan, límites que permanecen y bloqueos de aprobación/firma. No marcar la tarea completa mientras queden gates requeridos pendientes.
