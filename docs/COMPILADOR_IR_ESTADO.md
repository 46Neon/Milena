# Estado verificable de la IR canónica

Este registro complementa el contrato normativo de dos fases en `COMPILADOR_DOS_FASES.md`; no crea una fase, ruta ni IR adicional. Base verificada antes del incremento: PR #33 en `f5ba036e11062cc10750ba0325d99e6a7e0c7080`. Este registro describe solo el slice incorporado por el commit que contiene el archivo; la evidencia de CI debe consultarse contra ese SHA exacto.

## Cobertura implementada en este incremento

Se reconcilia `include/ir.h` / `src/ir.c` añadiendo metadatos tipados a la representación IR existente, bloques básicos explícitos y un validador fail-closed. El constructor tipado y el verificador admiten exactamente:

- Tipos: `I64`, `F64`, `BOOL` y `VOID` (más `INVALID` como valor de rechazo).
- Constantes: `IR_CONST_I64`, `IR_CONST_F64`.
- Operaciones escalares: `IR_ADD_I64`, `IR_ADD_F64`, `IR_EQ_I64`.
- Terminadores: `IR_BRANCH`, `IR_COND_BRANCH`, `IR_RETURN`.
- Bloques contiguos con IDs no nulos y únicos, terminador obligatorio, sucesores explícitos existentes y consistencia entre aristas y terminador.
- IDs de valores únicos; operandos definidos anteriormente en el mismo bloque y con el tipo requerido; opcode, tipo de resultado y forma de operandos explícitos. El rechazo de usos entre bloques es deliberado: esta iteración aún no implementa parámetros/phi ni cálculo de dominadores.

El validador rechaza las operaciones legadas basadas en cadenas y cualquier opcode no listado. La API anterior de `ir_generate` se conserva por compatibilidad experimental, pero no produce ni valida este subconjunto tipado. El ejecutable oficial no incluye `src/ir.c`; el target de prueba lo compila aislado para no enlazar una VM/backend incompletos en el producto.

## Lowering desde el lenguaje: todavía no implementado

La cobertura de lowering AST → IR tipada en esta iteración es **0 de 72 valores de `ASTNodeType`**. No se debe confundir el validador estructural ni el generador experimental de cadenas con un lowering del frontend canónico. Los 72 valores de nodo declarados en `include/ast.h` deben resolverse en la matriz oficial de sintaxis/semántica: cada uno debe recibir lowering tipado con pruebas o quedar explícitamente excluido/deprecado mediante política; ninguno tiene lowering a esta IR tipada en este slice:

`AST_PROGRAMA`, `AST_BLOQUE_ANALISIS`, `AST_DECLARACION_DATOS`, `AST_DECLARACION_ESTADISTICA`, `AST_ASIGNACION_DATASET`, `AST_LLAMADA_CARGAR`, `AST_BLOQUE_LIMPIAR`, `AST_BLOQUE_TRANSFORMAR`, `AST_BLOQUE_FILTRAR`, `AST_BLOQUE_AGRUPAR`, `AST_BLOQUE_RESUMIR`, `AST_BLOQUE_VISUALIZAR`, `AST_BLOQUE_EXPORTAR`, `AST_EXPRESION_OPERACION`, `AST_EXPRESION_LITERAL`, `AST_EXPRESION_IDENTIFICADOR`, `AST_EXPRESION_FUNCION`, `AST_EXPRESION_ARRAY`, `AST_EXPRESION_LLAMADA`, `AST_BLOQUE_FUNCION`, `AST_COMANDO_RETORNAR`, `AST_CONDICION_SI`, `AST_DECLARACION_FUNCION`, `AST_DECLARACION_ARRAY`, `AST_DECLARACION_VARIABLE`, `AST_ASIGNACION_VARIABLE`, `AST_COMANDO_NULOS`, `AST_COMANDO_DUPLICADOS`, `AST_COMANDO_CONDICION`, `AST_COMANDO_EXTRAER`, `AST_COMANDO_TOTAL`, `AST_COMANDO_PERIODO`, `AST_AGRUPACION_POR`, `AST_AGRUPACION_SPILL`, `AST_RESUMEN_METRICA`, `AST_OPERACION_ESTADISTICA`, `AST_DECLARACION_ENTRADA`, `AST_DECLARACION_SALIDA`, `AST_BLOQUE_SELECCIONAR`, `AST_COMANDO_COLUMNAS`, `AST_BLOQUE_UNIR`, `AST_COMANDO_DERECHA`, `AST_COMANDO_CLAVE`, `AST_COMANDO_SST`, `AST_STREAM_FILTER`, `AST_COLUMNAR_PROJECT`, `AST_COLUMNAR_FIELD`, `AST_SQL_PROGRAM`, `AST_SQL_QUERY`, `AST_SQL_EXECUTE`, `AST_SQL_BEGIN`, `AST_SQL_COMMIT`, `AST_SQL_ROLLBACK`, `AST_SQL_PARAMETER`, `AST_SQL_TABLE_SCHEMA`, `AST_SQL_SCHEMA_COLUMN`, `AST_SQL_TYPED_SELECT`, `AST_SQL_TABLE_REFERENCE`, `AST_SQL_PROJECTION_LIST`, `AST_SQL_PROJECTED_COLUMN`, `AST_SQL_FILTER`, `AST_SQL_FILTER_COLUMN`, `AST_SQL_FILTER_OPERATOR`, `AST_SQL_TYPED_INSERT`, `AST_SQL_INSERT_COLUMN_LIST`, `AST_SQL_INSERT_COLUMN`, `AST_SQL_INSERT_VALUE_LIST`, `AST_SQL_TYPED_UPDATE`, `AST_SQL_UPDATE_ASSIGNMENT_LIST`, `AST_SQL_UPDATE_ASSIGNMENT`, `AST_SQL_UPDATE_COLUMN`, `AST_SQL_UPDATE_FILTER`, `AST_NODE_TYPE_COUNT` (sentinel, no es nodo).

## Restante de la fase 1

No se implementan aún lowering desde AST tipado, firmas/parámetros/llamadas/variables, ramas con valores de bloque y dominancia, ciclos con semántica de lenguaje, conversiones, operadores adicionales, arrays, nulos, cadenas, datasets/tablas, joins, estadísticas, CSV/IO, SQL ni streams; tampoco spans de origen, efectos, errores/ownership/límites en instrucciones de IR, bytecode portable serializable/versionado, lector/validador de bytecode, VM de referencia, ejecución diferencial o cobertura completa por constructo. Este incremento no satisface ni declara satisfecho el gate de salida de fase 1. La fase 2 (AOT nativo) permanece fuera de alcance.
