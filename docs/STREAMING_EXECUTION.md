# Ejecución en flujo para grandes CSV

PR24 añade una ruta de ejecución acotada para análisis de resúmenes sobre CSV
que no necesitan materializar todo el dataset.

## Sintaxis

```milena
.analisis ventas_masivas {
    variable importe numerica
    dataset cargar flujo("datos/ventas.csv", 4096)

    .resumir dataset {
        #suma("importe")
        #media("importe")
        #minimo("importe")
        #maximo("importe")
        #conteo("importe")
        #varianza("importe")
        #desviacion_estandar("importe")
    }

    .exportar {
        ("reporte_flujo.json")
    }
}
```

El segundo argumento es el tamaño lógico del lote y es opcional. Si se omite,
se utiliza `4096`. El lote no se conserva completo: el motor mantiene un
registro CSV reutilizable y acumuladores numéricos. Por eso el uso de memoria
es acotado por el tamaño del registro, el encabezado y las métricas solicitadas,
no por el número de filas.

## Qué hace

- Lee el CSV secuencialmente con un búfer de E/S de 64 KiB.
- Mantiene acumuladores de suma compensada, media, mínimo, máximo, conteo y
  varianza en una pasada.
- Rechaza columnas inexistentes y reporta filas con campos no numéricos.
- Escribe un JSON con filas procesadas, filas válidas, filas malformadas,
  tamaño de lote, tiempo medido y resultados.
- No crea `Dataset`, `MilenaTable` ni una copia de todas las filas.

## Límites deliberados

La primera ruta de flujo admite resúmenes numéricos sin agrupación. No admite
mediana, percentiles, joins, limpieza que necesite observar todo el conjunto,
ni transformaciones que generen columnas materializadas. Es preferible rechazar
esas operaciones que fingir que son streaming y volver a consumir memoria sin
control.

El tiempo en milisegundos se mide y se informa, pero no se promete una latencia
fija: leer un archivo grande siempre tiene un coste proporcional a sus filas y
depende del disco, el sistema operativo, el tamaño de los registros y el
hardware. La optimización de PR24 consiste en una sola pasada, poca memoria,
bajo overhead de asignación y acumuladores O(1), no en una garantía física de
milisegundos para cualquier volumen.

## Contrato de rendimiento

Para mantener esta ruta honesta, las mejoras posteriores deben conservar:

- memoria O(columnas + métricas + registro máximo);
- cero copias del dataset completo;
- una pasada cuando la operación lo permita;
- acumuladores numéricamente estables;
- mediciones reproducibles de filas, tiempo y errores;
- pruebas con archivos pequeños, filas inválidas y registros CSV entrecomillados.
