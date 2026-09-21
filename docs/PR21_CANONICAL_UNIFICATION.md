# PR21: estado de la unificación canónica

Este documento registra el estado verificable de la migración iniciada desde
`main` para el PR21. No declara completa la reingeniería: separa lo que ya
está implementado de los trabajos que siguen abiertos.

## Superficie oficial

La ruta oficial es `milena run <archivo.milena>` y debe recorrer lexer ->
parser -> AST -> semántica -> runtime. `src/script.c` puede clasificar una
fuente como canónica o histórica, pero la ruta histórica está encapsulada y no
puede recibir capacidades nuevas. Los módulos IR/VM/GC, compiler, assembler,
forest y demás módulos experimentales permanecen fuera del binario oficial.

`Dataset` sigue siendo el formato de entrada/salida de compatibilidad. La
representación tabular canónica es `MilenaTable`; `milena_dataset_from_table`
realiza una conversión explícita y reemplaza el destino solo después de
construir una conversión completa. El destino debe estar inicializado por el
llamante, como en el resto de la API `Dataset`.

Una fuente con bloques de dataset canónicos (`dataset cargar`, `.limpiar` o
`.transformar`) o con delimitadores truncados se rechaza con el diagnóstico del
parser y nunca cae a la ruta histórica. Las fixtures completas de arrays que
preceden a la gramática AST se conservan como compatibilidad explícita; esa
ruta no añade capacidades y sus errores de ejecución siguen siendo reportados
por el ejecutor de arrays.

## Contrato de ownership verificable

- `dataset_init` y `milena_table_init` establecen el estado que debe recibir
  cada destructor; `dataset_destroy` y `milena_table_destroy` son idempotentes
  sobre ese estado y liberan todos los strings, máscaras, diccionarios,
  metadata y storage de arrays que posee su descriptor.
- Las funciones `*_add_*_copy`, `milena_table_clone` y las conversiones
  `milena_table_from_dataset`/`milena_dataset_from_table` copian los datos que
  instalan. El llamante conserva y puede liberar inmediatamente sus arrays,
  strings, diccionarios y el origen después de un retorno exitoso.
- Las operaciones que reciben `out` construyen un temporal y hacen commit solo
  al completar la operación. Ante un error, `out` conserva su valor anterior;
  la conversión inversa además exige un `Dataset` inicializado. La regresión
  `tests/test_pr21_regressions.c` cubre este contrato con un dtype complejo no
  convertible y verifica que no se pierde el dataset previo.
- Los arrays compartidos usan el contador de referencias de `MilenaArray`; una
  vista no transfiere ownership del origen. Las columnas de tabla materializan
  copias contiguas cuando la API lo exige, por lo que no retienen punteros a
  buffers externos.

## Cambios implementados en PR21

- El join no escribe la cadena `next` después de un fallo de reserva; la
  inicialización de esa estructura está protegida por la puerta de memoria.
- La conversión `MilenaTable -> Dataset` construye un temporal, libera el
  destino anterior al hacer commit y descarta completamente el temporal ante
  cualquier error. Esto evita fugas, reutilización de memoria vieja y
  datasets parcialmente observables.
- El escritor de strings JSON con escapes para comillas, barra inversa,
  control y saltos de línea vive en `common.c` y es usado por el serializer de
  `Dataset` y por las salidas SST que escriben nombres de columnas, métodos y
  etiquetas.
- El runtime SST propaga errores de lectura de celdas y rechaza dtypes no
  numéricos, en vez de producir estadísticas parciales o convertirlos
  silenciosamente a cero. Las conversiones numéricas aceptan los dtypes
  enteros y de coma flotante soportados por `MilenaTable`.
- El parser centraliza la conexión fallible de nodos AST en los puntos de
  expresión y programa, diagnostica comandos desconocidos en bloques de
  limpieza/transformación y deja de descartar tokens desconocidos en el
  bloque de análisis.
- `tests/test_table_worker4.c` está conectado al objetivo `make test` como
  regresión de la API tabular.

## Checklist pendiente

- [ ] Completar la API de valores común para escalares, arrays y tablas, con
      coerciones y tipos comprobados por semántica.
- [ ] Trasladar la superficie restante de transformaciones/selección/uniones
      al AST sin ampliar el router textual legado.
- [ ] Sustituir las salidas JSON formateadas restantes por un escritor común
      (en particular todas las operaciones SST que solo escriben números y
      booleanos deben conservar una política única de errores de I/O).
- [ ] Auditar y propagar cada resultado de `ast_add_child` en las ramas de
      funciones y en los constructores de bloques complejos.
- [x] Añadir pruebas de inyección de escapes JSON, todos los dtypes numéricos
      soportados, fallo de extracción por dtype y preservación del destino ante
      error de `MilenaTable -> Dataset` (`tests/test_pr21_regressions.c`).
- [x] Revisar el router: una fuente con marcadores canónicos y error del
      parser devuelve el diagnóstico y no cae silenciosamente a compatibilidad.
- [ ] Revisar todos los diagnósticos semánticos y garantizar que ninguna
      sintaxis no reconocida pueda caer silenciosamente a compatibilidad.
- [ ] Mantener la clasificación de fuentes experimentales y eliminar APIs
      duplicadas solo cuando exista una sustitución canónica con pruebas.

## Puerta de CI

Antes de fusionar PR21 deben pasar los checks requeridos por los workflows,
incluidos GCC, Clang, ASan/UBSan, Debian, Windows, enlaces de Markdown y
Cloudflare Pages. El gate local equivalente es `make test`; por el entorno de
trabajo de esta migración la compilación y la ejecución se delegan a GitHub
Actions.

## Auditoría de esta iteración (HEAD posterior)

- [x] Se revisaron todos los usos del conector AST en `src/parser.c`: los
  constructores de bloques, expresiones, funciones y transferencias de hijos
  ahora comprueban `ast_add_child` mediante el helper fallible del parser y
  abortan sin dejar un AST truncado.
- [x] La conversión numérica SST comparte una única lectura segura para bool,
  enteros con y sin signo y `float32/float64`; los dtypes complejos y valores
  no finitos se rechazan, sin reinterpretar memoria. Las rutas de tasa,
  Poisson, correlación, Wilcoxon, chi-cuadrado, riesgo, finanzas y modelo SST
  propagan errores de extracción y no escriben reportes parciales.
- [x] Se eliminó el escaper JSON duplicado de `sst_report_advanced.c`; las
  cadenas dinámicas usan `milena_json_write_string`.
- [ ] Endurecer el router textual sin romper los fixtures existentes: la
  clasificación canónica debe conservar el diagnóstico específico del parser
  para análisis y arrays antes de rechazar cualquier fallback.

## Pendientes verificados después de esta iteración

- [ ] Ejecutar y documentar los 8 checks remotos de CI para el commit de esta
  iteración; este entorno no dispone de compilador local.
- [x] Añadir casos automatizados específicos para cada dtype numérico
  soportado, escapes JSON, fallos de extracción y preservación del destino
  en conversiones fallidas; `test-pr21-regressions` está en la matriz
  `make test`.
- [ ] Completar la API de valores común y trasladar transformaciones restantes
  al AST; no se inventan APIs nuevas en esta iteración.
- [x] Revisar y documentar los contratos de propiedad de las APIs públicas
  tabulares y de conversión; la eliminación de duplicados queda pendiente
  hasta disponer de una sustitución canónica probada.
