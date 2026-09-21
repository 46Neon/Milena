# Compatibilidad histórica del router de scripts

`src/script.c` conserva una ruta de compatibilidad para archivos antiguos que
no pueden producir un AST canónico. Esa ruta está deliberadamente separada de
la ejecución oficial.

## Rutas permitidas

- `SCRIPT_PIPELINE_CANONICAL_DATASET`: lexer, parser, AST y `MilenaTable`.
- `SCRIPT_PIPELINE_CANONICAL_ARRAY`: lexer, parser, AST y runtime de arrays.
- `SCRIPT_PIPELINE_CANONICAL_FUNCTION`: lexer, parser, AST e intérprete.
- `SCRIPT_PIPELINE_LEGACY_*`: sintaxis histórica, únicamente para compatibilidad.

Las funciones `run_legacy_*` no son un segundo lugar para implementar
capacidades. No deben recibir comandos SST, transformaciones, tipos, modelos,
operaciones de tablas o funciones nuevas.

## Regla de migración

Una sintaxis heredada solo puede permanecer mientras exista un ejemplo o
cliente que dependa de ella. Cuando se migra una capacidad, su ejecución debe
pasar a `language_runtime.c`; el ejecutor textual anterior debe quedar sin
cambios salvo correcciones de seguridad o aislamiento.

La clasificación textual puede decidir que un archivo es legado, pero nunca
puede decidir cómo se ejecuta una capacidad oficial. Toda capacidad oficial
debe estar representada por tokens, AST, validación semántica y runtime común.
Si el lexer o parser encuentran un token no reconocido en una construcción que
pretende ser canónica, la ejecución debe fallar con diagnóstico; no se permite
un fallback silencioso a la ruta histórica. La compatibilidad solo se activa
para sintaxis histórica identificada explícitamente por su forma documentada.

La ejecución SST textual heredada fue retirada del router. Los comandos SST
solo se ejecutan desde `AST_COMANDO_SST` y `language_runtime.c`; un archivo
antiguo debe migrarse a la sintaxis oficial en vez de obtener un reporte SST
por una segunda ruta.
