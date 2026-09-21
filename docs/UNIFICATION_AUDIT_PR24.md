# Auditoría de unificación de PR24

Esta auditoría describe qué rutas de ejecución forman parte del producto y qué
contratos conservan una frontera de compatibilidad. El objetivo no es borrar
las APIs públicas heredadas ni fingir que los módulos experimentales son el
runtime oficial.

## Mapa de rutas

| Capacidad | Entrada actual | Frontera canónica | Estado en PR24 |
| --- | --- | --- | --- |
| `analizar`, `perfil`, `inspect` | CLI directa | `milena_cli_*` valida fuente mediante lexer → parser → AST → semántica y delega al backend compatible | unificada como adaptador |
| Scripts `.milena` | router de compatibilidad | `milena_run_script` selecciona el runtime canónico para arrays/datasets; la sintaxis legacy solo se reconoce por compatibilidad | unificada en la entrada |
| Dataset legado | `dataset_*` y `Dataset` | backend compatible consumido por el runtime; API pública preservada | frontera pendiente |
| Parser de funciones numéricas/tablas | `function_parser.c`, `user_functions.c` | API heredada de funciones | frontera pendiente; no se añade sintaxis nueva al fallback |
| Arrays | APIs de array + AST | lexer → parser → AST → semántica → runtime de lenguaje → backend array | unificada |
| Tablas y datasets | `MilenaTable`/`Dataset` | runtime canónico para scripts de dataset y tablas | unificada para scripts; API Dataset pendiente |
| Flujo CSV | `stream.c` | AST de flujo → semántica → runtime de lenguaje → backend `stream.c` | canónica; no crear otro parser ni ejecutable |
| Finanzas | APIs C financieras | nodos SST/finanzas → semántica → runtime → backend financiero | unificada para scripts; C API preservada |
| SST | APIs C SST | nodos SST → semántica → runtime → backend SST | unificada para scripts; C API preservada |
| compiler/IR/VM/GC/arena/forest/module | módulos experimentales | contrato de aislamiento del binario oficial | separados deliberadamente |

## Regla de compatibilidad

Las entradas públicas heredadas permanecen disponibles para no romper programas
C existentes. En esta fase no reciben capacidades nuevas ni implementan un
segundo lenguaje: cuando una entrada es ejecutable desde la CLI, primero pasa
por una fuente mínima del lenguaje canónico y su AST/semántica. El backend
compatible solo conserva el formato histórico de salida.

`stream.c` es la única implementación de flujo y conserva sus límites de
registro, columnas, lote y memoria. El runtime de lenguaje es quien decide
cuándo invocarlo.

## Fronteras honestas

No se incorpora compiler/IR/VM/GC/arena/forest/module al binario oficial: sus
contratos y representaciones no son equivalentes al AST/runtime canónico y
mezclarlos introduciría un segundo camino de ejecución. La migración futura
debe definir primero un adaptador de AST y pruebas de equivalencia; hasta
entonces `check-experimental-isolation.py` es la garantía verificable.

La unificación de las APIs C heredadas (Dataset, funciones numéricas, finanzas y
SST) queda documentada como trabajo posterior: ya son backends consumidos por
el runtime cuando se usa la sintaxis del lenguaje, pero sus símbolos públicos
no se eliminan ni se reescriben mediante un parser paralelo.
