# Ejecución en flujo para grandes CSV

PR24 ofrece una ruta canónica y acotada para resumir CSV grandes sin
materializar el dataset completo. La entrada sigue el recorrido lexer → parser
→ AST → semántica → runtime; no hay un intérprete textual paralelo.

## Sintaxis humana recomendada

```milena
.analisis ventas_masivas {
    datos desde "datos/ventas.csv"
        procesar por lotes de 4096 filas
        con registros de hasta 8 MiB

    resumir {
        suma de "importe";
        media de "importe";
        minimo de "importe";
        maximo de "importe";
        contar de "importe";
        varianza de "importe";
        desviacion_estandar de "importe";
    }

    guardar resultado en "reporte_flujo.json"
}
```

`procesar por lotes de N filas` expresa el tamaño de lectura lógico. El motor
no conserva el lote completo: reutiliza un registro CSV, la cabecera, los
punteros de campos y un acumulador por métrica. `con registros de hasta N MiB`
es opcional; el límite duro del runtime es 64 MiB por registro y el valor
predeterminado también es 64 MiB. El tamaño mínimo aceptado es 4096 bytes.
Estos límites permiten fallar pronto con un diagnóstico explícito en lugar de
reservar memoria sin cota. La salida incluye el límite configurado, el número
de columnas de la cabecera y el pico de búfer observado.

La forma anterior de PR24 continúa funcionando:

```milena
dataset cargar flujo("datos/ventas.csv", 4096)
.resumir dataset { #suma("importe") #media("importe") #conteo("importe") }
.exportar { ("reporte_flujo.json") }
```

En la forma legacy, el segundo argumento es opcional y el valor predeterminado
es 4096 filas.

## Qué hace

- Lee el CSV secuencialmente con un búfer de E/S de 64 KiB.
- Soporta registros entrecomillados, comillas escapadas y registros grandes
  hasta el límite configurado.
- Mantiene acumuladores de suma compensada, media, mínimo, máximo, conteo y
  varianza/desviación estándar en una pasada.
- Rechaza columnas inexistentes, demasiadas columnas (máximo 4096), registros
  que exceden el límite y CSV con comillas sin cerrar.
- Reporta filas leídas, filas válidas, filas malformadas, límite de registro,
  límite de columnas, pico de búfer y tiempo observado.
- No crea `Dataset`, `MilenaTable` ni una copia de todas las filas.

## Límites deliberados

El flujo actual admite resúmenes numéricos globales. No admite mediana,
percentiles, joins, limpieza que necesite observar todo el conjunto,
transformaciones materializadas ni agrupaciones ilimitadas. Es preferible
rechazar esas operaciones antes que fingir que son streaming y desbordar la
memoria. Tampoco ofrece procesamiento distribuido, spill a disco, reanudación,
compresión ni garantías de latencia fija.

El tiempo en milisegundos se mide y se informa únicamente como observabilidad;
no se promete una latencia fija. Leer un archivo grande cuesta en proporción a
sus filas y depende del disco, sistema operativo, tamaño de los registros y
hardware. La garantía práctica de esta ruta es memoria acotada por
`O(columnas + métricas + registro máximo)` y una sola pasada, no una cifra de
milisegundos.

## Contrato de rendimiento

Las mejoras posteriores deben conservar:

- cero copias del dataset completo;
- una pasada cuando la operación lo permita;
- acumuladores numéricamente estables;
- límites configurables con tope duro y errores accionables;
- mediciones reproducibles de filas, límites, tiempo y errores;
- pruebas con archivos pequeños, filas inválidas, comillas y registros que
  superen el límite.
