# Estado verificable de la IR canónica

Este registro complementa el contrato normativo de dos fases en `COMPILADOR_DOS_FASES.md`; no crea una fase, ruta ni IR adicional. Base de este incremento: PR #33 en `f441506413d45d573a085f4f1b3cb9f792d33e8c`. Este registro describe solo el slice incorporado por el commit que contiene el archivo; la evidencia de CI debe consultarse contra el SHA exacto que lo publique. La fase 1 continúa incompleta.

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

La cobertura de lowering AST → IR tipada en esta iteración es **0 de 72 valores de `ASTNodeType`**. No se debe confundir el validador estructural ni el generador experimental de cadenas con un lowering del frontend canónico. Los 72 valores de nodo declarados en `include/ast.h` deben resolverse en la matriz oficial de sintaxis/semántica: cada uno debe recibir lowering tipado con pruebas o quedar explícitamente excluido/deprecado mediante política; ninguno tiene lowering a esta IR tipada en este slice:

`AST_PROGRAMA`, `AST_BLOQUE_ANALISIS`, `AST_DECLARACION_DATOS`, `AST_DECLARACION_ESTADISTICA`, `AST_ASIGNACION_DATASET`, `AST_LLAMADA_CARGAR`, `AST_BLOQUE_LIMPIAR`, `AST_BLOQUE_TRANSFORMAR`, `AST_BLOQUE_FILTRAR`, `AST_BLOQUE_AGRUPAR`, `AST_BLOQUE_RESUMIR`, `AST_BLOQUE_VISUALIZAR`, `AST_BLOQUE_EXPORTAR`, `AST_EXPRESION_OPERACION`, `AST_EXPRESION_LITERAL`, `AST_EXPRESION_IDENTIFICADOR`, `AST_EXPRESION_FUNCION`, `AST_EXPRESION_ARRAY`, `AST_EXPRESION_LLAMADA`, `AST_BLOQUE_FUNCION`, `AST_COMANDO_RETORNAR`, `AST_CONDICION_SI`, `AST_DECLARACION_FUNCION`, `AST_DECLARACION_ARRAY`, `AST_DECLARACION_VARIABLE`, `AST_ASIGNACION_VARIABLE`, `AST_COMANDO_NULOS`, `AST_COMANDO_DUPLICADOS`, `AST_COMANDO_CONDICION`, `AST_COMANDO_EXTRAER`, `AST_COMANDO_TOTAL`, `AST_COMANDO_PERIODO`, `AST_AGRUPACION_POR`, `AST_AGRUPACION_SPILL`, `AST_RESUMEN_METRICA`, `AST_OPERACION_ESTADISTICA`, `AST_DECLARACION_ENTRADA`, `AST_DECLARACION_SALIDA`, `AST_BLOQUE_SELECCIONAR`, `AST_COMANDO_COLUMNAS`, `AST_BLOQUE_UNIR`, `AST_COMANDO_DERECHA`, `AST_COMANDO_CLAVE`, `AST_COMANDO_SST`, `AST_STREAM_FILTER`, `AST_COLUMNAR_PROJECT`, `AST_COLUMNAR_FIELD`, `AST_SQL_PROGRAM`, `AST_SQL_QUERY`, `AST_SQL_EXECUTE`, `AST_SQL_BEGIN`, `AST_SQL_COMMIT`, `AST_SQL_ROLLBACK`, `AST_SQL_PARAMETER`, `AST_SQL_TABLE_SCHEMA`, `AST_SQL_SCHEMA_COLUMN`, `AST_SQL_TYPED_SELECT`, `AST_SQL_TABLE_REFERENCE`, `AST_SQL_PROJECTION_LIST`, `AST_SQL_PROJECTED_COLUMN`, `AST_SQL_FILTER`, `AST_SQL_FILTER_COLUMN`, `AST_SQL_FILTER_OPERATOR`, `AST_SQL_TYPED_INSERT`, `AST_SQL_INSERT_COLUMN_LIST`, `AST_SQL_INSERT_COLUMN`, `AST_SQL_INSERT_VALUE_LIST`, `AST_SQL_TYPED_UPDATE`, `AST_SQL_UPDATE_ASSIGNMENT_LIST`, `AST_SQL_UPDATE_ASSIGNMENT`, `AST_SQL_UPDATE_COLUMN`, `AST_SQL_UPDATE_FILTER`, `AST_NODE_TYPE_COUNT` (sentinel, no es nodo).

## Restante de la fase 1

No se implementan aún lowering desde AST tipado, firmas/parámetros/llamadas/variables, parámetros de entrada, ciclos con semántica de lenguaje, conversiones, operadores adicionales, arrays, nulos, cadenas, datasets/tablas, joins, estadísticas, CSV/IO, SQL ni streams; tampoco spans de origen, efectos, errores, ownership y límites de recursos en instrucciones IR, bytecode portable serializable/versionado, lector/validador de bytecode, VM de referencia, ejecución diferencial o cobertura completa por constructo. Aunque ya hay aristas tipadas y dominancia en la IR experimental, no hay frontend que las produzca ni semántica ejecutable.

Este incremento añade pruebas para merge de valores en un diamante, uso de valor dominante entre bloques, rechazo de argumentos de arista faltantes/duplicados/de tipo incorrecto, rechazo de valor definido en bloque hermano y de uso no dominante, y rechazo de bloques inalcanzables. Debe ejecutarse el target dedicado en GCC, Clang y sanitizadores contra el SHA publicado; las pruebas locales no sustituyen CI.

## Continuación necesaria para el gate de salida de fase 1

1. Fijar el inventario normativo de sintaxis/AST contra gramática, parser y semántica oficiales; por cada forma, probar lowering completo o retirarla/deprecarla formalmente de la superficie oficial y actualizar pruebas/documentación.
2. Completar frontend tipado (tipos, nombres/ámbitos, firmas, conversiones, errores y spans) e implementar lowering AST→esta única IR, incluyendo entradas de función y control de flujo; añadir tests de cobertura AST con matriz inventariada sin casos omitidos.
3. Completar contrato de tipos, efectos, errores, ownership, límites de memoria/recursos y operaciones de dominio antes de congelar la representación portable; validar aristas, ciclos y terminadores bajo casos positivos y negativos.
4. Definir y probar formato/versionado canónico de bytecode; serializador/lector con límites, corrupción, truncamiento y verificación fail-closed, incluida la IR convertida a bytecode.
5. Implementar VM de referencia consumiendo únicamente bytecode verificado, y pruebas diferenciales frontend→IR→bytecode→VM frente al comportamiento oficial, más errores y limpieza bajo fallos.
6. Ejecutar pruebas completas y CI GCC/Clang/sanitizadores/plataformas contra el SHA exacto; documentar evidencia física Android/Termux donde el gate de dos fases la requiera. Solo con toda esa evidencia puede cerrarse fase 1. AOT/JIT nativo corresponde únicamente a fase 2 y queda fuera de alcance.

Este incremento no satisface ni declara satisfecho el gate de salida de fase 1. La fase 2 (AOT/JIT nativo) permanece fuera de alcance.
