# Plan industrial del compilador de Milena

**Estado de esta rama:** fundamento de la fase 1 en revisión de CI; las ocho fases no están completadas y no hay backend promovido al producto. Este documento define alcance y criterios; no declara capacidades ausentes.

## Principios y límites

- El trabajo parte de los componentes experimentales existentes (`IRProgram`, `InstrOpcode`, assembler y VM), pero no los confunde entre sí ni los presenta como una ruta integrada. Se deben reconciliar antes de promoverlos.
- La bytecode portable es un formato de Milena versionado, no código máquina. PE/ABI de Windows, ELF/ABI de Linux y Bionic/ABI de Android/Termux son objetivos nativos distintos.
- La ejecución interpretada segura y el fallback VM siguen disponibles cuando AOT/JIT no están disponibles. Código ejecutable debe respetar W^X. No se promoverá el GC actual sin raíces y ownership revisados.
- Los módulos experimentales permanecen fuera del producto canónico hasta integrar y probar explícitamente cada frontera. GPU es un backend futuro separado y no forma parte de esta entrega.

## Ocho fases y gates de salida

1. **Lexer y spans.** Tokens con rangos de fuente inequívocos, diagnóstico en posición inicial, reglas numéricas/escape definidas y pruebas de límites, errores y limpieza/estado. El primer incremento introduce offsets de bytes semiabiertos y coordenadas exclusivas, notación exponencial finita y errores explícitos para escapes desconocidos. Aún falta validar el contrato completo en todos los consumidores y plataformas.
2. **AST estructurado, semántica y HIR.** Sustituir el AST genérico por nodos estructurados con spans; agregar comprobación semántica tipada y una HIR explícita. Conservar diagnósticos y demostrar compatibilidad del frontend canónico mediante pruebas end-to-end.
3. **IR, opcode y assembler.** Inventariar el IR experimental y el enum de instrucciones; definir una única semántica de operandos, tipos y control de flujo. El ensamblado debe preservar valores, cadenas, tipos y errores, no mapear operaciones distintas a cargas genéricas ni convertir números a enteros sin semántica.
4. **Bytecode portable.** Especificar formato binario con magic, versión, enteros de ancho fijo y endianess definido; serializador/deserializador con límites, errores de truncación y verificador de instrucciones, operandos, saltos y recursos. Pruebas de round-trip y fuzzing/boundaries.
5. **VM y memoria por ejecución.** Interpretar bytecode verificado de la ruta canónica y definir ownership de todos los valores y recursos. Proveer un administrador de memoria/arena por ejecución con fallos controlados, limpieza en éxito/error/cancelación y pruebas de límites; no reutilizar GC sin roots seguros.
6. **AOT nativo.** Backend sin stubs para targets validados Windows PE, Linux ELF y Android/Termux Bionic. Cada target necesita compilación y ejecución reales, ABI documentada, pruebas de equivalencia frente a VM y fallback preservado.
7. **JIT.** Emisión y ejecución real con permisos de memoria por target, transición W^X, invalidación/liberación y fallback seguro. Probar ciclo de vida, errores y paridad semántica; no declarar JIT sobre una API de reserva solamente.
8. **Integración industrial.** Conectar los componentes al producto canónico detrás de una frontera explícita, optimizar solo con mediciones reproducibles, probar multiplataforma y publicar artefactos/evidencia de release. CI debe estar verde en el SHA exacto revisado; gates de dispositivo Termux requieren un runner o dispositivo real.

## Contrato de spans del lexer (incremento actual)

`Token.start_offset` y `Token.end_offset` son offsets **de bytes** en el buffer fuente, con intervalo semiabierto `[start_offset, end_offset)`. `line`/`column` identifican el inicio, y `end_line`/`end_column` el final exclusivo. Las líneas y columnas empiezan en 1; por ahora las columnas cuentan bytes y LF avanza a la línea siguiente. El `lexeme` de cadena es el contenido decodificado, mientras el span cubre también las comillas y escapes en el texto fuente. EOF tiene un span vacío en el final de la fuente. Comentarios y espacios se omiten y no se emiten como tokens.

Los números admiten dígitos, una fracción decimal y un exponente decimal opcional (`e`/`E` con signo opcional). Un exponente sin dígitos y valores no finitos/fuera de rango se reportan como errores. El signo unario es el token operador separado. Las cadenas admiten `\\n`, `\\t`, `\\r`, `\\\\` y `\\"`; un escape desconocido es un error, no se elimina silenciosamente.

Este contrato todavía necesita revisión con la gramática completa y validación bajo sanitizers y CI del repositorio. El resultado del CI asociado a esta rama, no una compilación local inexistente, es la evidencia de ejecución.