# SQLite nativo: baseline SQL explícito (3C pendiente)

El backend reutiliza la amalgamación SQLite 3.50.4 compilada estáticamente y expone dos superficies locales distintas por la ruta canónica `lexer → parser → AST → validación semántica/límites → plan → runtime → backend C SQLite → MilenaTable`: (1) el baseline de sentencias SQL crudas explícitas y (2) un slice tipado todavía acotado para esquema declarado dentro del bloque, SELECT de proyección/filtro de igualdad, INSERT de una fila y UPDATE con asignaciones y filtro. En el slice tipado el AST y el plan conservan nodos separados de tabla, columnas, proyección, filtros, operadores, valores y tipos; la validación de tabla/columnas/tipo ocurre antes de abrir SQLite. El generador arma SQL solo con identificadores ASCII validados, entre comillas dobles, y gramática fija; todo valor usa `?` y prepared-statement binding. INSERT/UPDATE aceptan `nulo` como binding NULL independientemente del tipo declarado de la columna; la restricción de nullability física queda a cargo de SQLite y no se compara durante la validación del esquema. **El slice no completa la vertical SQL/ORM ni cierra la Fase 3C.** El baseline raw SQL mantiene SQL como texto, que analiza SQLite.

## Baseline de SQL crudo (compatibilidad explícita)

```milena
sql desde "datos.sqlite" limites filas 10000 bytes 8388608 tiempo 2000 {
  ejecutar "CREATE TABLE personas(id INTEGER PRIMARY KEY, nombre TEXT)";
  ejecutar "INSERT INTO personas VALUES(?, ?)" con (42, "Ada");
  consulta "SELECT id, nombre FROM personas WHERE id = ?" con (42);
  iniciar;
  ejecutar "INSERT INTO personas VALUES(?, ?)" con (43, "Grace");
  confirmar;
}
```

`limites` es opcional: los valores predeterminados son 10 000 filas, 8 MiB lógicos contabilizados y 2 000 ms. Los máximos configurables son 100 000 filas, 64 MiB y 30 000 ms; deben ser positivos. Cada `consulta` o `ejecutar` prepara una sentencia SQLite y acepta hasta 999 bindings posicionales `?`. Los valores del lenguaje admitidos actualmente son texto UTF-8, entero int64, real finito, booleano (binding entero 0/1) y nulo. Los valores se enlazan con la API de prepared statements; no se interpolan en el SQL. Una consulta exitosa materializa y publica una `MilenaTable`.

## Slice tipado disponible en el candidato (3C sigue pendiente)

La gramática española acotada admite una o más columnas declaradas, un SELECT con proyección explícita y exactamente un predicado de igualdad, un INSERT de una fila con listas explícitas de columnas y literales tipados, y el UPDATE acotado descrito abajo. El esquema es metadata declarada en el bloque y no ejecuta `CREATE TABLE`; la tabla debe existir con el mismo nombre literal como objeto `table` en `main.sqlite_schema` al comenzar la ejecución del bloque. Después de abrir la conexión y antes de ejecutar cualquier operación del plan, el backend compara cada tabla usada por operaciones tipadas con `PRAGMA main.table_xinfo`: deben coincidir exactamente la cantidad, el orden y los nombres de columnas, y sus familias de tipos SQLite deben ser compatibles con los tipos Milena declarados. La comparación se repite inmediatamente antes de cada operación tipada para detectar cambios de esquema producidos por sentencias raw anteriores del mismo bloque. Una tabla temporal con el mismo nombre se rechaza para evitar que oculte la tabla verificada. `entero` requiere afinidad INTEGER; `real`, afinidad REAL; `texto`, afinidad TEXT; y `booleano`, una declaración de tipo entero o NUMERIC compatible, con valores seleccionados limitados a 0, 1 o nulo. No se comparan restricciones, claves, defaults ni nullability. Las palabras de tipo admitidas son `entero`, `real`, `texto` y `booleano`; los nombres de tabla/columna son identificadores ASCII `[A-Za-z_][A-Za-z0-9_]*`. La tabla debe declararse antes de usarla. Cada proyección, columna de filtro y columna de escritura debe aparecer en ese esquema; los nombres de columnas de escritura no pueden repetirse y cada literal no nulo de escritura debe coincidir exactamente con el tipo declarado. `nulo` puede enlazarse como NULL a cualquier columna; el filtro admite solo literales tipados no nulos.

```milena
sql desde "datos.sqlite" limites filas 10000 bytes 8388608 tiempo 2000 {
  tabla personas (id entero, nombre texto, activo booleano);
  seleccionar id, nombre de personas donde id = 42;
}
```

Se permite como operador escrito `=` o `==` (ambos se normalizan al nodo AST de igualdad). Los operadores de comparación conocidos `!=`, `<`, `<=`, `>` y `>=` tienen nodos explícitos, pero la validación semántica los rechaza para este slice antes de abrir SQLite. La consulta anterior se genera como `SELECT "id", "nombre" FROM "personas" WHERE "id" = ?`; el valor `42` se enlaza como parámetro int64. Antes de imprimir el resultado, el backend verifica que los nombres y tipos de las columnas de la tabla Milena coincidan con la proyección tipada; el materializador también rechaza celdas dinámicas incompatibles con el tipo físico declarado, y booleanos con valores distintos de 0/1. Si falla cualquier fila, no se imprime ninguna parte del resultado de ese SELECT. La única escritura tipada disponible es INSERT de una fila:

```milena
sql desde "datos.sqlite" {
  tabla personas (id entero, nombre texto);
  iniciar;
  insertar en personas (id, nombre) valores (43, "Ada'); DROP TABLE personas; --");
  confirmar;
}
```

Ese ejemplo genera `INSERT INTO "personas" ("id", "nombre") VALUES (?, ?)`; ambos literales se convierten a bindings tipados y el texto de apariencia SQL permanece como dato. La lista de columnas y la de valores debe tener la misma longitud (1–128); no se admiten columnas repetidas ni valores tipados incompatibles con el esquema. `nulo` se enlaza como SQL NULL para cualquier tipo declarado y una restricción física `NOT NULL` fallará en SQLite, revirtiendo la transacción activa. `iniciar`/`confirmar` y `iniciar`/`revertir` se admiten alrededor del INSERT sujeto a las reglas comunes del plan.

También está disponible una única forma de UPDATE tipado sobre la tabla declarada, con 1–128 asignaciones distintas y un solo predicado:

```milena
sql desde "datos.sqlite" {
  tabla personas (id entero, nombre texto);
  iniciar;
  actualizar personas establecer nombre = "Ada Lovelace" donde id = 43;
  confirmar;
}
```

La sintaxis completa es `actualizar <tabla> establecer <columna> = <literal> [, ...] donde <columna> <operador> <literal>;`. Tabla y columnas deben existir en el esquema previamente declarado; nombres y duplicados se validan antes de abrir SQLite. Cada valor no nulo de asignación y el literal del filtro deben coincidir exactamente con el tipo declarado; `nulo` se admite en asignaciones y se enlaza como NULL. La comparación de NULL en el filtro no está soportada, porque el slice no implementa `IS NULL`. El backend aplica además la verificación del esquema físico indicada arriba antes de iniciar operaciones del plan. El filtro admite `=`, `==`, `!=`, `<`, `<=`, `>` y `>=`; `==` se genera como `=`. La salida de ejemplo se genera como `UPDATE "personas" SET "nombre" = ? WHERE "id" = ?`; todos los valores, incluidos los textos con apariencia SQL, se enlazan mediante parámetros preparados. Una transacción puede confirmar el UPDATE o revertirlo explícitamente; ante error de una operación, la transacción activa se revierte. El UPDATE sin predicado, los predicados compuestos, las expresiones y `IS NULL` no forman parte del slice. No se admite `*`, alias, join, ordenamiento, DELETE ni inferencia de esquema.

## Perfil admitido y rechazado

- **Admitido por el baseline raw:** SQL explícito entre comillas, sujeto a la gramática instalada de SQLite, un único statement por operación, operaciones de consulta de solo lectura, operaciones de ejecución, bindings posicionales tipados y transacciones `iniciar`/`confirmar`/`revertir` balanceadas.
- **Admitido solo por el slice tipado:** esquema declarado de tabla con columnas `entero`/`real`/`texto`/`booleano`, comprobado contra la tabla física SQLite existente; SELECT sobre tabla previamente declarada; proyección de 1–128 columnas conocidas, sin duplicados; un solo predicado de igualdad tipado no nulo; INSERT de una fila con 1–128 columnas distintas y valores cuyo tipo corresponde al esquema, admitiendo `nulo` como binding NULL; UPDATE de 1–128 asignaciones distintas (también con `nulo`) y un solo filtro con operador de comparación de la lista documentada. Semántica y generación SQL se validan antes de abrir SQLite, y el esquema físico se verifica después de abrir la conexión pero antes de ejecutar operaciones.
- **Rechazado por el backend:** statements múltiples; escritura a través de `consulta`; uso SQL directo de transacciones; `ATTACH`, `DETACH` y `PRAGMA`; cantidad de parámetros discrepante; BLOB y resultados no representables (incluido texto no UTF-8 o NUL incrustado); afinidades de columna con valores incompatibles; y resultados que excedan los límites configurados.
- **No implementado / no anunciado como capacidad:** ORM completo, comparación de restricciones/claves/defaults/nullability del esquema físico, esquema de resultado persistente, DELETE y formas de UPDATE más allá de la operación acotada descrita aquí, otras escrituras tipadas distintas del INSERT de una fila y UPDATE documentados, joins, filtros compuestos, pooling, conexiones remotas, migraciones y cursores streaming. El slice tipado no es una allowlist del baseline raw: dentro de `consulta`/`ejecutar`, la gramática SQL sigue siendo la de SQLite y el usuario del baseline debe tratar SQL explícito como una superficie separada. El raw SQL no satisface el perfil ORM de 3C.

Las secuencias de transacción sin anidación se validan antes de abrir el archivo; el cierre sin transacción activa y el bloque incompleto se rechazan. Un error de sentencia revierte la transacción activa; el cierre de conexión también revierte cualquier transacción abierta.

## Contrato de recursos y límites conocidos

- El timeout se aplica al bucle de ejecución de SQLite con un progress handler cada 1 000 instrucciones virtuales. No es un deadline estricto para parseo/prepare, apertura, conversiones ni impresión. El busy timeout es 250 ms.
- El backend expone `milena_sql_cancel()` como la única operación admitida concurrentemente con una ejecución activa; las demás operaciones de un handle requieren ownership exclusivo. No se debe cerrar un handle mientras ejecuta, y `milena_sql_reset_cancel()` solo se llama una vez finalizada la ejecución. La prueba nativa cancela desde otro hilo una CTE recursiva de larga duración, verifica status/diagnóstico, resetea la cancelación y prueba la reutilización; también ejecuta trabajo independiente en dos handles y comprueba que cancelar uno no interrumpe el otro. La CLI conecta `SIGINT` a cancelación cooperativa; no se afirma cancelación por `SIGTERM`.
- Los presupuestos de filas/bytes evitan publicar una tabla incompleta y cuentan lógicamente nombres y valores, pero no limitan estrictamente RSS, sobreasignación de vectores ni buffers de SQLite.
- Ante errores del backend la tabla de salida solo se sustituye cuando se construye completamente un resultado exitoso. La integración canónica de CLI, sin embargo, imprime cada consulta exitosa al avanzar; no proporciona atomicidad del conjunto completo de varias operaciones.
- Los diagnósticos del backend son genéricos y no incorporan valores enlazados. No se debe registrar la cadena SQL si contiene datos sensibles.

## Pruebas existentes y gates abiertos

Las pruebas candidatas cubren AST/plan tipados, rechazos semánticos por tabla/columna/tipo/operador antes de abrir una ruta DB inexistente, bindings NULL de INSERT/UPDATE tipados, y E2E de CLI para SELECT/INSERT/UPDATE sobre esquemas físicos coincidentes. La CLI también prueba valores hostiles por binding, UTF-8, rollback explícito y rollback de un INSERT previo cuando un segundo INSERT tipado falla por clave primaria duplicada; verifica que no quede la primera escritura. SELECT tipado con límites de filas/bytes debe fallar sin imprimir encabezado o resultado parcial. También se prueban tabla física inexistente, columnas físicas ausentes/sobrantes, familia de tipo incompatible, DDL raw previo que cambia el esquema y valores de fila incompatibles sin publicación parcial. Las pruebas nativas cubren NULL, int64 mayor que 2^53, UTF-8, límites de filas/bytes, timeout, cancelación desde otro hilo, reset/reutilización y trabajo simultáneo en handles independientes. La verificación local (no CI) incluye un build fresco del CLI a partir de las 57 unidades C del Makefile con Zig 0.16.0, `x86_64-linux-musl`, C17 y `-O2 -g0`, con SQLite 3.50.4 amalgamado y defines `SQLITE_THREADSAFE=1`, `SQLITE_DQS=0`, `SQLITE_OMIT_LOAD_EXTENSION`; SHA-256 de `milena_schema_fresh`: `0ca189c274c98841ad288780363aa9830d9d1c8117cfb80804020ee1547ca6ba`. `MILENA_BIN=./milena_schema_fresh bash tests/test_sqlite_cli.sh` pasó. Pasaron también `tests/test_sqlite_typed_sql` fresco (`-UNDEBUG`, SHA-256 `7a2af99e2a3ebf24eb09fb01a7352eb5d5b0f1c8fd73cab81247a40e7a3bd738`) y `tests/test_sqlite_backend` fresco (`-UNDEBUG`, enlazado con `third_party/sqlite/sqlite3.c` compilado con `-Oz`, SHA-256 `6ec6612b697d5c89c1d6b8b82d5acc7a2aee63fd779155d7642836ad92a30b98`). Sus salidas fueron `typed SQLite AST/semantic/planner checks passed`, `SQLite cross-thread cancellation and independent-connection checks passed` y `SQLite native backend checks passed`. SHA-256 de fuentes clave: `src/sqlite_backend.c` `2ba300f44528b30b3ff0147190073f1e5ed1eb7856e836e071269b4c490f04d8`; `src/query_plan.c` `b6599984308ab6e5fcf6fcac2000105464cf26cb3b843935a4f97ee636b1c5b6`; `src/parser.c` `242a016ccbc9fe90660c47f8f33fc09edb37bb44115d71456634ac76d9a9a155`; `src/language_semantic.c` `37406f7e4f654a538a3737885db438863a874bbe214c47df2a5169c248931fb1`; `tests/test_sqlite_backend.c` `67b93cc1f327694678fe02c4bb769d46ace793dba49198cebb794883256c3bab`; `tests/test_sqlite_typed_sql.c` `f1c71b0410deaef1e108705d123127c71a81f4eae6aa1413b725b6fd17a94567`; `tests/test_sqlite_cli.sh` `2bbf3441e77c96e4bd50857e6a43bdc5ec56c376b5c7db5cbdb047e9c3d41036`. Los targets oficiales de reproducción son `make test-sqlite-typed-sql test-sqlite-backend test-sqlite-cli`; el `make test` completo y la CI de un SHA nuevo no se ejecutaron. Los archivos ejecutables están fuera del árbol de fuentes y no son artefactos CI. No se afirma CI para estos cambios locales. Esto aún no equivale a la aceptación 3C: faltan suite completa, sanitizers para estos cambios, integración/empaquetado específico, builds y E2E multiplataforma y el gate físico Termux/AArch64.

La procedencia y SHA-256 del artefacto SQLite están en `third_party/sqlite/README.md`. La amalgamación se compila con `SQLITE_THREADSAFE=1`, `SQLITE_DQS=0` y `SQLITE_OMIT_LOAD_EXTENSION`; la procedencia/licencia se incluye en los paquetes Windows, Termux y Debian. Esta información de empaquetado no demuestra por sí sola que la nueva superficie SQL compile y pase sus gates en esos tres targets.
