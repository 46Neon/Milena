# Hoja de ruta 2026: abstracción humana y ejecución escalable

Este documento separa el contrato arquitectónico verificable de la visión futura. Milena conserva una sola ruta `lexer -> parser -> AST -> semántica -> runtime`; no se introducen un parser ni un runtime alternativos.

## Estado a 2026-09-22

### Implementado

- Sintaxis de usuario en español y ejecución canónica de arrays, tablas, datasets, estadísticas SST, finanzas iniciales y funciones AST.
- `MilenaExecutionPlan` común para tabla en memoria y CSV streaming, con reportes de ejecución y límites de memoria del flujo.
- En PR25: metadatos de operadores lógicos (tipo y capacidades), validación del contrato y clasificación puramente local de URI. Se reconocen `s3://`, `gs://`, `az://` y `hdfs://` solo para rechazarlos claramente: no hay red ni backend cloud.
- Pruebas deterministas de capacidades, fuentes y regresión de stream/table; Makefile integra ambas nuevas pruebas.

### En progreso (PR25)

- Base reproducible para Big Data: contratos de datos/recursos, benchmarks de streaming y tablas, fixtures, consistencia y memoria acotada.
- Consolidación de documentación y guardas arquitectónicas para impedir rutas paralelas.
- Endurecimiento CI y evaluación de resultados; PR25 no se fusiona sin revisión.

### Futuro, no implementado

- **Plan lógico completo:** filtros, proyecciones, joins, ventanas y ordenamiento como operadores AST/runtime canónicos. El contrato enumera sus propiedades, pero sus backends aún no existen.
- **Paralelismo automático:** particionamiento, planificación de tareas, ejecución concurrente y control de recursos. `parallelizable` describe una posibilidad; no activa hilos ni procesos.
- **Distribución:** coordinador, workers, shuffle, tolerancia a fallos y observabilidad. No existe ejecución distribuida.
- **Adaptadores Spark/Flink:** interfaces de integración futuras, sin dependencia ni integración Spark/Flink actual.
- **Cloud:** conectores autenticados para S3/GCS/Azure/HDFS, credenciales, lectura/escritura y pruebas de integración. Hoy las URI remotas se clasifican y se rechazan sin red.
- **ETL unificado:** lectura multi-formato, schema/evolución, transformaciones y sinks cloud; aún no es producto.
- **ML:** contrato experimental de sugerencias/predicciones solo después de contar con un AST/runtime/backend real y datasets reproducibles. No hay ML oficial ni predicciones simuladas.

## Fases y criterios de aceptación

1. **F0 — núcleo canónico (implementado/parcial):** cada capacidad oficial debe atravesar lexer, parser, AST, semántica y runtime; guardas deben detectar módulos paralelos. Aceptación: pruebas end-to-end y CI verde.
2. **F1 — contrato lógico y datos (PR25):** tipos de operador, capacidades, reportes compatibles y URI tipadas. Aceptación: API anterior compila sin cambios, fuentes locales funcionan, remotas fallan explícitamente sin I/O, tests de stream/table permanecen verdes.
3. **F2 — ETL local:** AST/runtime para selección, filtro, joins y sinks locales; formatos y schemas explícitos. Aceptación: equivalencia streaming/en memoria, límites medidos y fixtures deterministas.
4. **F3 — paralelismo local:** particionado y ejecución concurrente medidos, con determinismo y límites. Aceptación: benchmarks reproducibles, speedup documentado sin prometer latencia fija.
5. **F4 — conectores cloud:** implementar un adaptador por vez, credenciales seguras, retries y pruebas contra emulador/entorno controlado. Aceptación: no se anuncia soporte antes de pruebas reales; cada URI no soportada conserva error claro.
6. **F5 — distribución/adaptadores:** backend-neutral primero; luego adaptadores Spark/Flink opcionales, sin duplicar AST/runtime. Aceptación: plan equivalente, shuffle y fallos observables, documentación honesta.
7. **F6 — ML integrado:** contratos de dataset/modelo y sugerencias/predicciones como capacidades AST/runtime reales. Aceptación: backend ejecutable, métricas y fixtures, reproducibilidad, y separación experimental hasta superar la puerta de producto.

La sintaxis española no cambia para habilitar estas fases: las nuevas capacidades deben añadirse al pipeline canónico, nunca como comandos externos o un segundo intérprete.
