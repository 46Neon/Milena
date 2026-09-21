# Paquete Termux de Milena

## Estado verificable y límite explícito

PR23 contiene un contrato fail-closed para un runner `self-hosted`, `termux`,
`aarch64`, `milena`, pero **no crea, registra ni emula hardware**. No hay una
prueba de compilación, instalación, actualización o eliminación en Android
hasta que un operador registre un dispositivo Termux/aarch64 con esas etiquetas.
Por ello no se anuncia todavía un paquete instalable mediante APT.

La cadena verificable es:

```text
commit exacto -> preflight de identidad -> clang/aarch64 -> make test
-> ELF/dependencias -> .deb bajo $PREFIX -> SBOM + procedencia + SHA-256
-> validación de metadatos -> (solo con dispositivo) install/upgrade/remove
```

## Construcción en un dispositivo real

```bash
pkg update
pkg install -y git clang make dpkg python3 binutils
./scripts/termux-runner-preflight.sh
SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)" ./packaging/termux/build-local-deb.sh
```

El constructor exige `uname -m=aarch64`, `dpkg --print-architecture=aarch64`,
`PREFIX=/data/data/<paquete>/files/usr`, Clang con objetivo aarch64, workspace
limpio y herramientas Termux. Compila por la frontera canónica
`lexer -> parser -> AST -> language_semantic -> language_runtime -> MilenaTable`;
no enlaza compiler/IR/VM experimentales.

La salida contiene un único `.deb`, su `.sha256`, un SBOM CycloneDX, un JSON de
procedencia con commit/compiler/target/epoch y `SHA256SUMS`. `validate_termux_artifact.py`
rechaza rutas Debian/Ubuntu, arquitecturas no aarch64, dependencias libc de
Debian y nombres que no coinciden con la versión del paquete.

## Prueba condicionada al runner

Solo después de una ejecución aprobada del workflow manual se puede ejecutar:

```bash
./scripts/termux-install-smoke.sh dist/termux/milena_*_aarch64.deb
```

La prueba se niega si `milena` ya estaba instalado, instala el paquete, ejecuta
el binario, repite la instalación para cubrir actualización/reconfiguración y
lo elimina. Esto no sustituye la prueba APT desde una sesión Termux limpia.

## Portabilidad nativa y autodiagnóstico

El constructor usa la versión exacta del paquete al compilar (`--version` debe
coincidir con `dpkg-deb`), ejecuta `--self-check` antes de empaquetar y exige un
triple Clang `aarch64-*-android*`. El ELF se inspecciona sin ejecutarlo:
`ELF64/AArch64`, intérprete `/system/bin/linker64` y ausencia de dependencias
Debian/glibc. El validador del workflow repite esa inspección dentro del `.deb`.

No se añaden bibliotecas externas: el enlace usa únicamente la libc/bionic y
libm provistas por Termux. Las pruebas con límite de tiempo tienen una ruta
alternativa cuando `timeout` no está disponible, y el Makefile separa
`CPPFLAGS`, `CFLAGS` y `LDFLAGS` para no asumir flags de GNU. Las pruebas del
paquete siguen validando el binario canónico; los módulos experimentales
continúan fuera de `SOURCES` y de la frontera de ejecución oficial.
