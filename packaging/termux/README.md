# Paquete Termux de Milena

## Estado actual

Este directorio construye un `.deb` nativo de Termux para **aarch64**, sin root y bajo `$PREFIX`. El resultado local es verificable; todavía no existe un repositorio APT oficial ni una afirmación de instalación para terceros.

La secuencia que aún debe completarse antes de una solicitud oficial es:

```text
fuente Milena
  ↓
clang + make + pruebas
  ↓
.deb Termux/aarch64 + SHA-256
  ↓
instalación y ejecución en un dispositivo Termux real
  ↓
índices APT y firmas Release/InRelease
  ↓
prueba de instalación, actualización y eliminación desde el repositorio publicado
```

## Requisitos en Termux

```bash
pkg update
pkg install -y git clang make dpkg
```

El compilador es Clang. La validación con GCC se ejecuta en otro entorno Linux o en CI; un binario Termux no se mezcla con un binario Debian/Ubuntu.

## Construcción local

Desde la raíz del proyecto:

```bash
./packaging/termux/build-local-deb.sh
```

El script:

1. compila Milena con Clang y ejecuta las pruebas;
2. exige que `dpkg --print-architecture` sea `aarch64`;
3. instala el binario y la documentación bajo el prefijo real de Termux;
4. normaliza timestamps y permisos mediante `SOURCE_DATE_EPOCH`;
5. genera `dist/termux/milena_VERSION_aarch64.deb` y su `.sha256`.

Prueba local (en un dispositivo Termux, sin confundirla con una prueba APT):

```bash
dpkg -i dist/termux/milena_*.deb
command -v milena
milena --help
sha256sum -c dist/termux/*.deb.sha256
```

La instalación se debe revertir con el gestor local después de la prueba. Esta instrucción no demuestra que exista un repositorio remoto.

## Repositorio APT preparado, pero no oficial

`generate-apt-repo.sh` se ejecuta en Linux/CI y acepta exactamente un paquete `milena` de arquitectura `aarch64`. Rechaza artefactos Debian, genera solo:

```text
dists/stable/Release
dists/stable/InRelease
dists/stable/Release.gpg
dists/stable/main/binary-aarch64/Packages.gz
pool/main/m/milena/milena_VERSION_aarch64.deb
milena-archive-keyring.asc
```

El workflow descarga únicamente el artefacto Termux con ese nombre, valida su arquitectura y sus rutas bajo `$PREFIX`, y nunca declara `arm`, `i686`, `x86_64` ni `amd64` sin paquetes construidos. Netlify es solo el hosting opcional documentado en el workflow; no es una dependencia de Milena.

## Arquitecturas y rutas

`aarch64` es el único objetivo Termux activo hasta contar con builds y pruebas reales para otras arquitecturas. El paquete usa rutas bajo `$PREFIX`, no `/usr/local/bin`, `/usr/bin` ni `sudo`. Debian/Ubuntu tiene un constructor separado y no se puede reutilizar aquí.

## Pendientes antes de la solicitud oficial

- construir en un dispositivo o runner Termux/aarch64 real;
- instalar, ejecutar, actualizar y eliminar el paquete local;
- comprobar checksum y dependencias dinámicas;
- generar y revisar `Packages.gz`, `Release`, `InRelease` y `Release.gpg`;
- publicar el repositorio en el único hosting configurado y probarlo desde Termux;
- verificar la clave pública y la instalación desde APT en una sesión limpia;
- documentar commit, versión de Termux, arquitectura y resultados;
- solo después, preparar la solicitud oficial a los repositorios de Termux.
