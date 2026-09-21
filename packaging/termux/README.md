# Paquete Termux de Milena

Milena es un lenguaje de programación completo orientado al análisis de datos; su sintaxis española evoluciona hacia una forma cada vez más humana. La receta conserva una sola ruta lexer → parser → AST → semántica → runtime → MilenaTable.

## Estado de PR23

El camino oficial candidato es `packaging/termux-packages/milena/build.sh`, que
se instala temporalmente como `packages/milena/build.sh` dentro de un checkout
de `termux/termux-packages`. La receta usa el tarball real `v0.1.1` y un SHA-256
verificado; todavía no existe aceptación ni repositorio APT oficial de Milena.

El workflow manual de PR23 solo compila si existe un dispositivo Android/Termux
registrado con las etiquetas `self-hosted`, `termux`, `aarch64`, `milena` y la
variable `TERMUX_PACKAGES_DIR` apunta al checkout oficial. Sin ese runner, el
job no se ejecuta y no se afirma compilación o instalación real.

## Entorno Termux real

```bash
pkg update
pkg install -y git clang make dpkg python binutils
export TERMUX_PACKAGES_DIR="$HOME/termux-packages"
```

El compilador es Clang y la libc es Android/Bionic. `aarch64` se comprueba con
`uname -m` y `dpkg --print-architecture`; no se mezcla un binario Debian/glibc.
El entorno Termux es Linux incompleto respecto a Debian: no presupone `sudo`,
`/usr/bin`, systemd, glibc, APT externo ni rutas de staging Debian.

## Candidata y build oficial

Desde la raíz de Milena se valida la receta y luego se copia al checkout oficial:

```bash
python3 scripts/validate_termux_recipe.py \
  packaging/termux-packages/milena/build.sh --fetch
mkdir -p "$TERMUX_PACKAGES_DIR/packages/milena"
cp packaging/termux-packages/milena/build.sh "$TERMUX_PACKAGES_DIR/packages/milena/"
python3 scripts/validate_termux_recipe.py \
  "$TERMUX_PACKAGES_DIR/packages/milena/build.sh" \
  --official-dir "$TERMUX_PACKAGES_DIR"
cd "$TERMUX_PACKAGES_DIR"
./build-package.sh -I -f milena
```

El builder oficial genera el `.deb` en su directorio de salida. El workflow lo
copia a `dist/termux/`, valida `Architecture: aarch64`, `$PREFIX`, el ELF y el
checksum, y conserva evidencia del runner. No se incluye ningún test, header,
fuente, objeto, example ni módulo compiler/IR/VM en el paquete; el `Makefile`
canónico enlaza lexer → parser → AST → semántica → runtime → MilenaTable.

Para una comprobación local de staging (no sustituye `build-package.sh`) existe:

```bash
./packaging/termux/build-local-deb.sh
```

Ese script también exige `$PREFIX`, `aarch64` y Clang/Bionic, y no debe
interpretarse como una construcción Debian.

## Ciclo de vida en Android

En el runner dedicado, el smoke test ejecuta realmente:

```bash
pkg install -y dist/termux/milena_..._aarch64.deb
pkg upgrade -y
pkg remove -y milena
```

La prueba se niega a continuar si `milena` ya estaba instalado. Una URL HTTPS
opcional permite probar además un repositorio APT previamente configurado; PR23
no inventa host, source-list, clave ni secreto. Sin URL, ese tramo queda marcado
como `apt_smoke=not-run (fail-closed)`.

## Runner y pruebas virtuales

Registrar un runner self-hosted en un dispositivo Android/aarch64 dedicado con
las etiquetas exactas `termux`, `aarch64`, `milena`; instalar el runner usando
el método soportado por GitHub para ese dispositivo, configurar
`TERMUX_PACKAGES_DIR` a un checkout oficial mantenido y limpiar el workspace
entre ejecuciones. El workflow manual exige `confirm_device=true`.

Un contenedor Debian, WSL, Ubuntu hosted runner o emulador no demuestra Termux,
Bionic ni aarch64: solo puede ejecutar las pruebas estáticas y la fixture de
metadatos (`python3 scripts/test_termux_packaging.py`). Si no existe hardware
registrado, esa limitación debe permanecer visible y no se genera una falsa
marca de compilación/instalación.

## Antes de solicitar inclusión oficial

1. Ejecutar lint y `build-package.sh -I -f milena` en el checkout oficial.
2. Registrar modelo Android, versión de Termux, commit, `PREFIX`, arquitectura,
   versión de Clang, checksum y logs sin secretos.
3. Completar `pkg install`, `pkg upgrade`, ejecución y `pkg remove` en una sesión
   limpia; verificar también el caso de actualización desde la versión previa.
4. Revisar licencia, dependencias vacías, rutas `$TERMUX_PREFIX` y el contenido
   mínimo del paquete.
5. Preparar el cambio para `termux/termux-packages` siguiendo su revisión; no
   afirmar disponibilidad hasta que los mantenedores lo acepten y publiquen.
