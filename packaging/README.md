# Empaquetado de Milena

El código fuente y el empaquetado se mantienen separados:

- `packaging/termux`: construcción del paquete nativo de Termux.
- `packaging/debian`: construcción independiente para Debian/Ubuntu.
- `.github/workflows/publish-apt.yml`: única definición de publicación APT; no hay una copia ejecutable bajo `packaging/ci`.

## Estado verificable

El objetivo actual de este flujo es un `.deb` local de Termux/aarch64 que se pueda inspeccionar e instalar en un dispositivo Termux real. El repositorio APT todavía no es oficial: antes de anunciarlo deben existir una construcción real, una instalación y actualización verificadas en Termux, índices firmados y una prueba desde el repositorio publicado.

No se anuncia ninguna instalación mediante un gestor de paquetes mientras esas pruebas no estén documentadas. La mera presencia de un `.deb` en una Release de GitHub no constituye un repositorio Termux.

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
`termux-packages`, pero contiene un placeholder de SHA-256 y no está aceptada
oficialmente. Solo se puede proponer después de una Release y una validación
real en Termux/aarch64; no se anuncia `pkg install milena` antes de ese proceso.
