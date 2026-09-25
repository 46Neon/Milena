# Compilador de Milena: plan normativo en dos fases

**Estado:** plan y contrato de aceptación. Este documento no afirma que la IR universal ni la compilación nativa ya estén implementadas. Base auditada: `main` en `694512c99c3add66dca8405b7f2144adb9983490`; PR #33 aporta actualmente la base de spans y diagnósticos del lexer.

La entrega completa se organiza en exactamente dos fases. Las tareas internas son criterios de salida, no fases adicionales.

## Diagnóstico de la base

- El ejecutable oficial de Milena se construye desde fuentes C17 con el compilador C de la plataforma. Eso compila la implementación de Milena, no cada programa `.milena` a código nativo.
- El producto ya integra lexer, parser, AST, validación semántica y runtime. `canonical_compiler.c` construye y ejecuta una HIR de datos acotada; no emite bytes de instrucciones nativas.
- `compiler.c`, `ir.c`, `assembler.c` y `vm.c` contienen prototipos experimentales aislados del binario oficial. El IR secuencial y el ensamblador actuales no representan aún todas las construcciones ni un ISA/ABI de Windows, Linux o Android.
- El repositorio contiene capacidades de arrays, tablas, estadísticas, CSV, flujo, SQLite y Arrow. Cada capacidad solo cuenta como lenguaje compilable cuando su sintaxis y semántica tienen una representación completa en la ruta canónica y pruebas de equivalencia; una biblioteca C disponible no basta.

## Regla de arquitectura

La única ruta canónica de un programa será:

`fuente Milena → lexer → AST tipado → análisis semántico → IR Milena verificada → backend → ejecutable nativo`

No se añadirá otro parser, un segundo IR competidor ni un runtime alternativo. La VM de referencia de la fase 1 y el backend nativo de la fase 2 consumirán el mismo contrato IR. Se puede conservar compatibilidad durante la migración, pero ninguna sintaxis oficialmente soportada podrá quedar indefinidamente en una ruta no compilable.

## Fase 1 — frontend completo e IR Milena de altas capacidades

**Objetivo:** representar sin pérdida semántica todo el lenguaje oficialmente soportado en una IR propia, tipada, versionada, portable y verificable. Al cierre de esta fase, los programas se podrán serializar como bytecode portable y ejecutar en una VM de referencia; ese bytecode no es código máquina de la CPU.

1. **Cerrar el inventario del lenguaje.** Crear una matriz exhaustiva `token/sintaxis → AST → regla semántica → operación IR → ejecución de referencia → pruebas`. Incluir funciones, expresiones, control de flujo, arrays, datasets/tablas, estadísticas, entrada/salida y capacidades de flujo que formen parte del lenguaje público. Cada nodo y variante AST queda soportado o se elimina/depreca explícitamente con una política de versión; nunca se omite por un `default` recursivo que finja cobertura.
2. **Completar el frontend antes del lowering.** Consolidar los spans y diagnósticos del lexer, estructurar los nodos AST por constructo, y tipar/resolver símbolos, funciones, columnas, operaciones, nulabilidad y errores. La transformación a IR debe consumir payloads AST tipados, no volver a interpretar cadenas de comandos ni reparsear el texto de origen.
3. **Definir una IR canónica de una sola ruta.** Usar valores tipados y operaciones con operandos explícitos, IDs estables, bloques y flujo de control verificable, tipos y firmas de funciones, constantes, llamadas y retornos. Conservar spans de origen, efectos, contratos de error, ownership/lifetimes y límites de recursos. Representar arrays, tablas, CSV/streaming y operaciones estadísticas con instrucciones de dominio cuando ello evite destruir oportunidades de optimización; bajar esas instrucciones mediante pases definidos a las operaciones escalares y de control de la misma IR, no a un motor paralelo.
4. **Especificar y verificar el bytecode portable.** Cabecera mágica y versión explícita; enteros de ancho fijo y endianess definidos; ningún puntero nativo serializado. El lector impone límites antes de reservar memoria y rechaza truncamiento, IDs/tipos inválidos, firmas incompatibles, bloques o saltos inválidos, valores sin definición, recursos excesivos y opcodes desconocidos. Añadir serialización determinista y pruebas round-trip, de límites y fuzzing.
5. **Implementar la VM de referencia sobre la IR verificada.** Ejecutar todas las operaciones admitidas con semántica idéntica al runtime actual. Definir la propiedad de cada valor y recurso, limpieza ante éxito/error/cancelación y propagación determinista de diagnósticos. La VM sirve como oráculo de equivalencia y fallback explícito; no sustituye al compilador nativo de la fase 2.

**Gate de salida de fase 1:** cobertura automática del 100 % de los constructos oficialmente soportados; ningún constructo con rechazo accidental, lowering silencioso o pérdida de semántica; verificador fail-closed; bytecode portable validado; pruebas diferenciales del runtime/VM sobre resultados, errores, límites de memoria y limpieza; CI verde en el SHA exacto con GCC, Clang y sanitizadores. La fase no se declara completa por tener un HIR parcial, una especificación escrita o pruebas solo estructurales.

## Contrato de la IR de altas capacidades

- **Tipos y valores:** escalares y valores nulos/optionales; arrays con dtype, forma y layout explícitos; tablas con esquema, tipos y nulabilidad; streams con fuente, modo, particionado y límites; funciones con parámetros/retorno tipados. La matriz semántica del lenguaje decide los tipos exactos: el backend no puede inferirlos de cadenas o nombres.
- **Operaciones:** constantes; conversión y aritmética tipada; comparaciones; bloques/ramas/ciclos; llamadas; lectura/escritura de bindings; errores; operaciones de arrays/reducciones; carga, filtro, proyección, transformación, agrupación, resumen y join de tablas; estadísticas; lectura/escritura y flujo. Cada opcode tiene operandos, tipos, efectos, errores, límites y reglas de ownership normativos.
- **Control y validación:** bloques con terminador explícito; cada valor se define una sola vez o por parámetro de bloque; los usos están dominados por su definición; las entradas/salidas de bloques concuerdan en tipo; toda rama apunta a un bloque existente; las funciones retornan según su firma. Ningún archivo serializado puede conseguir ejecución antes de superar el verificador.
- **Efectos y recursos:** las operaciones de archivos, reportes, estado y memoria son visibles en IR; límites de filas, columnas, bytes y memoria se validan como parte del contrato de ejecución, no como optimizaciones opcionales. El backend conserva el orden observable y no puede eliminar efectos.
- **Fuente y errores:** cada operación conserva span suficiente para señalar el origen Milena; los errores de parseo, tipo, datos, E/S, límite y runtime conservan categoría y ubicación a través de AST, IR, VM y binario nativo.

## Fase 2 — backend AOT directo a código máquina

**Objetivo:** convertir cada programa aceptado por la fase 1 en un ejecutable nativo por plataforma sin invocar LLVM, GCC, Clang ni otro compilador como backend del programa Milena.

1. Implementar selección de instrucciones, asignación de registros, frames de pila, llamadas y convenciones ABI, ramas, constantes, relocaciones y tratamiento de errores para cada target acordado.
2. El compilador Milena produce el código nativo y el formato final de ejecutable/objeto mediante componentes propios. Para que el artefacto no necesite un intérprete separado, las rutinas de soporte Milena utilizadas (memoria, arrays, tablas, CSV/flujo, estadísticas y E/S aplicables) se compilan o se enlazan estáticamente dentro del artefacto. El camino AOT no admite fallback silencioso a interpretación.
3. Validar targets explícitos: Windows/PE y ABI Windows, Linux/ELF y ABI Linux, Android/Termux/ELF y Bionic, en las arquitecturas acordadas. Termux debe validarse en Android real; un host Ubuntu no es evidencia de Bionic.
4. Comparar cada binario nativo contra la VM de referencia usando la misma batería de programas, entradas, errores y límites. Medir RAM, CPU e I/O con workloads reproducibles antes de anunciar rendimiento o escalabilidad.

**Gate de salida de fase 2:** todo constructo de la fase 1 genera código nativo o falla con diagnóstico; ninguna ruta ejecuta el intérprete en modo AOT; los binarios corren en cada target real; resultados, errores y límites concuerdan con la VM; artefactos y CI quedan vinculados al SHA validado. Si se anuncia procesamiento masivo, añadir las pruebas de escala y recursos correspondientes en Linux, Windows y Android/Termux.

## Límite de la promesa “sin otro componente”

El programa `.milena` no debe necesitar un compilador externo, LLVM ni una VM/intérprete separado para generar y ejecutar el binario AOT; el runtime Milena requerido puede ir incorporado estáticamente en el artefacto. Un ejecutable nativo sigue necesitando el cargador, el kernel y las interfaces del sistema operativo. El compilador actual está escrito en C: reconstruir el propio compilador desde fuentes seguirá necesitando un toolchain C mientras no se emprenda explícitamente un proyecto de autoalojamiento, que no forma parte de estas dos fases.
