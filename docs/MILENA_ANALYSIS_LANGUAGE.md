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


### Agrupación streaming con spill y métricas múltiples

En `.analisis` con CSV streaming, la agrupación conserva la forma histórica de una sola clave (`agrupar por "grupo"`) y admite hasta 64 métricas tipadas. El spill permite además una o dos claves de grupo declaradas `texto`; una clave compuesta solo se admite con `#spill`, y ambas columnas deben estar declaradas, ser distintas y existir en la cabecera CSV. Una solicitud de varias claves sin spill, una tercera clave, columnas desconocidas o claves de otro tipo se rechaza antes de ejecutar/abrir los datos. El adaptador tabular `.agrupar dataset` conserva su alcance separado.

```milena
.analisis resumen_compuesto {
  variable region texto
  variable segmento texto
  variable importe numerica
  datos desde "entrada.csv" con filas hasta 1000000 con tiempo hasta 300000 ms
  agrupar por "region", "segmento" #spill("scratch.bin", 4096, 134217728, 4096, 100000, 16777216, 4096)
    resumir { suma de "importe"; contar de "importe"; }
  guardar resultado en "reporte.json"
}
```

El spill de una o dos claves texto reutiliza el mismo reducer tipado, identidad de métrica, ordenamiento externo y publicación atómica. Para una clave el esquema JSON existente (`clave`) no cambia; con dos claves el encabezado incluye `columnas_grupo` y cada resultado lleva `claves`, una lista de componentes `{nombre,tipo,valor,valido}` en el orden declarado. Las longitudes/type tags versionados mantienen distintos los límites entre componentes, incluso con comas, comillas o separadores en el texto; el texto vacío válido es distinto de un valor inválido/nulo. Los resultados compuestos son deterministas y se ordenan por la codificación completa de la clave. Las métricas aparecen en orden declarado: `suma`, `media`, `minimo`, `maximo` requieren columnas `numerica`; `contar` admite una columna declarada y cuenta celdas no vacías. Se permiten distintas operaciones sobre una misma columna.

Cada métrica conserva conteos tipados de válidos, nulos e inválidos. Para métricas numéricas, vacío es nulo y texto malformado es inválido; `contar` cuenta campos no vacíos, incluso no numéricos. El límite de grupos cuenta pares de valores distintos, no entradas internas grupo-métrica. Presupuestos de memoria, cuota scratch, bytes máximos de clave codificada, grupos, bytes de reporte y runs siguen explícitos y acotados; en error se eliminan temporales y no se publica un reporte parcial. Esta extensión solo cubre dos claves TEXT en el backend CSV de spill; claves compuestas numéricas, agrupación compuesta sin spill y extensión tabular permanecen fuera de alcance. La validación de un millón de filas sigue siendo la existente para workload de una clave y no se extrapola a claves compuestas ni a escalas arbitrarias.

