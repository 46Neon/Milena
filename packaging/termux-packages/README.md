# Candidata `termux-packages` para Milena

Milena se presenta como un lenguaje de programación completo para análisis de datos, con sintaxis española cada vez más humana y una única ruta lexer → parser → AST → semántica → runtime → MilenaTable.

`milena/build.sh` sigue la estructura oficial `packages/<name>/build.sh` y ya
contiene una fuente versionada real (`v0.2.0`) y el SHA-256 del tarball exacto.
Es una candidata en este PR, no una afirmación de que `milena` haya sido
aceptada o publicada por Termux.

## Metadatos y verificación

La receta declara `TERMUX_PKG_HOMEPAGE`, `DESCRIPTION`, `LICENSE`, `MAINTAINER`,
`VERSION`, `SRCURL`, `SHA256`, `DEPENDS` y `BUILD_IN_SRC`. El digest se puede
regenerar y comprobar sin confiar en una copia local:

```bash
make tools/check_repository_contracts CC=clang
./tools/check_repository_contracts termux-recipe \
  packaging/termux-packages/milena/build.sh --fetch
```

Si se cambia el tag, primero se descarga el `SRCURL` exacto y se calcula
`sha256sum`; nunca se coloca un placeholder ni se deja el campo vacío como si
fuera publicable. La validación estática falla ante SHA ausente, URL no
versionada, rutas Debian o pasos de build que no usan `$TERMUX_PREFIX`.

## Validar con el repositorio oficial sin clonar de más

En una máquina de revisión se puede mantener un checkout shallow/sparse del
repositorio oficial (el `build-package.sh` y sus scripts son necesarios; no se
copia el repositorio oficial a este proyecto):

```bash
git clone --filter=blob:none --no-checkout \
  https://github.com/termux/termux-packages.git "$HOME/termux-packages"
cd "$HOME/termux-packages"
git sparse-checkout set build-package.sh scripts packages
cd -
./tools/check_repository_contracts termux-recipe \
  packaging/termux-packages/milena/build.sh \
  --official-dir "$HOME/termux-packages"
```

En el dispositivo Android/aarch64 registrado, se instala la receta candidata
en `packages/milena/` y se usa el builder oficial:

```bash
cd "$TERMUX_PACKAGES_DIR"
./build-package.sh -I -f milena
```

`-I` evita recompilar dependencias y `-f` fuerza la descarga/build de la
fuente; en un dispositivo Termux la arquitectura la determina el entorno y el
preflight de PR23 exige `aarch64`.

## Solicitud oficial

Antes de proponer el cambio a `termux/termux-packages` se deben adjuntar el
commit de Milena, el resultado del lint/build oficial, el SHA del `SRCURL`, el
ELF Bionic/aarch64 y evidencia de instalación, ejecución y eliminación del
paquete `milena` con `pkg install`/`pkg remove`. No ejecutar `pkg upgrade` en el
runner: puede actualizar paquetes ajenos a la prueba. Cualquier prueba de
actualización desde una versión anterior debe aislarse y no actualizar otros
paquetes del dispositivo. La revisión de nombres, dependencias, licencia y
disponibilidad queda a cargo de Termux. No se debe anunciar
`pkg install milena` para terceros hasta que el cambio sea aceptado y publicado.

La receta instala `milena`, `README.md` y los avisos/licencias de Milena,
nanoarrow/flatcc y SQLite bajo `$TERMUX_PREFIX`; no incorpora tests, headers,
fuentes, examples ni módulos experimentales compiler/IR/VM.
