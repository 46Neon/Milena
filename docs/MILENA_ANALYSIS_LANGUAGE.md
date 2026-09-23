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

En `.analisis` con CSV streaming, `agrupar por "grupo" #spill(...) resumir { ... }` admite exactamente una clave de grupo declarada `texto` y entre 1 y 64 métricas tipadas, en el orden del bloque. La sintaxis de una segunda clave (por ejemplo, `agrupar por "grupo", "periodo"`) se rechaza en parsing antes de abrir el CSV; las claves compuestas texto+número no están implementadas ni cubiertas por la validación de un millón de filas. No confundir la identidad interna compuesta (grupo, métrica) del reducer con una clave de grupo compuesta. Las métricas admitidas son: `suma`, `media`, `minimo`, `maximo` sobre columnas `numerica`, y `contar` sobre una columna declarada. Ejemplo:

```milena
.analisis resumen {
  variable grupo texto
  variable importe numerica
  datos desde "entrada.csv" con filas hasta 1000000 con tiempo hasta 300000 ms
  agrupar por "grupo" #spill("scratch.bin", 4096, 134217728, 4096, 100000, 16777216, 4096)
    resumir { suma de "importe"; media de "importe"; contar de "importe"; }
  guardar resultado en "reporte.json"
}
```

Todas las métricas reutilizan el mismo reducer tipado con identidad de métrica codificada junto a la clave del grupo; se permite que operaciones distintas, por ejemplo `suma`, `media` y `contar`, usen la misma columna numérica. No se crea un reducer independiente ni se materializan las filas. Después de ordenar cada run, los estados parciales repetidos de cada clave compuesta (grupo, métrica) se combinan antes de la fusión externa por pares; por ello cada grupo aparece una sola vez, en orden de bytes, y sus métricas aparecen en el orden declarado, incluso si la agregación residente vació varias veces claves repetidas. La combinación preserva estados tipados y conteos exactos. Cada métrica conserva su propio conteo de válidos, nulos e inválidos. `contar` cuenta celdas no vacías, incluidas cadenas no numéricas; para métricas numéricas, vacío es nulo y texto numérico malformado es inválido. En el resumen de cabecera, `filas_validas` cuenta filas con al menos una métrica válida y `filas_malformadas` filas con al menos una métrica nula o inválida; los dos conteos pueden solaparse. El límite de grupos cuenta claves distintas, no entradas internas grupo-métrica; presupuesto de memoria, cuota scratch, max_key_bytes codificada, grupos, bytes de reporte y runs permanecen explícitos y acotados. La semántica y el planner rechazan antes de ejecutar operaciones/tipos no admitidos. La forma tabular `.agrupar dataset` mantiene su alcance propio y no hereda esta extensión.

> **Composite grouping status:** the language currently accepts one text grouping key only. The binary pair-key codec is a tested internal groundwork item in the PR29 roadmap, not a user-visible feature; `.analisis` continues to reject a second key until canonical AST/planner/runtime/spill integration and end-to-end tests are complete. Numeric composite keys are also pending.
