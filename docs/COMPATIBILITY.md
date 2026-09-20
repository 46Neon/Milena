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
