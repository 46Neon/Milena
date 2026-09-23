# Observabilidad para escalabilidad industrial

PR #27 añade `bytes_leidos` al reporte del flujo CSV y a
`MilenaStreamReport`. Una ejecución de datos de producción debe poder
responder no solo cuántas filas procesó, sino qué volumen físico recorrió y
qué relación existe entre bytes, filas, tiempo y errores.

El campo se informa junto a filas leídas y válidas, filas malformadas, pico
del registro reutilizado, límites de registro y columnas, tiempo de pared y
resultados numéricos.

## Interpretación operativa

Un orquestador puede conservar este reporte como evidencia de ejecución y
calcular posteriormente:

```text
bytes_por_segundo = bytes_leidos / (tiempo_ms / 1000)
filas_por_segundo = filas / (tiempo_ms / 1000)
```

No se deben comparar resultados de distintas máquinas sin conservar también
la versión, arquitectura, compilador, flags y tipo de almacenamiento.

La ruta continúa siendo una pasada, con memoria acotada y sin materializar el
dataset completo. El campo no implica todavía particionado, paralelismo,
spill-to-disk ni procesamiento distribuido.
