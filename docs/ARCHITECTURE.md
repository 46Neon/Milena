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
| CLI | `src/main.c` + `src/entrypoints.c` | adaptador canónico | Conservar formatos históricos y retirar backends duplicados gradualmente |
| Router de scripts | `src/script.c` | frontend AST + compatibilidad explícita | Retirar solo fallback cuando exista paridad comprobada |
| Lexer | `src/lexer.c` | integrada parcialmente | Validar límites y diagnósticos |
| Parser | `src/parser.c` | integrada parcialmente | Cubrir datasets, arrays y exportación |
| AST | `src/ast.c` | integrada parcialmente | Convertir todas las operaciones en nodos |
| Runtime de arrays | `src/language_runtime.c` | integración inicial | Añadir tablas y transformaciones |
| Arrays numéricos | `src/array.c` | integrada | Mantener como backend numérico |
| Tablas | `src/table.c` (canónica) y `src/dataset.c` (adaptador) | unificada con compatibilidad explícita | Mantener `Dataset` solo en las fronteras heredadas |
| Frontera de compilador | `src/canonical_compiler.c` | adaptador canónico | Entrega AST + `MilenaTable` prestada sin enlazar IR/VM |
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

La auditoría detallada de entradas, backends y fronteras pendientes está en
[`UNIFICATION_AUDIT_PR24.md`](UNIFICATION_AUDIT_PR24.md). Las entradas CLI
históricas pasan por `entrypoints.c`, que construye y valida un AST mínimo
antes de delegar al backend que conserva el formato público. La prueba
estructural `check_unification_architecture.py` impide que `main.c` vuelva a
contener lógica de Dataset o análisis.

`canonical_compiler.h` y `src/canonical_compiler.c` son la frontera segura para
la siguiente fase. Parsean y validan mediante el pipeline oficial, comprueban
las columnas SST contra `MilenaTable` y exponen una vista prestada
`ASTNode + MilenaTable` para un backend futuro. No crean IR, no poseen la tabla
y no enlazan `compiler.c`, `ir.c` ni `vm.c`; por tanto, esta frontera no se
describe como un compilador integrado todavía. `tests/test_canonical_compiler.c`
comprueba el contrato y que la tabla prestada sobrevive al liberar el adaptador.

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

## Flujo numérico canónico

El flujo de CSV es una capacidad del lenguaje, no una utilidad C independiente.
La forma humana canónica es `datos desde "archivo.csv"`, opcionalmente seguida
por `procesar por lotes de N filas`, un bloque `resumir` y `guardar resultado`.
Cada operación (`suma`, `media`, `minimo`, `maximo`, `contar`, `varianza` y
`desviacion_estandar`) se reconoce en el lexer, se tipa en el AST, se valida
semánticamente y se traduce una sola vez al runtime común antes de llamar al
backend acotado. La semántica de `contar` cuenta valores numéricos válidos; las
otras operaciones también ignoran valores inválidos por métrica y los reportan.

PR25 implementa resúmenes numéricos globales de una pasada con un búfer de
registro reutilizado y acumuladores constantes por métrica. No materializa
las filas ni promete latencia fija, escala industrial o ejecución distribuida.
La sintaxis `operacion:columna` queda únicamente como compatibilidad legacy y
no es la superficie recomendada ni un segundo parser. Agrupaciones streaming,
spill-to-disk y joins externos son fases futuras explícitas y no forman parte
del contrato global de este flujo.

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

