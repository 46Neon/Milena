# Arquitectura ejecutable de Milena

## Objetivo

Milena debe tener una sola ruta oficial para ejecutar un programa:

```text
fuente .milena
  -> lexer
  -> parser
  -> AST
  -> análisis semántico
  -> runtime
  -> arreglos, tablas, estadística y reportes
```

El código de compatibilidad puede existir durante la migración, pero no debe
recibir funcionalidades nuevas ni competir con el pipeline canónico.

## Estado actual de las capas

| Capa | Implementación | Estado | Siguiente acción |
|---|---|---|---|
| CLI | `src/main.c` | integrada | Mantener un único punto de entrada |
| Router de scripts | `src/script.c` | compatibilidad | Reducirlo y retirar el parseo textual |
| Lexer | `src/lexer.c` | integrada parcialmente | Validar límites y diagnósticos |
| Parser | `src/parser.c` | integrada parcialmente | Cubrir datasets, arrays y exportación |
| AST | `src/ast.c` | integrada parcialmente | Convertir todas las operaciones en nodos |
| Runtime de arrays | `src/language_runtime.c` | integración inicial | Añadir tablas y transformaciones |
| Arrays numéricos | `src/array.c` | integrada | Mantener como backend numérico |
| Tablas | `src/table.c` y `src/dataset.c` | dos APIs | Elegir un modelo canónico |
| Estadística | `src/sst_*.c` | biblioteca | Exponerla mediante AST y builtins |
| IR y VM | `src/ir.c`, `src/vm.c` | experimental | No usar hasta estabilizar el intérprete |
| Modelos | `src/forest.c` | experimental | Integrar solo con sintaxis y tests estables |

## Regla de integración

Una capacidad solo se considera parte del lenguaje cuando cumple las cuatro
condiciones siguientes:

1. Tiene sintaxis documentada.
2. El parser la representa en el AST.
3. El runtime la ejecuta mediante una API estable.
4. Tiene al menos una prueba de integración ejecutada por CI.

Una biblioteca C que no cumpla estas condiciones debe marcarse como
experimental o de compatibilidad; no debe presentarse como una característica
completa del lenguaje.

## Política de migración

- `milena run archivo.milena` debe migrar progresivamente al pipeline canónico.
- La compatibilidad heredada debe aislarse y no ampliar su parser textual.
- Las tablas, los arrays y los valores escalares deben converger en un modelo
  de valores común.
- La VM, el bytecode y el GC quedan pospuestos hasta que el intérprete basado
  en AST tenga paridad funcional.
- Cada cambio de integración debe añadir o actualizar una prueba end-to-end.

La clasificación de fuentes se comprueba con:

```bash
make check-source-manifest
```

Si se añade una fuente C sin decidir su responsabilidad, la validación falla.
Esto evita que el proyecto acumule módulos sin dueño arquitectónico.

La frontera con las implementaciones experimentales se comprueba además con:

```bash
make check-experimental-isolation
```

Esta validación garantiza que `SOURCES` no enlace arena, IR, compilador,
ensamblador, VM, GC, módulos alternativos ni bosque, y que las capas del
producto no incluyan sus headers. Estos módulos pueden compilarse en pruebas
específicas, pero no pueden convertirse accidentalmente en una ruta oficial.

## Puerta de unificación

Mientras la ruta canónica no tenga paridad con las capacidades que se desean
conservar, el proyecto permanece en fase de unificación. Durante esta fase:

- no se añaden capacidades nuevas al parser textual, a APIs paralelas ni a
  módulos aislados;
- una implementación nueva solo puede entrar mediante el lexer, parser, AST,
  semántica y runtime canónicos;
- las bibliotecas existentes se adaptan al runtime común o se declaran
  experimentales y quedan fuera de la superficie oficial;
- los ejemplos y pruebas nuevas deben ejecutar `milena run` sobre el mismo
  pipeline que usará el producto final.

La fase de nuevas capacidades comienza únicamente cuando todos los componentes
conservados tengan una ruta unificada, una API de valores común y pruebas de
integración. A partir de ese momento, cada capacidad nueva debe ampliar el AST,
el runtime y la suite común; nunca debe crear otro parser o ejecutor paralelo.
