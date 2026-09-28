# Compilador de Milena: plan normativo por fases

**Estado:** contrato normativo de aceptación. Este roadmap sustituye la organización anterior en dos fases. `COMPILADOR_IR_PLAN.md` queda como archivo histórico de análisis e incrementos; cuando haya diferencias de estado o alcance, rige este documento y la evidencia del SHA exacto.

La entrega se organiza en ocho fases secuenciales. Cada fase tiene un gate de salida verificable. Preparar documentación, aislar prototipos, añadir un inventario, o completar solo un subconjunto no cierra una fase. Ninguna fase posterior se considera iniciada formalmente antes de cerrar la anterior; se permiten incrementos preparatorios fail-closed siempre que se describan como tales.

## Regla de arquitectura

La ruta canónica es única:

`fuente Milena → lexer → AST tipado → análisis semántico → HIR → IR Milena verificada → bytecode portable → VM de referencia → backend AOT/JIT`

No se agregará un parser, un IR de lenguaje o un runtime competidor. La compatibilidad con consumidores heredados es transitoria, explícita y no puede anunciarse como compilación. Ninguna sintaxis oficialmente soportada podrá quedar indefinidamente fuera de la ruta canónica: cada forma se implementa o se depreca con política de versión y diagnóstico explícito.

## Fase 1 — lexer y spans

**Objetivo:** tokens, errores léxicos y rangos de fuente inequívocos, conservados por el parser en los nodos que construye.

- Los offsets son bytes y spans semiabiertos `[inicio, fin)`; líneas/columnas empiezan en 1, el final es exclusivo y LF avanza de línea. EOF tiene span vacío. Las columnas cuentan bytes salvo que una futura versión defina otra unidad.
- Fijar reglas numéricas y de escapes: signo unario separado, exponente con signo opcional y dígitos obligatorios, rechazo de valores no finitos/fuera de rango, y error para escapes desconocidos.
- El lexema decodificado no altera el span del texto fuente. Los diagnósticos conservan la posición inicial pertinente.
- El parser copia spans a expresiones numéricas/identificadores y operaciones aritméticas, declaraciones/asignaciones, llamadas estadísticas, bloques de análisis y funciones numéricas; los contenedores agregan rangos de hijos. Los diagnósticos semánticos cubiertos apuntan al origen del argumento. Esta fase no exige rediseñar el AST ni construir HIR.

**Gate de salida:** pruebas de límites, errores, limpieza/estado y reglas numéricas/escapes; spans consistentes en la superficie enumerada; matrices CI requeridas verdes en el SHA exacto. No contar un workflow omitido como aprobado ni inferir ejecución local inexistente.

## Fase 2 — AST estructurado, semántica y HIR

**Objetivo:** representar estructuralmente cada constructo oficial, con semántica tipada y HIR poseída, sin reinterpretar cadenas de comandos ni volver a parsear la fuente.

- Mantener una matriz exhaustiva `sintaxis → AST → regla semántica → HIR → ejecución de referencia → pruebas`. Toda variante AST debe estar soportada o tener estado histórico/gramatical resuelto y política explícita de exclusión o deprecación.
- Crear nodos/payloads tipados por constructo con spans; resolver bindings, funciones, datasets, columnas, tipos, nulabilidad, efectos, errores y políticas de recursos que correspondan.
- La HIR debe conservar IDs estables, operandos tipados, referencias resueltas, ownership/lifetimes y spans. Los consumidores estrictos rechazan sin salida parcial cualquier forma no representada; la ruta de compatibilidad no autoriza compilación.
- Verificar el recorrido canónico lexer→parser→semántica→HIR y paridad con el runtime existente para cada constructo admitido; probar diagnósticos, fallos de asignación, limpieza, límites y entradas inválidas.

**Gate de salida:** cobertura por constructo cerrada o exclusión versionada; pruebas end-to-end y de error/cleanup; ausencia de fallback semántico silencioso; CI verde en el SHA exacto. Una HIR escalar o de datos parcial no satisface el gate.

**Estado incremental:** el slice de roles de esquema implementa en la HIR de datos canónica `AST_DECLARACION_ENTRADA` (`categorica`) y `AST_DECLARACION_SALIDA` (`binaria`) como metadatos de columna poseídos: valida forma/payload y unicidad, liga la columna fuente y conserva identidad/tipo físico/forma/span disponible. No modifica valores ni crea columnas, y la columna ausente falla durante binding. El contrato y runtime existente siguen siendo el oráculo de esos roles; los nodos aún no se bajan a la IR tipada ni bytecode/VM. Las pruebas canónicas de fuente cubren las dos declaraciones válidas, tipo inválido, binding ausente, duplicado y conflicto; esta cobertura parcial no cierra la Fase 2 ni cambia el ledger AST→IR→bytecode de las fases posteriores.

## Fase 3 — IR, opcode y assembler

**Objetivo:** traducir la HIR admitida a una representación de instrucciones tipada con una semántica inequívoca, preservando operandos, valores, strings, tipos, control de flujo y errores.

- Auditar y poner en cuarentena o retirar `IRProgram`/`IROpCode`, `InstrOpcode`/`MachineInstruction` y `OpCode`/`Instruction` antes de promover un contrato tipado.
- Prohibir conversiones por ordinal, cast, coincidencia de nombre o mapeo genérico; no permitir conversiones numéricas, de strings, tipos o errores con pérdida. El lowering falla cerrado ante operaciones no admitidas.
- Usar MLBC v1.2 como referencia del subconjunto escalar y mantener `MilenaDataHIR`/DPLN v1.3 en su contrato separado; no afirmar una HIR universal por compartir infraestructura.
- Probar cada opcode aceptado desde el productor HIR hasta la instrucción tipada emitida y su verificación estructural, con casos positivos/negativos y límites. Cuando ya haya un oráculo de ejecución, comparar también resultados y errores; las operaciones sin semántica ejecutable permanecen fuera del conjunto admitido. Documentar por separado los opcodes no implementados y los módulos experimentales.

**Gate de salida:** matriz opcode por opcode ejecutada y verde; trazabilidad semántica completa para cada operación admitida; ninguna ruta canónica usa los mapeos heredados con pérdida; guardias estáticas y pruebas E2E verdes en el SHA exacto. Un contrato escrito, una cuarentena o una matriz sin recorrido de ejecución no cierran la fase.

El contrato detallado de fase 3 se mantiene en `IR_OPCODE_ASSEMBLER_CONTRACT.md` dentro de la PR preparatoria de esa fase; sus listas de soporte son cerradas y no amplían el lenguaje por inferencia.

## Fase 4 — bytecode portable

Especificar magic/versión, anchos fijos, endianess y serialización determinista sin punteros nativos. El lector debe validar tamaños y límites antes de reservar memoria y rechazar truncamiento, IDs/tipos/firmas inválidos, CFG/saltos incorrectos, valores sin definición, recursos excesivos y opcodes desconocidos. Gate: round-trip, límites, entradas malformadas y fuzzing reproducible.

## Fase 5 — VM y memoria por ejecución

Ejecutar solo IR/bytecode verificados. Definir ownership de valores/recursos y limpieza en éxito, error y cancelación; propagar errores deterministas con categoría/span. Implementar límites y memoria por ejecución con fallos controlados; no reutilizar un GC sin roots seguros. Gate: batería diferencial completa y sanitizadores verdes.

## Fase 6 — AOT nativo

Generar ejecutables nativos sin invocar LLVM, GCC, Clang u otro compilador como backend del programa Milena, y sin fallback silencioso a interpretación. Implementar ABI, instrucciones, registros, frames, llamadas, ramas, constantes, relocaciones y runtime necesario enlazado estáticamente. Validar Windows/PE, Linux/ELF y Android/Termux/Bionic en targets reales acordados; Termux requiere dispositivo/runner Android real. Gate: ejecución y paridad de resultados, errores y límites contra la VM, artefactos ligados al SHA.

## Fase 7 — JIT

Implementar emisión/ejecución real por target, permisos de memoria y transición W^X, invalidación/liberación y fallback seguro. Gate: lifecycle, errores, límites y paridad semántica; una API de reserva o un stub no cuenta como JIT.

## Fase 8 — integración industrial

Integrar componentes en el producto canónico detrás de fronteras explícitas; optimizar con mediciones reproducibles; validar multiplataforma, recursos y releases. Publicar artefactos y evidencia vinculados al SHA revisado. Un gate de dispositivo requiere un dispositivo/runner real.

## Evidencia y cambios de alcance

- Una fase solo se declara completa cuando sus criterios y dependencias anteriores pasan en el SHA exacto revisado. CI en un SHA anterior no valida cambios posteriores.
- Los informes separan código implementado, especificación, pruebas compiladas, pruebas ejecutadas y CI; no se extrapola soporte de un subconjunto a todo el lenguaje.
- Cambiar el orden, alcance oficial, targets, semántica o gates requiere actualizar este contrato y su matriz de aceptación antes de tratarlo como nuevo requisito.
