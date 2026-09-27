# Contratos normativos de las formas AST tabulares pendientes

Este documento cierra el contrato público de sintaxis de las 16 formas AST enumeradas aquí, con base en el parser, la semántica y las rutas de runtime/compilación que existen en la revisión que incorpora este archivo. «Contrato documentado» significa que se fija qué AST produce la sintaxis, qué significa y qué límites/error aplican; **no** significa que la forma esté completamente bajada a la IR tipada, expuesta por el ejecutable o liberada. Los estados de soporte por ruta se especifican en cada contrato.

Los nombres de AST de este documento son identificadores internos, no sintaxis para el usuario. Los textos citados son la sintaxis fuente observada y normativa. Los errores de análisis o validación deben fallar cerrados: nunca deben convertir una forma desconocida o inválida en una operación aparentemente exitosa.

## Bloques tabulares

### `AST_BLOQUE_LIMPIAR`

- **Sintaxis:** `.limpiar [dataset] { #nulos("eliminar") #duplicados("eliminar") }`. `dataset` es un marcador opcional. El cuerpo admite una o más órdenes `#nulos` y/o `#duplicados`, en el orden de la fuente; no se requiere separador entre órdenes. No se admiten órdenes desconocidas. Un bloque vacío es inválido.
- **Forma AST:** un nodo `AST_BLOQUE_LIMPIAR` bajo `AST_BLOQUE_ANALISIS`; cada hijo es un único `AST_COMANDO_NULOS` o `AST_COMANDO_DUPLICADOS`, con `value == "eliminar"`. Los nodos de orden conservan span cuando el parser lo proporciona.
- **Semántica:** aplica cada comando sucesivamente sobre la tabla actual. La eliminación de nulos descarta la fila si cualquier columna es nula. La eliminación de duplicados compara la fila completa y conserva la primera ocurrencia; el orden relativo de las filas retenidas se conserva.
- **Restricciones y errores:** la única acción normativa es el literal exacto `eliminar`; acción distinta, texto de comando inválido o bloque vacío es error y no debe llegar a ejecución. La igualdad de valores sigue el tipo almacenado: nulo equivale a nulo, texto por valor, categórico por código y valores respaldados por array por sus bytes almacenados. No hay conversión de tipos.
- **Recursos:** procesa una tabla residente en memoria y puede construir una tabla resultado; tamaño de entrada, asignaciones disponibles y overflow son límites operativos. Un fallo de memoria/datos deja la tabla de entrada sin publicación parcial.
- **Alias/compatibilidad:** no hay alias documentado para estas órdenes.
- **Estado:** parser y runtime tabular tipado implementan ambas operaciones; la HIR de datos las representa. No están bajadas de forma completa a la IR tipada/bytecode/VM y no equivalen a cobertura AST completa.

### `AST_COMANDO_NULOS`

- **Sintaxis:** únicamente `#nulos("eliminar")`, como hijo de `.limpiar`.
- **Forma AST:** hoja `AST_COMANDO_NULOS`; `value == "eliminar"`; sin hijos. El span apunta a la orden cuando está disponible.
- **Semántica:** elimina toda fila que contenga un nulo en cualquier columna; no imputa ni sustituye valores.
- **Restricciones y errores:** la acción es exacta y sensible a la ortografía; cualquier otro texto falla cerrado. Se conserva la estructura y los tipos de las columnas.
- **Recursos:** operación sobre tabla en memoria; el resultado respeta el límite impuesto por memoria disponible y no publica una tabla parcial en caso de error.
- **Alias/compatibilidad:** ninguno documentado.
- **Estado:** ejecutado por runtime de tabla tipada y representado por HIR de datos; sin lowering completo a IR/bytecode/VM.

### `AST_COMANDO_DUPLICADOS`

- **Sintaxis:** únicamente `#duplicados("eliminar")`, como hijo de `.limpiar`.
- **Forma AST:** hoja `AST_COMANDO_DUPLICADOS`; `value == "eliminar"`; sin hijos. El span apunta a la orden cuando está disponible.
- **Semántica:** elimina filas exactamente iguales en todas sus columnas y conserva la primera fila de cada grupo igual. Nulos son iguales entre sí; textos se comparan por contenido; categóricos por código; celdas respaldadas por array por los bytes almacenados.
- **Restricciones y errores:** la acción es exacta; no hay comparación aproximada, clave parcial ni selección de columnas. Otros textos fallan cerrado.
- **Recursos:** el runtime materializa/recorre una tabla en memoria; se limita por memoria disponible y debe evitar publicar resultado parcial al fallar.
- **Alias/compatibilidad:** ninguno documentado.
- **Estado:** ejecutado por runtime de tabla tipada y representado por HIR de datos; sin lowering completo a IR/bytecode/VM.

### `AST_BLOQUE_TRANSFORMAR`

- **Sintaxis:** `.transformar [dataset] { #total("col_a * col_b") #periodo("mes de fecha") }`. `dataset` es opcional. El cuerpo admite comandos `#total` y `#periodo` en orden de fuente. El bloque vacío es una transformación identidad (no agrega ni modifica columnas).
- **Forma AST:** nodo `AST_BLOQUE_TRANSFORMAR` con hijos hoja `AST_COMANDO_TOTAL` y/o `AST_COMANDO_PERIODO`, que guardan en `value` el texto exacto entre comillas.
- **Semántica:** `#total` agrega la columna numérica `total` con el producto fila a fila de las columnas indicadas; los nulos se propagan. `#periodo` agrega la columna de texto `periodo` derivando `YYYY-MM` de la columna de texto `fecha`; los nulos se propagan.
- **Restricciones y errores:** cada orden se valida antes de ejecutarse. `total` exige exactamente dos nombres de columna, separados por un `*` independiente, sin tokens finales; las dos entradas deben existir y ser numéricas, `total` no debe existir ya, y un producto no finito/overflow falla. `periodo` solo acepta el payload exacto `mes de fecha`; exige columna de texto `fecha`, ausencia previa de `periodo` y fechas ISO `YYYY-MM-DD` con año 0001–9999 y día válido del calendario gregoriano (incluidas reglas de año bisiesto). Fechas con sufijo, ancho distinto, día/mes imposible o año 0000 fallan; no se normalizan.
- **Recursos:** opera sobre tablas en memoria y asigna columna de salida; la memoria disponible/overflow son límites operativos. Ante error no se publica una columna incompleta.
- **Alias/compatibilidad:** se conserva explícitamente la forma histórica `#periodo extraer("mes de fecha")`, que se normaliza al mismo `AST_COMANDO_PERIODO`; no crea `AST_COMANDO_EXTRAER`. El alias solo cambia la grafía, no amplía los valores admitidos.
- **Estado:** el runtime tabular ejecuta `#total` y `#periodo`; la HIR de datos del compilador admite `#total`, pero no baja `#periodo` y por eso la compilación HIR cerrada no cubre esa operación. Este documento no afirma que el bloque completo se baje a IR/bytecode/VM.

### `AST_COMANDO_TOTAL`

- **Sintaxis:** `#total("col_a * col_b")`, dentro de `.transformar`.
- **Forma AST:** hoja `AST_COMANDO_TOTAL`, `value` igual al payload de la cadena, sin hijos.
- **Semántica:** produce la columna numérica `total = col_a × col_b` en cada fila. Si cualquiera de las entradas es nula, la salida de la fila es nula.
- **Restricciones y errores:** el payload debe describir exactamente dos tokens de columna y un operador `*`, sin operación general, paréntesis, tercer token ni sufijo. Ambas columnas deben existir y tener dtype numérico; se rechaza una columna de salida `total` ya existente. Overflow o resultado no finito es error de tipo/overflow; no se trunca.
- **Recursos:** un valor de salida por fila, memoria acorde a la tabla; asignación fallida debe reportarse como error.
- **Alias/compatibilidad:** ninguno documentado.
- **Estado:** se ejecuta en runtime tabular y se representa como producto en HIR de datos. No implica gramática de expresiones aritméticas libre ni lowering completo a IR/bytecode/VM.

### `AST_COMANDO_PERIODO`

- **Sintaxis:** `#periodo("mes de fecha")`, dentro de `.transformar`.
- **Forma AST:** hoja `AST_COMANDO_PERIODO`, con `value` igual al texto entre comillas y sin hijos. La forma histórica con `extraer` produce exactamente este mismo tipo de nodo.
- **Semántica:** lee `fecha` (texto) y agrega texto `periodo`, con año y mes `YYYY-MM`; conserva nulos fila a fila.
- **Restricciones y errores:** payload literal exacto `mes de fecha`; columna `fecha` de texto; columna de salida `periodo` ausente. Cada valor no nulo debe ser ISO `YYYY-MM-DD`, con dígitos ASCII y fecha real del calendario gregoriano, año 0001–9999; se rechazan fechas inválidas, sufijos y espacios. No se infiere ni se convierte otro formato.
- **Recursos:** columna derivada residente en memoria; errores de validación o asignación no dejan salida parcial.
- **Alias/compatibilidad:** `#periodo extraer("mes de fecha")` es alias histórico explícito y se normaliza a este nodo. Ninguna otra forma de `extraer` queda habilitada.
- **Estado:** runtime tabular implementa el cálculo y la validación estricta del calendario; el constructor actual de HIR de datos no baja `AST_COMANDO_PERIODO`. No está disponible como lowering completo de IR/bytecode/VM.

### `AST_BLOQUE_FILTRAR`

- **Sintaxis:** `.filtrar [dataset] { #condicion("columna operador número") }`. La variante de bloque nombrado `.filtrar { #condicion("...") }` produce el mismo AST normalizado. El cuerpo admite exactamente una condición.
- **Forma AST:** nodo `AST_BLOQUE_FILTRAR` con exactamente un hijo `AST_COMANDO_CONDICION`, cuyo payload tipado incluye columna, operador y umbral finito. El parser debe rechazar predicados no analizables y cardinalidad distinta de uno, incluso si podría crear un nodo sintáctico sin ellos.
- **Semántica:** retiene, sin reordenarlas, las filas cuyo valor numérico de columna satisface la comparación. Las filas con nulo no coinciden. El resultado conserva columnas y tipos.
- **Restricciones y errores:** columna es un token no vacío sin espacios; operador uno de `==`, `!=`, `>`, `>=`, `<`, `<=`, con espacios alrededor del operador; umbral es un único número finito aceptado por el conversor numérico, sin texto final. La columna debe existir y ser numérica. No admite predicados textuales, expresiones, conjunciones ni filtros múltiples. Un nodo sin payload de predicado tipado es inválido.
- **Recursos:** filtra una tabla en memoria y materializa el resultado; memoria/overflow pueden producir error sin salida parcial.
- **Alias/compatibilidad:** las dos grafías de bloque citadas comparten semántica; no hay alias de predicado documentado.
- **Estado:** runtime tabular y HIR de datos implementan este predicado numérico limitado. No debe confundirse con `AST_STREAM_FILTER`, el predicado distinto de CSV/Arrow streaming. No hay lowering completo a IR/bytecode/VM.

### `AST_COMANDO_CONDICION`

- **Sintaxis:** únicamente `#condicion("columna operador número")` como hijo de un bloque `.filtrar` admitido.
- **Forma AST:** hoja `AST_COMANDO_CONDICION`; conserva el payload original en `value` y, solo si el parseo tiene éxito, establece `has_filter_predicate`, `filter_column`, `filter_operator` y `filter_threshold`.
- **Semántica:** condición numérica tipada que utiliza exclusivamente una columna, uno de seis operadores de comparación y un umbral numérico finito; su evaluación es la especificada para `AST_BLOQUE_FILTRAR`.
- **Restricciones y errores:** una cadena libre que no cumpla esa gramática no es un predicado válido y debe fallar en parser/validación; nunca se acepta solo porque el parser pueda construir una hoja AST. El campo/columna debe existir y ser numérico al ejecutar. No equivale a `AST_STREAM_FILTER`.
- **Recursos:** no crea una tabla por sí sola; hereda los requisitos de materialización del bloque que la contiene.
- **Alias/compatibilidad:** ninguno.
- **Estado:** normalización tipada en parser tras la corrección fail-closed; HIR de datos/runtime ejecutan el predicado limitado. Sin lowering completo a IR/bytecode/VM.

## Roles de esquema y selección

### `AST_DECLARACION_ENTRADA`

- **Sintaxis:** `entrada categorica "columna"`; punto y coma final opcional, dentro de `.analisis`.
- **Forma AST:** hoja `AST_DECLARACION_ENTRADA`, `value` con el nombre de columna y `type_name == "categorica"`; sin hijos.
- **Semántica:** declara la columna con rol de entrada categórica. No crea por sí misma una columna, no convierte valores y no infiere un modelo.
- **Restricciones y errores:** el tipo literal admitido es exactamente `categorica`; el nombre debe ser no vacío, existir en el esquema/dataset consumido y no entrar en conflicto con otra declaración de esquema. No se extiende a otros tipos por similitud de nombre.
- **Recursos:** metadato de esquema; no transforma las celdas. Errores de esquema rechazan la ejecución.
- **Alias/compatibilidad:** ninguno documentado.
- **Estado:** parser y runtime de lenguaje mapean el nodo a rol categórico de entrada en el esquema. El constructor actual de HIR de datos no consume este nodo y no se afirma lowering canónico.

### `AST_DECLARACION_SALIDA`

- **Sintaxis:** `salida binaria "columna"`; punto y coma final opcional, dentro de `.analisis`.
- **Forma AST:** hoja `AST_DECLARACION_SALIDA`, `value` con el nombre de columna y `type_name == "binaria"`; sin hijos.
- **Semántica:** declara la columna con rol de salida binaria. No convierte valores ni ejecuta estadística o entrenamiento.
- **Restricciones y errores:** el tipo literal admitido es exactamente `binaria`; el nombre debe ser no vacío, existir en el esquema/dataset consumido y no entrar en conflicto con otra declaración. No se admiten roles de salida de otros tipos.
- **Recursos:** metadato de esquema, sujeto a los límites ordinarios del esquema/memoria.
- **Alias/compatibilidad:** ninguno documentado.
- **Estado:** parser y runtime de lenguaje mapean el nodo a rol binario de salida. El constructor actual de HIR de datos no consume este nodo; no se afirma lowering canónico.

### `AST_BLOQUE_SELECCIONAR`

- **Sintaxis:** `.seleccionar { #columnas("a,b") }`; se permite separador final `,` o `;` según el parser. El bloque compilable contiene una sola orden `#columnas`.
- **Forma AST:** nodo `AST_BLOQUE_SELECCIONAR` con exactamente un hijo `AST_COMANDO_COLUMNAS` y `value` no nulo. Cualquier forma AST con hijos extra o forma distinta es rechazada por la HIR de datos.
- **Semántica:** proyecta las columnas solicitadas, en el orden listado, conservando sus tipos y filas.
- **Restricciones y errores:** los nombres deben cumplir el contrato de `AST_COMANDO_COLUMNAS`; al resolver contra la tabla, una columna ausente es error. El parser puede formar AST que la HIR de datos posteriormente rechaza; una AST sintáctica por sí sola no prueba que se compiló.
- **Recursos:** crea una tabla de proyección en memoria; se limita por el tamaño de entrada/salida y memoria disponible.
- **Alias/compatibilidad:** ninguno documentado.
- **Estado:** selección implementada en HIR de datos/runtime tabular. Sin lowering completo a IR/bytecode/VM ni prueba de cobertura total de AST.

### `AST_COMANDO_COLUMNAS`

- **Sintaxis:** `#columnas("a,b")`, exclusivamente como el único comando del bloque seleccionar.
- **Forma AST:** hoja `AST_COMANDO_COLUMNAS`; `value` es la cadena completa no dividida; sin hijos.
- **Semántica:** la HIR divide por coma, recorta espacios al inicio/final de cada nombre y conserva el orden solicitado.
- **Restricciones y errores:** entre 1 y 32 nombres; cada uno no vacío y menor de 128 bytes; nombres únicos luego de recortar; sin comas finales ni elementos vacíos. Cada nombre debe resolver a una columna existente al ejecutar. Los nombres se interpretan literalmente, no como expresiones.
- **Recursos:** hasta 32 referencias de columnas, con nombres menores de 128 bytes; la tabla resultado depende del tamaño de datos y memoria disponible.
- **Alias/compatibilidad:** no hay nombres alternativos ni comodines.
- **Estado:** parser conserva el texto; la HIR de datos valida cardinalidad/nombres y el runtime selecciona. Sin lowering completo a IR/bytecode/VM.

## Proyección candidata Arrow IPC

### `AST_COLUMNAR_PROJECT`

- **Sintaxis:** `proyectar { "campo1", "campo2"; }`; se requiere al menos un campo. Los elementos se separan con coma o punto y coma; cada campo va entre comillas. El separador final es opcional.
- **Forma AST:** nodo `AST_COLUMNAR_PROJECT`, con hijos `AST_COLUMNAR_FIELD` en orden de fuente; sin subárboles adicionales.
- **Semántica:** declara la proyección ordenada del plan Arrow IPC; no ejecuta por sí sola lectura ni escritura.
- **Restricciones y errores:** el plan validado exige una fuente local `arrow_ipc_stream`, una proyección, un único sink de salida, como máximo un filtro tipado y límites explícitos de recursos. Hay hasta 128 campos proyectados, únicos y declarados en el esquema como `numerica` o `texto`. Una forma fuera del plan cerrado falla; no se extrapola a otros formatos o pipelines.
- **Recursos:** la ejecución candidata está sujeta a límites explícitos de memoria, lote, filas, bytes y tiempo definidos en `docs/ARROW_IPC_STREAM_PROFILE.md`.
- **Alias/compatibilidad:** no hay alias.
- **Estado:** el parser y el plan/HIR candidato reconocen la proyección, pero el perfil Arrow permanece WIP, no verificado y no liberado. No se declara sintaxis compilable soportada por el producto, runtime público ni lowering a IR/MLBC/VM.

### `AST_COLUMNAR_FIELD`

- **Sintaxis:** un único campo de la lista de `proyectar`, como literal no vacío entre comillas.
- **Forma AST:** hoja `AST_COLUMNAR_FIELD`; `value` conserva el nombre de campo literal; sin hijos. Su posición define el orden de proyección.
- **Semántica:** referencia a una columna incluida por `AST_COLUMNAR_PROJECT`; no ejecuta una operación aislada.
- **Restricciones y errores:** hasta 128 referencias proyectadas por el plan, sin duplicados; cada campo debe estar declarado `numerica` o `texto`. Campo vacío, repetido, no declarado o de otro dtype rechaza el plan. La fuente y límites deben cumplir el perfil Arrow WIP.
- **Recursos:** hereda límites de ejecución del plan Arrow y longitud/memoria del esquema; no añade límites independientes no definidos en el perfil.
- **Alias/compatibilidad:** ninguno.
- **Estado:** nodo candidato sujeto al estado WIP/no verificado/no liberado de `AST_COLUMNAR_PROJECT`; no implica soporte público ni lowering canónico.

## Marcadores del análisis

### `AST_DECLARACION_DATOS`

- **Sintaxis:** `#datos`, sin argumentos, dentro de `.analisis`.
- **Forma AST:** nodo vacío `AST_DECLARACION_DATOS`, sin `value`, `type_name` ni hijos; el parser no lo convierte en una carga/dataset.
- **Semántica:** marcador declarativo neutro reservado para etiquetar el contexto de datos. No carga, declara, limpia ni transforma datos; no genera cálculo ni efecto de runtime.
- **Restricciones y errores:** no admite argumentos, payload ni hijos. No debe inferirse una operación por el nombre del marcador. Cualquier operación de datos debe aparecer con su propia construcción contractual.
- **Recursos:** cero trabajo de datos y sin asignación de tabla.
- **Alias/compatibilidad:** sin alias documentado; se conserva como marcador legado mientras siga en la gramática.
- **Estado:** parser lo construye y el pase semántico legado lo acepta; el constructor de HIR de datos y lowering a IR no lo consumen. Runtime no realiza operación para el marcador.

### `AST_DECLARACION_ESTADISTICA`

- **Sintaxis:** `#estadistica`, sin argumentos, dentro de `.analisis`.
- **Forma AST:** nodo vacío `AST_DECLARACION_ESTADISTICA`, sin `value`, `type_name` ni hijos.
- **Semántica:** marcador declarativo neutro de contexto estadístico. No ejecuta un análisis, no crea métricas y no implica que las operaciones estadísticas existentes estén disponibles.
- **Restricciones y errores:** sin argumentos, payload o hijos. No se confunde con `AST_OPERACION_ESTADISTICA`, que es otro nodo con contrato independiente.
- **Recursos:** no consume datos ni crea resultados estadísticos.
- **Alias/compatibilidad:** no hay alias documentado.
- **Estado:** parser y pase semántico legado lo aceptan; no lo baja el constructor actual de HIR de datos y no tiene ejecución estadística asociada.

## Límite de esta decisión

La clasificación como contrato de sintaxis documentado no altera los estados por variante de lowering, semántica verificada o pruebas end-to-end del ledger. Las cuatro reservas enum-only —`AST_ASIGNACION_DATASET`, `AST_BLOQUE_VISUALIZAR`, `AST_EXPRESION_FUNCION` y `AST_COMANDO_EXTRAER`— se mantienen como slots internos fuera del lenguaje fuente y no requieren lowering. La sintaxis `.visualizar` se rechaza explícitamente para impedir que el parser quede sin avanzar; `#periodo extraer(...)` continúa normalizándose a `AST_COMANDO_PERIODO`. No se reordenan ni eliminan valores del enum; cualquier eliminación futura requiere revisión versionada de compatibilidad. Las pruebas de parser/runtime acreditan solo los casos concretos cubiertos. La fase 1 sigue incompleta: faltan lowering fiel de la superficie oficial, pruebas diferenciales completas, errores/spans/ownership/recursos por construcción y los gates sobre el SHA exacto. Los contratos de marcador y Arrow WIP son límites explícitos de no ejecución/no soporte, no promesas de implementación.
