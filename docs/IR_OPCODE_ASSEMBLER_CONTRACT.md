# Contrato de IR, opcode y assembler

**Estado:** contrato de diseño para preparar la fase 3; no describe una implementación completada. La fase 2 sigue abierta según `COMPILADOR_IR_PLAN.md`. Este documento no la cierra ni autoriza a empezar cambios de fase 3 antes de superar su gate de salida.

## 1. Decisión de arquitectura

Milena conserva representaciones tipadas por familia; no se inventa una HIR universal para hacer coincidir prototipos incompatibles. La fuente de verdad semántica es el HIR canónico de cada familia. La frontera ejecutable escalar y los planes de datos son formatos distintos:

| Ruta | Contrato de representación | Límite |
|---|---|---|
| Funciones escalares | El contrato tipado de registros/opcodes de MLBC v1.2 descrito en `BYTECODE_V1.md` | Subconjunto cerrado de `MilenaScalarHIR`; sin coerciones implícitas. |
| Operaciones de tabla | `MilenaDataHIR` y el plan `DPLN` de `BYTECODE_DATA_ABI.md` | El ABI v1.3 es un slice compile-only; no se ejecuta en la VM escalar. |
| Streaming, Arrow y SQL | Sus planificadores tipados específicos | No se reinterpretan como opcodes escalares genéricos. |

La adopción semántica de MLBC v1.2 no promueve por sí sola los módulos experimentales a producción ni cambia los bytes v1.0–v1.2. Cualquier cambio de wire requiere una nueva versión y su propia especificación, verificador y pruebas. `DPLN` v1.3 conserva su módulo-kind y límites independientes; no es un alias de una instrucción escalar.

## 2. Prototipos heredados: inventario y disposición

Los nombres siguientes representan contratos diferentes y **no se pueden convertir por ordinal, cast ni nombre parecido**:

| Prototipo | Forma actual | Disposición bajo este contrato |
|---|---|---|
| `IRProgram` / `IROpCode` | 15 operaciones de alto nivel sobre datasets; operandos `arg1/arg2/arg3` como cadenas y dos `double` | Compatibilidad/prototipo; no es entrada de un backend canónico. |
| `InstrOpcode` / `MachineInstruction` | 16 códigos de bajo nivel (`LOAD`…`PUSH`/`POP`); instrucción de cuatro bytes | Prototipo separado; no es el encoding del assembler ni MLBC. |
| `OpCode` / `Instruction` del assembler | 14 códigos propios; tres operandos `int` | Prototipo separado; no es compatible por layout ni por semántica con los otros dos. |
| MLBC v1.2 | Registro fijo de 24 bytes, registros con tags y verifier escalar | Referencia de semántica escalar tipada, sin cambios retroactivos al wire. |
| `DPLN` v1.3 | Plan de datos tipado, cadenas por IDs y límites explícitos | ABI distinto; solo cubre el slice especificado y no la ejecución VM/AOT. |

El assembler heredado demuestra por qué no es una traducción válida: `IR_CLEAN_NULLS`, `IR_TRANSFORM_TOTAL`, filtros, agrupación, promedio, mínimo, máximo y exportación se convierten en `OP_LOAD`; `IR_AGGREGATE_SUM` se convierte en `OP_ADD`. Además, `assembler_assemble` escribe `strlen(arg)` en un operando entero y convierte `num_arg1` a `int`; `0.0` se confunde con ausencia. Esas operaciones no preservan valores, strings ni tipos. El VM heredado consume `IRProgram` directamente, no el resultado del assembler; los agregados en su switch solo imprimen un mensaje. Por tanto, no hay hoy un recorrido IR→assembler→VM con paridad semántica.

Los tres prototipos quedan explícitamente en cuarentena para compilación canónica. Se pueden mantener temporalmente para compatibilidad interna, pero no se amplían ni se presentan como backend de Milena. Cualquier migración o retirada física de sus APIs requiere pruebas de consumidores y no forma parte de este documento.

## 3. Contrato escalar de operandos, tipos y control de flujo

La bajada escalar consume un `MilenaScalarHIR` ya resuelto; no vuelve a parsear source text ni reconstruye tipos a partir de `char *`. La semántica concreta de cada instrucción es la definida por `BYTECODE_V1.md`, especialmente su v1.2 tipado:

- Los registros son IDs acotados del módulo; cada registro tiene un único tipo fijo por programa: número o booleano. Una escritura incompatible se rechaza en la instrucción que la produce; un registro no cambia de tipo entre ramas.
- Los números son IEEE-754 binary64 finitos. Las constantes no finitas, división por cero y resultados aritméticos no finitos producen un error explícito; no se truncan a entero ni se sustituyen por cero.
- Las comparaciones consumen operandos del tipo admitido y producen booleanos. Las ramas condicionales consumen un booleano; no convierten números a condiciones.
- Los `FUNCTION`, `CALL`, `RETURN` y saltos usan IDs, rangos de registros y destinos de instrucción validados, nunca nombres reinterpretados como offsets. Aridad, tipos de argumentos/resultado, pertenencia de rama a función y retornos se comprueban antes de ejecutar. No se permiten saltos fuera del módulo ni entre funciones. Las restricciones de profundidad y recursión son las del contrato v1.2 vigente.
- Los bytes reservados deben ser cero, los opcodes desconocidos son error y el opcode desconocido **no** se transforma en `HALT`, `LOAD` u otra operación por defecto.
- Las llamadas conservan sus argumentos y tipos exactos; no hay coerción número/booleano ni conversión de aridad a partir de texto.

El lowerer/emisor debe producir un módulo completo o fallar sin publicar salida parcial. El verificador valida el módulo entero antes de cualquier efecto y la VM/AOT verifican los mismos bytes que consumen. Los diagnósticos de parseo/semántica/lowering conservan el span de HIR; el formato v1.2 no se modifica para insertar punteros o layouts C. Un mapa de PC a spans, si se incorpora, será metadata explícita y versionada, no una dependencia de direcciones de memoria del proceso.

## 4. Strings y operaciones de datos

El contrato escalar v1.2 no tiene operandos string arbitrarios. Una ruta, nombre de columna u otro texto de datos no puede codificarse como su longitud, como `atoi`, ni como un registro numérico. Para el slice de datos admitido, las cadenas se representan por IDs de la tabla UTF-8 con longitud explícita definida por `BYTECODE_DATA_ABI.md`; el verificador comprueba límites, encoding, referencias y reglas de nombres. No se permite terminación NUL implícita, truncamiento ni conservar pointers del AST/HIR en bytes serializados.

`MilenaDataHIR` sigue siendo dueño de la semántica de las operaciones de datos. Solo se baja a `DPLN` la forma exacta que acepta su ABI; las demás formas se rechazan antes de emitir. Hasta que exista una ruta de ejecución de datos dedicada y verificada, un plan v1.3 no se entrega a `milena_bytecode_run`, a AOT escalar ni al assembler heredado. No se mapea una operación de datos a `LOAD`, `ADD` o `PRINT` para aparentar soporte.

## 5. Contrato del emisor/assembler

Para la ruta canónica, “assembler” significa un emisor tipado desde HIR/IR aprobado hacia el formato objetivo; no un segundo parser textual ni un casteo entre enums. El emisor:

1. acepta solo la representación y versión especificadas para su familia;
2. convierte cada operación admitida mediante una tabla exhaustiva de opcode, aridad, tipo de cada operando, tipo de resultado y efectos/error;
3. preserva exactamente inmediatos binary64, booleanos, referencias a registros, IDs de funciones/bloques, aridad y cadenas por ID;
4. valida rangos antes de estrechar tipos enteros; no usa `strlen` como valor de un operando ni sentinel `0` para representar ausencia;
5. rechaza cualquier opcode/forma no soportada con estado y span de origen, sin fallback genérico;
6. publica el buffer solo tras terminar lowering, serialización y verificación; ante error, la salida queda vacía y se liberan recursos parciales.

Las operaciones que no tengan una fila completa de semántica y operandos en la tabla aprobada no se ensamblan. No se añade una conversión automática entre `IROpCode`, `InstrOpcode` y `OpCode`.

## 6. Errores, límites y efectos

El estado de error es parte del contrato: argumento inválido, sintaxis/semántica/tipo, opcode u operando inválido, destino/rango fuera de límites, módulo truncado/excesivo, overflow, agotamiento de recursos, división por cero/no finitud y fallo de I/O no se colapsan en éxito, `HALT` ni un mensaje impreso. Los estados retornados mantienen su etapa y el diagnóstico disponible; los efectos sobre tablas/archivos solo ocurren después de verificación y preparación completa, con publicación atómica donde aplique.

Los límites se fijan en los contratos vigentes por formato/runtime y se verifican con aritmética checked antes de reservar o indexar. Los callers pueden reducir límites de ejecución, nunca aumentarlos por encima del hard cap. No se presenta un límite de instrucciones o tamaño del módulo como límite de RSS si el allocator y los temporales no están incluidos.

## 7. Pruebas obligatorias para implementación de fase 3

Antes de dar por satisfecha la fase 3, la implementación debe añadir y pasar en CI, sobre el SHA exacto revisado:

- Una matriz exhaustiva de cada opcode aceptado y cada opcode legado retirado: operandos, tipos, resultado, control de flujo, efectos y errores esperados; sin casos que caigan en un default silencioso.
- Pruebas HIR→emisor→verificador que comprueben igualdad exacta de valores inmediatos, strings/IDs, registros, firmas y destinos; incluyen cero frente a “ausente”, valores fraccionarios, límites, nombres largos y tipos incompatibles.
- Pruebas de funciones/branches: rangos/IDs válidos e inválidos, ramas fuera de función, booleanos requeridos, aridad/tipos de llamadas, returns y límites de registros/instrucciones.
- Pruebas negativas de opcode desconocido, enum mal formado, encoding truncado/excesivo, operandos fuera de rango y fallo de asignación; no deben dejar salida parcial y deben limpiar ownership en sanitizers.
- Casos diferenciales fuente canónica→HIR→bytes→VM/AOT frente a la semántica de referencia para **cada** opcode y error observable del subconjunto admitido. El mismo buffer verificado se entrega al backend; no hay una segunda traducción privada.
- Un gate que demuestre que las APIs heredadas no se consumen por la ruta canónica o que han sido retiradas con sus consumidores migrados.

La aprobación de MLBC v1.2 no valida v1.3 data-plan; el gate de lowering compile-only para DPLN no demuestra ejecución, CLI ni paridad VM/AOT. Cada ruta mantiene su propio alcance y pruebas.

## 8. Dependencia y salida

Este contrato prepara una decisión de diseño de fase 3, no inicia su implementación. Antes de iniciar esa fase sigue siendo obligatorio cerrar el gate de fase 2 indicado en `COMPILADOR_IR_PLAN.md`: completar las brechas de AST/HIR/semántica, errores/ownership/límites, cobertura y gates de memoria/workspace, y tener CI verde para el SHA exacto. Una vez satisfecho ese gate, la fase 3 debe implementar la cuarentena/migración descrita aquí y entregar las pruebas de la sección 7. Hasta entonces, el estado correcto es **fase 2 abierta; fase 3 bloqueada**.
