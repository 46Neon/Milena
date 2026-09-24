# Plan industrial del compilador de Milena

**Estado de esta rama:** fase 1 completada y verificada en su SHA correspondiente; fase 2 tiene un primer incremento estructural, sin satisfacer aún su gate de salida. Las ocho fases no están completadas y no hay backend promovido al producto. Este documento define alcance y criterios; no declara capacidades ausentes.

## Principios y límites

- El trabajo parte de los componentes experimentales existentes (`IRProgram`, `InstrOpcode`, assembler y VM), pero no los confunde entre sí ni los presenta como una ruta integrada. Se deben reconciliar antes de promoverlos.
- La bytecode portable es un formato de Milena versionado, no código máquina. PE/ABI de Windows, ELF/ABI de Linux y Bionic/ABI de Android/Termux son objetivos nativos distintos.
- La ejecución interpretada segura y el fallback VM siguen disponibles cuando AOT/JIT no están disponibles. Código ejecutable debe respetar W^X. No se promoverá el GC actual sin raíces y ownership revisados.
- Los módulos experimentales permanecen fuera del producto canónico hasta integrar y probar explícitamente cada frontera. GPU es un backend futuro separado y no forma parte de esta entrega.

## Ocho fases y gates de salida

1. **Lexer y spans.** Tokens con rangos de fuente inequívocos, diagnóstico en posición inicial, reglas numéricas/escape definidas y pruebas de límites, errores y limpieza/estado. El lexer conserva offsets de bytes semiabiertos y coordenadas exclusivas; el parser copia rangos de tokens al AST para expresiones, declaraciones/operaciones cubiertas y diagnósticos semánticos. El signo del exponente se separa del operador siguiente, los exponentes mal formados y números no finitos se rechazan, y los escapes no admitidos generan error. El AST existente sigue siendo genérico; su rediseño estructurado y la HIR pertenecen a la fase 2.
2. **AST estructurado, semántica y HIR.** Sustituir el AST genérico por nodos estructurados con spans; agregar comprobación semántica tipada y una HIR explícita. Conservar diagnósticos y demostrar compatibilidad del frontend canónico mediante pruebas end-to-end.
3. **IR, opcode y assembler.** Inventariar el IR experimental y el enum de instrucciones; definir una única semántica de operandos, tipos y control de flujo. El ensamblado debe preservar valores, cadenas, tipos y errores, no mapear operaciones distintas a cargas genéricas ni convertir números a enteros sin semántica.
4. **Bytecode portable.** Especificar formato binario con magic, versión, enteros de ancho fijo y endianess definido; serializador/deserializador con límites, errores de truncación y verificador de instrucciones, operandos, saltos y recursos. Pruebas de round-trip y fuzzing/boundaries.
5. **VM y memoria por ejecución.** Interpretar bytecode verificado de la ruta canónica y definir ownership de todos los valores y recursos. Proveer un administrador de memoria/arena por ejecución con fallos controlados, limpieza en éxito/error/cancelación y pruebas de límites; no reutilizar GC sin roots seguros.
6. **AOT nativo.** Backend sin stubs para targets validados Windows PE, Linux ELF y Android/Termux Bionic. Cada target necesita compilación y ejecución reales, ABI documentada, pruebas de equivalencia frente a VM y fallback preservado.
7. **JIT.** Emisión y ejecución real con permisos de memoria por target, transición W^X, invalidación/liberación y fallback seguro. Probar ciclo de vida, errores y paridad semántica; no declarar JIT sobre una API de reserva solamente.
8. **Integración industrial.** Conectar los componentes al producto canónico detrás de una frontera explícita, optimizar solo con mediciones reproducibles, probar multiplataforma y publicar artefactos/evidencia de release. CI debe estar verde en el SHA exacto revisado; gates de dispositivo Termux requieren un runner o dispositivo real.

## Contrato de spans del lexer (incremento actual)

`Token.start_offset` y `Token.end_offset` son offsets **de bytes** en el buffer fuente, con intervalo semiabierto `[start_offset, end_offset)`. `line`/`column` identifican el inicio, y `end_line`/`end_column` el final exclusivo. Las líneas y columnas empiezan en 1; por ahora las columnas cuentan bytes y LF avanza a la línea siguiente. El `lexeme` de cadena es el contenido decodificado, mientras el span cubre también las comillas y escapes en el texto fuente. EOF tiene un span vacío en el final de la fuente. Comentarios y espacios se omiten y no se emiten como tokens.

Los números admiten dígitos, una fracción decimal y un exponente decimal opcional (`e`/`E` con signo opcional). Un exponente sin dígitos y valores no finitos/fuera de rango se reportan como errores. El signo unario es el token operador separado; por ejemplo, `1e-2-3` produce un número `1e-2`, un operador menos y un número `3`. Las cadenas admiten `\\n`, `\\t`, `\\r`, `\\\\` y `\\"`; un escape desconocido es un error, no se elimina silenciosamente.

Los nodos AST que se construyen desde esos tokens exponen `has_source_span`, offsets semiabiertos y coordenadas de inicio/final exclusivo. Esta pasada fija rangos en expresiones numéricas/identificadores y operaciones aritméticas, declaraciones/asignaciones de variables, llamadas estadísticas, bloque de análisis y funciones numéricas; los nodos contenedores también agregan los rangos de sus hijos. Los diagnósticos de símbolos estadísticos inexistentes apuntan al argumento original, aunque el parser ya haya avanzado al delimitador.

La fase 1 no rediseña el AST genérico ni implementa HIR; eso es alcance de la fase 2. La validación de ejecución se realiza en el CI asociado al SHA exacto (GCC, Clang, parser y ASan/UBSan), no mediante afirmaciones basadas en la compilación local inexistente.

## Fase 2 — incremento estructural inicial (incompleta)

`ast_validate` ofrece ahora una comprobación explícita y no mutante de un árbol AST ya construido: tipo de nodo dentro del enum, padre correcto (incluido el padre nulo de la raíz), almacenamiento coherente de hijos, ausencia de nodos nulos o compartidos/ciclos, consistencia básica de spans y contención de los spans hijos. Usa una pila iterativa y un conjunto de identidad para detectar estructuras repetidas sin depender de recursión del validador; un fallo de asignación se devuelve como error, no se interpreta como AST válido. La función no toma ownership y no sustituye validación sintáctica ni semántica.

Este incremento preserva las estructuras genéricas y los campos heredados del AST, y agrega pruebas de árbol válido, spans, enlaces padre-hijo, tipos fuera de rango, almacenamiento de hijos, nodos repetidos y raíz nula; el test del parser recorre la misma API pública. Está deliberadamente marcado **incompleto**: aún no introduce nodos tipados/estructurados, reglas de semántica tipada ni una HIR, ni satisface el gate de salida de fase 2. Es una base para identificar invariantes antes de migrar consumidores.

### Incremento de AST tipado escalar (aún no cierra la fase 2)

La rama conserva el AST legado para compatibilidad y agrega anotaciones explícitas `ASTValueType` (número, booleano, texto, arreglo/dataset y sin resolver) y `ASTOperatorKind`. Las operaciones binarias exponen `left_operand`/`right_operand` como alias no propietarios de `children[0]`/`children[1]`; el validador verifica aridad, enum, correspondencia entre el operador estructurado y el spelling heredado, y consistencia de los alias. El analizador semántico de la ruta canónica recorre expresiones escalares, anota literales, identificadores, arreglos, llamadas y operaciones, rechaza aritmética con booleanos y entrega error de tipo con el span del operador. Los operadores de texto se conservan por compatibilidad del intérprete, pero la comprobación semántica ya no depende de volver a dividir la cadena para validar estructura.

Las pruebas de frontend cubren la ruta canónica, anotaciones numéricas, operandos estructurados, rechazo de una operación de tipos incompatibles con posición y limpieza del AST en error; también ejecutan pruebas del intérprete para detectar regresiones. Este es un primer tipado, **no una HIR** y tampoco una migración del conjunto completo de operaciones/datos a nodos estructurados.

### Resolución de nombres del subconjunto de funciones numéricas (incremento en progreso)

La ruta canónica que recibe programas de funciones numéricas ahora recolecta firmas antes de analizar cuerpos, permite llamadas adelantadas, verifica nombre y aridad, y asigna IDs semánticos no propietarios a declaraciones, parámetros, usos, asignaciones y llamadas. La resolución de variables es léxica: parámetros y declaraciones locales solo son visibles dentro de su función/ámbito; las declaraciones duplicadas en el mismo ámbito, los usos/asignaciones sin binding, las asignaciones de tipo distinto y los argumentos no numéricos se rechazan con ubicación del nodo fuente. El contrato que se valida aquí es deliberadamente el ABI numérico actual: parámetros y retornos de funciones son numéricos; las comparaciones conservan tipo booleano y se aceptan en condiciones, no se coercionan implícitamente a argumentos/retornos numéricos. Los IDs viven en anotaciones del AST y todavía no forman un grafo HIR poseído por `MilenaCanonicalProgram`.

Este alcance es solo el sublenguaje escalar de programas de funciones. El análisis de tablas sigue preservando su AST legado: identificadores de columnas no equivalen a bindings de variables escalares, y sus data operations aún no tienen nodos HIR tipados ni contratos de operandos HIR. Tampoco se declara tipado completo del lenguaje canónico. Las pruebas e2e agregadas verifican llamadas adelantadas y enlaces, nombres desconocidos, aridad, aislamiento de ámbito, incompatibilidad de asignaciones, rechazo de argumentos booleanos y limpieza del AST no publicado ante cada error; no sustituyen pruebas de ejecución/paridad para una futura HIR.

Siguen pendientes el modelo tipado estructurado por constructo para todo el lenguaje canónico (incluyendo data operations y contratos de operandos), resolución de bindings de columnas/datasets/funciones para esos constructos, inferencia/coerción consistente más allá del subconjunto numérico documentado, HIR explícita poseída e integrada en `MilenaCanonicalProgram`, y pruebas e2e de equivalencia/errores/cleanup de esa HIR. Por tanto la fase 2 sigue abierta y no se debe iniciar fase 3.

