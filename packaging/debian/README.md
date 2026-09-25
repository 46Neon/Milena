# Paquete Debian/Ubuntu

Este canal es independiente del paquete Termux. Aunque ambos usan `.deb`, el binario Debian se compila contra el entorno Linux objetivo y se instala bajo `/usr/bin` y `/usr/share`.

## Construcción local

En Debian/Ubuntu:

```bash
sudo apt update
sudo apt install build-essential clang dpkg-dev
./packaging/debian/build-local-deb.sh
```

Resultado esperado:

```text
dist/debian/milena_VERSION_amd64.deb
```

## Generar un repositorio APT local (sin publicar)

Instala las herramientas de índices y firma:

```bash
sudo apt install dpkg-dev apt-utils gnupg
```

Con una clave de publicación disponible en el keyring local, genera el repositorio estático firmado:

```bash
export MILENA_GPG_KEY_ID='FINGERPRINT_DE_LA_CLAVE'
./packaging/debian/generate-apt-repo.sh dist/debian dist/debian-apt
```

El generador valida el `.deb`, su checksum, arquitectura y mantenedor; produce `Packages`, `Release`, `InRelease`, `Release.gpg` y el keyring público. La CI ejercita el generador con una clave efímera e instala/elimina `milena` desde el origen `file:` firmado; esa clave no se usa para publicar.

Esto **no publica ni configura** una fuente APT. `sudo apt install milena` solo funcionará cuando el repositorio Linux real esté alojado, firmado con la clave de producción y registrado en el sistema.

Para ARM64 se necesita un toolchain cruzado o un runner ARM64 y una compilación separada. No se debe reutilizar un `.deb` generado para Termux.
