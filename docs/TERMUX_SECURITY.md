# Seguridad del canal Termux

## Trust boundary

La frontera de confianza empieza en el commit fuente y termina en el ELF
bionic/aarch64 y el `.deb` firmado. El runner self-hosted es hardware dedicado;
no se confía en una etiqueta sin `uname`, `dpkg`, `PREFIX`, `readelf` y hashes.

El cliente debe importar la clave pública por un canal verificado y usar `gpgv`
para validar `Release.gpg`/`InRelease`. Nunca se guardan claves privadas,
contraseñas ni URL inventadas en el repositorio. El paquete se limita al binario
canónico y README; no enlaza compiler/IR/VM experimentales.
