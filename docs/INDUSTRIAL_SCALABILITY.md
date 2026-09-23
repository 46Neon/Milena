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
