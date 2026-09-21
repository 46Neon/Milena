# Plan de validación Termux y límites de integración

## Estado honesto en PR23

No hay un dispositivo Termux/aarch64 registrado como runner de GitHub Actions en
este proyecto. Un `runs-on: [self-hosted, termux, aarch64, milena]` no crea un dispositivo:
si se añade sin un runner registrado, el job queda en cola indefinidamente. Por
eso PR23 **no añade ni simula un runner** y no declara una compilación, instalación
o ejecución en Termux.

La CI reproducible que sí puede ejecutarse sin Android valida:

- metadatos `Package`, `Version` y `Architecture: aarch64` del `.deb`;
- que todos los destinos estén bajo `data/data/com.termux/files/usr` (el `$PREFIX`
  empaquetado), sin rutas Debian como `/usr/bin`;
- dependencias, checksum SHA-256 y nombre del artefacto;
- índice `Packages.gz`, correspondencia de hash con el paquete, `Release` y la
  ausencia de índices para arquitecturas no construidas;
- firmas `Release.gpg` e `InRelease` cuando se proporciona la clave pública a
  `gpgv` en el workflow de publicación.

`python3 scripts/test_termux_packaging.py` usa un paquete sintético para probar
los rechazos de metadatos, rutas y arquitectura. No es un binario Termux y no
sustituye una prueba en Android.

## Contrato para un futuro runner

PR23 incluye el contrato manual en
`.github/workflows/termux-aarch64-contract.yml`, pero no crea ni registra un
runner. El job solo puede ejecutarse con las etiquetas explícitas
`self-hosted`, `termux`, `aarch64` y `milena`, y exige la confirmación manual
`confirm_device=true`. Si no existe un runner registrado con esas etiquetas, el
job no se presenta como una prueba exitosa ni como una compilación Termux.

El equipo debe documentar antes de habilitarlo:

1. modelo, `aarch64`, versión de Android y versión de Termux;
2. versión del runner y método soportado para ejecutar sus acciones en Android;
3. `pkg install git clang make dpkg python` y el espacio disponible;
4. commit exacto, `PREFIX`, `dpkg --print-architecture` y `clang --version`;
5. limpieza del workspace entre ejecuciones y ausencia de secretos persistentes.

La primera ejecución debe ser manual y no publicar nada: el workflow ejecuta
`scripts/termux-runner-preflight.sh`, exige `uname -m=aarch64` y
`dpkg --print-architecture=aarch64`, luego ejecuta
`packaging/termux/build-local-deb.sh`, valida el checksum y conserva logs del
build. Después debe probarse en el mismo dispositivo la instalación local,
`command -v milena`, `milena --help`, actualización, ejecución de un script y
eliminación. La prueba APT requiere además un repositorio publicado, una sesión
limpia, verificación de la clave y una instalación/actualización/eliminación
real. Hasta entonces, el workflow Linux de publicación solo prepara y valida
metadatos; no afirma `pkg install`.

## Frontera compiler/IR/VM y tabla canónica

El binario oficial mantiene el pipeline:

```text
lexer -> parser -> AST -> language_semantic -> language_runtime -> MilenaTable
```

`Dataset` es una entrada de compatibilidad: el runtime materializa el dataset en
`MilenaTable` y las transformaciones, agrupaciones, resúmenes, joins y análisis
operan sobre la tabla canónica. `scripts/check_compiler_boundary.py` y el
objetivo `make test` comprueban que `compiler.c`, `ir.c`, `vm.c`, `gc.c` y los
demás módulos experimentales no entren en `Makefile:SOURCES` ni sean incluidos
por una fuente oficial.

Los headers y fuentes de compiler/IR/VM siguen siendo un área experimental, no
una segunda ruta de ejecución. Integrarlos ahora exigiría un diseño aprobado de
opcodes, tipos/errores, ownership/GC, semántica de columnas y equivalencia
end-to-end con `language_runtime`; enlazarlos a ciegas rompería la separación y
podría crear un runtime paralelo. La siguiente fase segura es especificar una
IR que represente operaciones de `MilenaTable`, añadir pruebas AST→IR→ejecución
con resultados comparados contra el runtime canónico y solo entonces evaluar
cada módulo para inclusión.
