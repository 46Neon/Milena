# Contrato de la ruta canónica del lenguaje

Este documento fija la frontera de arquitectura de la mejora 1: una sola entrada de lenguaje, un lexer/parser canónico y etapas tipadas hasta el runtime/backend. La ruta no significa que todos los backends sean idénticos ni que las APIs C heredadas desaparezcan; significa que toda sintaxis nueva entra por el mismo frontend y no obtiene un parser, CLI o ejecutable paralelo.

## Ruta canónica

```text
CLI milena run
  → milena_cli_run_script / milena_run_script
  → lexer → parser → AST tipado → semántica
  → HIR → plan físico → runtime/backend
  → runtime Milena → API C del motor → backend

Ruta objetivo explícita: lexer → parser → AST tipado → semántica → HIR → plan físico → runtime/backend.
```

El AST pasa por `milena_validate_ast` antes de aceptar una ejecución de dataset. Para los comandos soportados, `milena_run_dataset_program` conecta el frontend con los límites del runtime: `MilenaDataHIR` se construye, se vincula a tablas tipadas y lo consume `milena_canonical_program_execute_data`; CSV streaming, Arrow IPC y SQL consumen sus planificadores tipados y validadores antes del backend correspondiente. Los targets C17 del Makefile son `make check-hir-ast-coverage`, que verifica el inventario cerrado de HIR, y `make check-unification-architecture`, que depende del anterior y comprueba estas conexiones. El target `make test` ejecuta ambos como parte de sus guardas de arquitectura.

`analizar`, `perfil` e `inspect` son adaptadores CLI de compatibilidad: `main.c` delega en `entrypoints.c`, cuya entrada sintética pasa por lexer/parser/AST/semántica antes de invocar las APIs C históricas de Dataset/análisis. No son una ruta para agregar sintaxis o capacidades nuevas.

## Ramas tipadas actuales

| Familia | Representación/planner | Runtime/backend | Alcance comprobable |
|---|---|---|---|
| Funciones escalares | `MilenaScalarHIR`; para AOT, `MilenaIRProgram`/`MilenaIRModule` verificados | intérprete/runtime del lenguaje; `native_aot.c` para el subconjunto AOT (`milena build`) | El backend nativo consume y vuelve a verificar la IR tipada; dataset HIR y opcodes no soportados se rechazan. Véase [Native AOT](NATIVE_AOT.md). |
| Operaciones de tabla en memoria | `MilenaDataHIR` | `milena_canonical_program_execute_data` sobre `MilenaTable` | Subconjunto cerrado; fuente, esquema, spans, tipos y operaciones se validan. |
| CSV streaming | `MilenaStreamExecutionPlan` | runtime de lenguaje → `stream.c` | Plan lógico/físico tipado; plan y límites se validan antes de ejecutar. |
| Arrow IPC | `MilenaArrowIpcExecutionPlan` | runtime de lenguaje → backend Arrow | Proyección/filtro y operadores del slice soportado se validan antes del backend. |
| SQL local | `MilenaSqlExecutionPlan` | runtime de lenguaje → backend SQLite | Operaciones y parámetros aceptados pasan por validación de plan; el slice no equivale a un ORM completo. |

Los planes por familia son las representaciones tipadas disponibles para sus operaciones; no afirman que exista una IR universal ni que todas las operaciones compartan ya el mismo plan físico. Los elementos nuevos deben entrar por la gramática, la semántica y la representación tipada adecuada; si aún no existe, se rechazan explícitamente hasta que se incorpore a esta ruta.

## Excepciones de compatibilidad

- `src/function_parser.c` / `function_parser.o` permanece únicamente para `run_legacy_numeric_functions`; no recibe sintaxis nueva. El flujo nuevo de funciones usa lexer, parser, AST e intérprete canónicos.
- `script.c` conserva handlers textuales históricos para scripts sin construcciones canónicas. Si una entrada marca sintaxis canónica y falla el parseo, el router devuelve `SCRIPT_PIPELINE_PARSE_ERROR`; no debe reinterpretarla como éxito por el fallback.
- `entrypoints.c` conserva las APIs C y el formato de salida de los comandos `analizar`, `perfil` e `inspect`; el frontend sintético es una barrera de compatibilidad, no un segundo lenguaje.
- Las APIs C heredadas siguen disponibles para compatibilidad, pero no son un lugar para implementar una nueva capacidad invocable desde Milena.

Toda excepción nueva debe estar nombrada aquí, limitada a compatibilidad y cubierta por el chequeo de arquitectura. No se permite añadir otro `main`, otro lexer/parser de Milena ni una ruta textual que acepte comandos nuevos.

## Límites que siguen abiertos

- El HIR sigue siendo un subconjunto cerrado: la cobertura actual clasifica explícitamente nodos representados y rechazados. No existe un HIR universal; no se debe describir como universal ni como cobertura completa.
- `script_pipeline_from_ast` clasifica el fuente y el runtime vuelve a analizarlo: rehace el parseo. Eliminar ese trabajo repetido corresponde a la mejora 2 y queda fuera de esta mejora.
- Las ramas de SQL, streaming y Arrow tienen planificadores tipados propios; su presencia no prueba que todas las operaciones de Milena compartan un único plan lógico/físico.
- Las APIs heredadas y los límites de cada backend no quedan eliminados ni ampliados por este contrato.

## Tres reglas no negociables

1. **Alcance único:** esta PR implementa solo la mejora 1. Se trabaja en una sola PR nueva contra el `main` vigente; no se toca `main`, no se amplía la PR #31 ya integrada ni se mezclan las mejoras 2–100.
2. **Ruta única para sintaxis nueva:** toda capacidad nueva sigue lexer → parser → AST tipado → semántica → HIR/plan tipado → plan físico validado → runtime/backend. Las rutas heredadas quedan permitidas solo si están enumeradas aquí; no se añade parser, ejecutable ni fallback paralelo.
3. **Fallo seguro y evidencia exacta:** ante un fallo se detienen los reintentos ciegos, se identifica la causa raíz y se retoma desde el último estado verde. No se modifica `main`, no se fuerza el historial ni se eluden protecciones. La revisión requiere CI verde sobre el SHA exacto y la aprobación requerida; no se fusiona antes de esa aprobación y la confirmación explícita acordada.
