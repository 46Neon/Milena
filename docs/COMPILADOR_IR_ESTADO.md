# Estado verificable de la IR canónica

Este registro complementa el contrato normativo de dos fases en `COMPILADOR_DOS_FASES.md`; no crea una fase, ruta ni IR adicional. Snapshot auditado antes de este incremento: PR #33 en `59f307239d6030894ff37e10598f4efc8763d6fb`. Este registro describe solo el slice implementado en el snapshot indicado; la evidencia de CI debe consultarse contra el SHA exacto que lo publique. La fase 1 continúa incompleta.

## Cobertura implementada en este incremento

Se reconcilia `include/ir.h` / `src/ir.c` añadiendo metadatos tipados a la representación IR existente, bloques básicos explícitos y un validador fail-closed. El constructor tipado y el verificador admiten exactamente:

- Tipos: `I64`, `F64`, `BOOL` y `VOID` (más `INVALID` como valor de rechazo).
- Constantes: `IR_CONST_I64`, `IR_CONST_F64`.
- Operaciones escalares: `IR_ADD_I64`, `IR_ADD_F64`, `IR_EQ_I64`.
- Terminadores: `IR_BRANCH`, `IR_COND_BRANCH`, `IR_RETURN`.
- Bloques contiguos con IDs no nulos y únicos, terminador obligatorio, sucesores explícitos existentes y consistencia entre aristas y terminador. El primer bloque es la entrada y todos los bloques deben ser alcanzables desde ella; bloques desconectados se rechazan.
- IDs de valores únicos, operandos con tipo correcto, disponibilidad SSA dentro del bloque y dominancia de definiciones entre bloques. Se calcula dominancia iterativa sobre el CFG explícito, incluyendo ciclos/backedges; referencias desde bloques hermanos no dominantes se rechazan.
- Parámetros tipados de bloque, usados como valores SSA tipo phi, y argumentos explícitos por arista (`ir_program_add_block_parameter` / `ir_block_add_edge_argument`). Cada arista entrante a un bloque parametrizado debe suministrar exactamente una vez cada argumento, con índice/tipo correctos; la definición usada debe dominar el origen de la arista. En una rama condicional cuyos dos destinos coinciden, el par origen/destino es una arista lógica única y comparte su bundle de argumentos.
- Los parámetros del bloque de entrada se rechazan intencionalmente porque la firma/ABI de función aún no está implementada; no hay parámetros implícitos ni valores externos.

El validador rechaza las operaciones legadas basadas en cadenas y cualquier opcode no listado. La API anterior de `ir_generate` se conserva por compatibilidad experimental, pero no produce ni valida este subconjunto tipado. El ejecutable oficial no incluye `src/ir.c`; el paso dedicado de CI compila la prueba junto con `src/ir.c` y `src/common.c` de forma aislada, sin añadir esa fuente experimental al Makefile canónico ni enlazar una VM/backend incompletos en el producto.

## Lowering desde el lenguaje: todavía no implementado

La cobertura de lowering AST → IR tipada en este snapshot es **0 de 72 variantes reales de `ASTNodeType`**. `AST_NODE_TYPE_COUNT` es un sentinel de conteo, no una variante ni un nodo. No se debe confundir el validador estructural ni el generador experimental de cadenas con un lowering del frontend canónico. Las 72 variantes reales declaradas en `include/ast.h:8-79` deben resolverse en la matriz oficial de sintaxis/semántica; ninguna tiene lowering a esta IR tipada en este slice.

`AST_PROGRAMA`, `AST_BLOQUE_ANALISIS`, `AST_DECLARACION_DATOS`, `AST_DECLARACION_ESTADISTICA`, `AST_ASIGNACION_DATASET`, `AST_LLAMADA_CARGAR`, `AST_BLOQUE_LIMPIAR`, `AST_BLOQUE_TRANSFORMAR`, `AST_BLOQUE_FILTRAR`, `AST_BLOQUE_AGRUPAR`, `AST_BLOQUE_RESUMIR`, `AST_BLOQUE_VISUALIZAR`, `AST_BLOQUE_EXPORTAR`, `AST_EXPRESION_OPERACION`, `AST_EXPRESION_LITERAL`, `AST_EXPRESION_IDENTIFICADOR`, `AST_EXPRESION_FUNCION`, `AST_EXPRESION_ARRAY`, `AST_EXPRESION_LLAMADA`, `AST_BLOQUE_FUNCION`, `AST_COMANDO_RETORNAR`, `AST_CONDICION_SI`, `AST_DECLARACION_FUNCION`, `AST_DECLARACION_ARRAY`, `AST_DECLARACION_VARIABLE`, `AST_ASIGNACION_VARIABLE`, `AST_COMANDO_NULOS`, `AST_COMANDO_DUPLICADOS`, `AST_COMANDO_CONDICION`, `AST_COMANDO_EXTRAER`, `AST_COMANDO_TOTAL`, `AST_COMANDO_PERIODO`, `AST_AGRUPACION_POR`, `AST_AGRUPACION_SPILL`, `AST_RESUMEN_METRICA`, `AST_OPERACION_ESTADISTICA`, `AST_DECLARACION_ENTRADA`, `AST_DECLARACION_SALIDA`, `AST_BLOQUE_SELECCIONAR`, `AST_COMANDO_COLUMNAS`, `AST_BLOQUE_UNIR`, `AST_COMANDO_DERECHA`, `AST_COMANDO_CLAVE`, `AST_COMANDO_SST`, `AST_STREAM_FILTER`, `AST_COLUMNAR_PROJECT`, `AST_COLUMNAR_FIELD`, `AST_SQL_PROGRAM`, `AST_SQL_QUERY`, `AST_SQL_EXECUTE`, `AST_SQL_BEGIN`, `AST_SQL_COMMIT`, `AST_SQL_ROLLBACK`, `AST_SQL_PARAMETER`, `AST_SQL_TABLE_SCHEMA`, `AST_SQL_SCHEMA_COLUMN`, `AST_SQL_TYPED_SELECT`, `AST_SQL_TABLE_REFERENCE`, `AST_SQL_PROJECTION_LIST`, `AST_SQL_PROJECTED_COLUMN`, `AST_SQL_FILTER`, `AST_SQL_FILTER_COLUMN`, `AST_SQL_FILTER_OPERATOR`, `AST_SQL_TYPED_INSERT`, `AST_SQL_INSERT_COLUMN_LIST`, `AST_SQL_INSERT_COLUMN`, `AST_SQL_INSERT_VALUE_LIST`, `AST_SQL_TYPED_UPDATE`, `AST_SQL_UPDATE_ASSIGNMENT_LIST`, `AST_SQL_UPDATE_ASSIGNMENT`, `AST_SQL_UPDATE_COLUMN`, `AST_SQL_UPDATE_FILTER`, `AST_NODE_TYPE_COUNT` (sentinel, no es nodo).

## Inventario AST y estado de contrato del parser

La fuente primaria del inventario es `include/ast.h:8-80` (`ASTNodeType`); la comprobación de construcción/parser se hace contra `src/parser.c` y los constructores comunes de `src/ast.c`. Esta clasificación describe únicamente la gramática/estado documental actual, no cobertura de lowering ni ejecución IR. Las referencias normativas examinadas son `docs/COMPILADOR_DOS_FASES.md`, `docs/MILENA_FUNCTIONS.md`, `docs/MILENA_ARRAY_LANGUAGE_SPEC.md`, `docs/MILENA_STATISTICAL_AST.md`, `docs/GROUPED_SPILL_CONTRACT.md`, `docs/SQLITE_NATIVE_BACKEND.md` y `docs/COMPATIBILITY.md`.

### Contratos de sintaxis documentados

Estos conjuntos están respaldados por los documentos indicados y tienen representación actual en el parser. Eso no significa que estén bajados a la IR nueva:

- **Funciones y expresiones del intérprete numérico** (`docs/MILENA_FUNCTIONS.md`; parser `src/parser.c:234-408, 1729-2030`; nodos comunes de expresión creados por `ast_create_number` en `src/parser.c`): `AST_PROGRAMA`, `AST_DECLARACION_FUNCION`, `AST_BLOQUE_FUNCION` (contenedor de parámetros/cuerpo, no una operación), `AST_COMANDO_RETORNAR`, `AST_CONDICION_SI`, `AST_DECLARACION_VARIABLE`, `AST_ASIGNACION_VARIABLE`, `AST_EXPRESION_LITERAL`, `AST_EXPRESION_IDENTIFICADOR`, `AST_EXPRESION_OPERACION` y `AST_EXPRESION_LLAMADA`.
- **Arrays y expresiones de array** (`docs/MILENA_ARRAY_LANGUAGE_SPEC.md`; parser `src/parser.c:83-214, 234-319`): `AST_DECLARACION_ARRAY` y `AST_EXPRESION_ARRAY`. El documento distingue expresamente sintaxis/representación de ejecución ya integrada.
- **Operaciones estadísticas** (`docs/MILENA_STATISTICAL_AST.md`; parser `src/parser.c:456-620` y su llamada desde `src/parser.c:1120-1129`; constructor `ast_create_statistic`): `AST_OPERACION_ESTADISTICA`. Las ocho funciones y sus reglas sintácticas se documentan allí; el documento indica que este AST no está conectado al ejecutor de scripts.
- **Agrupación/streaming y publicación de salida** (`docs/GROUPED_SPILL_CONTRACT.md`; parser `src/parser.c:622-1019, 1026-1070, 1372-1567`): `AST_BLOQUE_ANALISIS`, `AST_LLAMADA_CARGAR`, `AST_BLOQUE_AGRUPAR`, `AST_AGRUPACION_POR`, `AST_AGRUPACION_SPILL`, `AST_RESUMEN_METRICA`, `AST_BLOQUE_RESUMIR` y `AST_BLOQUE_EXPORTAR`. El contrato documenta cortes y límites específicos; no extiende por implicación la gramática a otras formas AST.
- **SQL raw de compatibilidad y slice SQL tipado** (`docs/SQLITE_NATIVE_BACKEND.md`; parser `src/parser.c:2066-2688`): `AST_SQL_PROGRAM`, `AST_SQL_QUERY`, `AST_SQL_EXECUTE`, `AST_SQL_BEGIN`, `AST_SQL_COMMIT`, `AST_SQL_ROLLBACK`, `AST_SQL_PARAMETER`, `AST_SQL_TABLE_SCHEMA`, `AST_SQL_SCHEMA_COLUMN`, `AST_SQL_TYPED_SELECT`, `AST_SQL_TABLE_REFERENCE`, `AST_SQL_PROJECTION_LIST`, `AST_SQL_PROJECTED_COLUMN`, `AST_SQL_FILTER`, `AST_SQL_FILTER_COLUMN`, `AST_SQL_FILTER_OPERATOR`, `AST_SQL_TYPED_INSERT`, `AST_SQL_INSERT_COLUMN_LIST`, `AST_SQL_INSERT_COLUMN`, `AST_SQL_INSERT_VALUE_LIST`, `AST_SQL_TYPED_UPDATE`, `AST_SQL_UPDATE_ASSIGNMENT_LIST`, `AST_SQL_UPDATE_ASSIGNMENT`, `AST_SQL_UPDATE_COLUMN` y `AST_SQL_UPDATE_FILTER`. El raw SQL es una superficie de compatibilidad explícita separada del slice tipado acotado; ninguno de los dos debe confundirse con un ORM completo.

### Parser-reachable, pero con contrato público por cerrar

El parser crea o puede crear los siguientes tipos, pero esta revisión no encontró para cada forma un contrato oficial suficientemente completo para declararla sintaxis soportada sin reservas. Se mantienen como **resolución pendiente**, no como afirmación de soporte o exclusión. Las referencias dan la ubicación del constructor/rama: `src/parser.c:1026-1710` para bloques de análisis/tablas; `src/parser.c:83-408` para expresiones/arrays; y `src/parser.c:2066-2688` para SQL. En concreto:

- `AST_BLOQUE_LIMPIAR`, `AST_BLOQUE_TRANSFORMAR`, `AST_BLOQUE_FILTRAR`, `AST_COMANDO_NULOS`, `AST_COMANDO_DUPLICADOS`, `AST_COMANDO_CONDICION`, `AST_COMANDO_TOTAL`, `AST_COMANDO_PERIODO` (`src/parser.c:1216-1369, 1273-1308`): hay ramas del parser, pero falta reconciliar todas sus formas, semántica y compatibilidad con la especificación oficial antes de fijar su estado de soporte.
- `AST_DECLARACION_ENTRADA`, `AST_DECLARACION_SALIDA`, `AST_BLOQUE_SELECCIONAR`, `AST_COMANDO_COLUMNAS`, `AST_BLOQUE_UNIR`, `AST_COMANDO_DERECHA`, `AST_COMANDO_CLAVE` (`src/parser.c:1079-1107, 1586-1705`): los constructores existen; sus gramáticas y el alcance de soporte quedan pendientes de validación documental independiente.
- `AST_COMANDO_SST` (`src/parser.c:1131-1163`): el parser lo construye desde una lista cerrada de nombres, pero eso por sí solo no prueba que cada nombre sea una operación de lenguaje aceptada ni su semántica.
- `AST_STREAM_FILTER`, `AST_COLUMNAR_PROJECT`, `AST_COLUMNAR_FIELD` (`src/parser.c:918-1006`): tienen constructores de parser de flujo/columnas; queda pendiente resolver su contrato oficial y relación con la ruta canónica.
- `AST_DECLARACION_DATOS` y `AST_DECLARACION_ESTADISTICA` (`src/parser.c:1131-1139`) son **marcadores vacíos** creados para `#datos` y `#estadistica` dentro del análisis; no son una gramática de declaración completa. Su papel y cualquier requisito de compatibilidad son ambiguos y quedan sin resolver.

### Variantes declaradas sin constructor actual del parser

La búsqueda de constructores/rutas de creación en `src/parser.c` para estas cuatro variantes no halló una construcción actual: `AST_ASIGNACION_DATASET`, `AST_BLOQUE_VISUALIZAR`, `AST_EXPRESION_FUNCION` y `AST_COMANDO_EXTRAER` (`include/ast.h:12,19,24,37`). Algunas tienen ramas consumidoras o casos semánticos en `src/semantic.c`, `src/language_semantic.c`, `src/interpreter.c` o `src/canonical_compiler.c`, lo cual no demuestra que sean sintaxis activa. Se clasifican como **variantes enum-only / candidatas a legado o compatibilidad, con estado histórico no confirmado**; se conservan y no se recomienda eliminarlas/deprecarlas sin política de versión y evidencia. En particular, `docs/MILENA_FUNCTIONS.md` excluye explícitamente las funciones como valores, y `src/parser.c:1292-1294` convierte la forma antigua `#periodo extraer(...)` al nodo `AST_COMANDO_PERIODO`, no a `AST_COMANDO_EXTRAER`.

### Compatibilidad y ambigüedades que no se convierten en cambios de gramática

`src/parser.c:1292-1294` acepta explícitamente `#periodo extraer(...)` como forma histórica y la normaliza al nodo `AST_COMANDO_PERIODO`; `docs/COMPATIBILITY.md` exige que la compatibilidad histórica esté identificada explícitamente. La sintaxis de entrada debe permanecer anotada como **compatibilidad observada en implementación; aprobación normativa pendiente** hasta que la política del lenguaje la confirme.

En `src/parser.c:1586-1705`, un bloque con un nombre arbitrario seguido de `{` cae actualmente en la rama que usa `AST_BLOQUE_FILTRAR` salvo los nombres especiales `seleccionar` y `unir`; dentro de esos bloques hay caminos que avanzan sobre tokens no reconocidos sin emitir error. Esto colisiona con la regla fail-closed de `docs/COMPATIBILITY.md`, pero no se cambia aquí: decidir si es sintaxis histórica, extensibilidad accidental o error del parser requiere una decisión normativa. No se añade una regresión que congele esa conducta ambigua ni se inventa un diagnóstico nuevo para una sintaxis no resuelta.

### Clasificación del sentinel y límite de lowering

`AST_NODE_TYPE_COUNT` (`include/ast.h:80`) es exclusivamente el sentinel. Por tanto, el inventario es **72 variantes reales + 1 sentinel** y la cobertura de lowering de este slice es **0/72**, no 0/73. `AST_EXPRESION_LITERAL` y `AST_OPERACION_ESTADISTICA` son construidos mediante helpers (`ast_create_number` y `ast_create_statistic`), por lo que la ausencia de menciones literales directas en la llamada a `ast_create` no los convierte en variantes sin constructor.

## Restante de la fase 1

No se implementan aún lowering desde AST tipado, firmas/parámetros/llamadas/variables, parámetros de entrada, ciclos con semántica de lenguaje, conversiones, operadores adicionales, arrays, nulos, cadenas, datasets/tablas, joins, estadísticas, CSV/IO, SQL ni streams; tampoco spans de origen, efectos, errores, ownership y límites de recursos en instrucciones IR, bytecode portable serializable/versionado, lector/validador de bytecode, VM de referencia, ejecución diferencial o cobertura completa por constructo. Aunque ya hay aristas tipadas y dominancia en la IR experimental, no hay frontend que las produzca ni semántica ejecutable.

Este incremento añade pruebas para merge de valores en un diamante y valor de bucle transferido por backedge, uso de valor dominante entre bloques, rechazo de argumentos de arista faltantes/duplicados/de tipo incorrecto, rechazo de valor definido en bloque hermano y de uso no dominante, y rechazo de bloques inalcanzables. Debe ejecutarse el target dedicado en GCC, Clang y sanitizadores contra el SHA publicado; las pruebas locales no sustituyen CI.

## Continuación necesaria para el gate de salida de fase 1

1. Fijar el inventario normativo de sintaxis/AST contra gramática, parser y semántica oficiales; por cada forma, probar lowering completo o retirarla/deprecarla formalmente de la superficie oficial y actualizar pruebas/documentación.
2. Completar frontend tipado (tipos, nombres/ámbitos, firmas, conversiones, errores y spans) e implementar lowering AST→esta única IR, incluyendo entradas de función y control de flujo; añadir tests de cobertura AST con matriz inventariada sin casos omitidos.
3. Completar contrato de tipos, efectos, errores, ownership, límites de memoria/recursos y operaciones de dominio antes de congelar la representación portable; validar aristas, ciclos y terminadores bajo casos positivos y negativos.
4. Definir y probar formato/versionado canónico de bytecode; serializador/lector con límites, corrupción, truncamiento y verificación fail-closed, incluida la IR convertida a bytecode.
5. Implementar VM de referencia consumiendo únicamente bytecode verificado, y pruebas diferenciales frontend→IR→bytecode→VM frente al comportamiento oficial, más errores y limpieza bajo fallos.
6. Ejecutar pruebas completas y CI GCC/Clang/sanitizadores/plataformas contra el SHA exacto; documentar evidencia física Android/Termux donde el gate de dos fases la requiera. Solo con toda esa evidencia puede cerrarse fase 1. AOT/JIT nativo corresponde únicamente a fase 2 y queda fuera de alcance.

Este incremento no satisface ni declara satisfecho el gate de salida de fase 1. La fase 2 (AOT/JIT nativo) permanece fuera de alcance.
