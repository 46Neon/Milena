# Lenguaje de análisis de Milena

La superficie oficial de análisis usa una única cadena lexer → parser → AST → semántica → runtime. Los comandos SST reciben una lista de argumentos dentro de una sola cadena para mantener la sintaxis actual del AST.

## Comandos

```milena
#perfil_avanzado("columna")
#histograma("columna")
#normalidad("columna")
#tasa("eventos,exposicion,factor")
#poisson("eventos,exposicion,factor")
#correlacion("columna_x,columna_y")
#wilcoxon("antes,despues")
#chi_cuadrado("fila,columna")
#riesgo("exposicion,evento,categoria_expuesta,categoria_evento")
#modelo_sst("area,severidad,cargo")
#interes_simple("principal,tasa,periodos")
```

`#riesgo` requiere categorías positivas explícitas; el resultado ya no depende del orden accidental de las filas. Las columnas se resuelven contra la `MilenaTable` que produce el análisis. `#interes_simple` usa las operaciones decimales de finanzas sobre columnas numéricas de esa misma tabla.


### Predicados tipados en análisis en flujo

En `.analisis` con una fuente `datos desde` en modo streaming se permite un solo filtro: igualdad textual exacta (`filtrar "region" == "Norte";`, columna declarada `texto`) o comparación numérica estricta mayor que (`filtrar "importe" > 10;`, columna declarada `numerica`). Para la comparación numérica, el umbral debe ser un literal finito; los campos CSV vacíos, nulos, no numéricos, con texto extra o no finitos no coinciden y no llegan a la agregación. El valor exactamente igual al umbral tampoco coincide. La columna debe estar declarada con el tipo correcto y existir en la cabecera CSV. Los filtros se ejecutan antes de agregación global, agrupación en memoria y agrupación con spill mediante el pipeline canónico; otras formas de predicado se rechazan.
