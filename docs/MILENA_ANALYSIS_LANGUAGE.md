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
```

`#riesgo` requiere categorías positivas explícitas; el resultado ya no depende del orden accidental de las filas. Las columnas se resuelven contra la `MilenaTable` que produce el análisis.
