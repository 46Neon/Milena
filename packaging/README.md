# Empaquetado de Milena

El código fuente y el empaquetado se mantienen separados. La fuente de verdad para los comandos de instalación, arquitecturas objetivo y estado de disponibilidad es la [matriz de instalación](../docs/INSTALLATION_MATRIX.md).

- `packaging/termux`: construcción del paquete nativo de Termux.
- `packaging/debian`: construcción independiente para Debian/Ubuntu.
- `packaging/windows`: ejecutable portable y manifiestos WinGet.
- `.github/workflows/publish-apt.yml`: publicación del canal APT Termux/AArch64; no es un repositorio Debian/Ubuntu.

## Estado verificable

La Release `v0.2.0` contiene un `.deb` Debian/Ubuntu `amd64`, un ejecutable y ZIP portable Windows x64 y tres manifiestos WinGet adjuntos. Adjuntar los manifiestos a una Release no los publica en una fuente de WinGet. La receta Termux tiene una URL versionada y un SHA-256 real para `v0.2.0`, pero sigue siendo candidata; la Release no contiene el paquete Termux/AArch64 ni su procedencia exigida por el publicador APT.

El workflow APT actual genera el canal **Termux/AArch64** firmado; no construye ni publica índices Debian/Ubuntu. No se debe anunciar ninguno de los tres comandos como disponible hasta que el índice/fuente correspondiente exista y se pruebe en una instalación limpia. La matriz enlazada arriba mantiene los gates por plataforma.

## Separación de plataformas

- **Termux:** compilación con Clang, arquitectura `aarch64` por ahora y rutas bajo `$PREFIX`.
- **Debian/Ubuntu:** compilación separada contra su libc, arquitectura y rutas `/usr`.
- **Windows:** ejecutable portable independiente.

Un `.deb` de Termux no es intercambiable con uno de Debian/Ubuntu. El workflow APT solo acepta el artefacto Termux `milena_*_aarch64.deb`; el paquete Debian nunca se publica como si fuera Termux.

## Reproducibilidad y seguridad

El constructor Termux fija `SOURCE_DATE_EPOCH`, normaliza modos y timestamps y emite un checksum SHA-256. El generador APT crea únicamente `binary-aarch64`, valida el nombre, metadatos y rutas del paquete, y firma `Release` e `InRelease`. Las claves privadas y contraseñas solo se leen desde secretos de CI; nunca se guardan en el repositorio ni en el árbol publicado.

La publicación a Netlify es un canal de hosting opcional del workflow único y no es una dependencia del lenguaje.

## Plantilla para termux-packages

`packaging/termux-packages/milena/build.sh` sigue la forma de una receta de
`termux-packages` y fija el tarball y SHA-256 reales de `v0.2.0`; todavía no está
aceptada oficialmente. La aceptación requiere build del paquete, validación en
Android/Termux AArch64 y revisión de los mantenedores. No se anuncia `pkg install
milena` como disponible hasta que el paquete se publique en una fuente accesible.
