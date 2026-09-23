# Escalabilidad industrial: medición antes que promesas

El reporte de streaming incluye `bytes_leidos` para que los benchmarks puedan
relacionar volumen físico y tiempo observado. PR #27 no fija un umbral de
rendimiento: establece la telemetría mínima para comparar ejecuciones de forma
reproducible.

Toda comparación debe guardar el commit de Milena, compilador y flags,
arquitectura, sistema operativo, tamaño y contenido del CSV, almacenamiento,
filas, bytes, errores, tiempo y límites configurados.

Una cifra aislada no demuestra escalabilidad industrial. La siguiente fase
deberá añadir fixtures grandes deterministas, ejecuciones repetidas,
medición de memoria residente, comparación entre tamaños y pruebas en más de
una arquitectura.
