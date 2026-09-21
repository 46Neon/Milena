# Repositorio APT de Milena

Este directorio documenta el formato del repositorio Termux que el workflow único `.github/workflows/publish-apt.yml` puede generar. El canal todavía no es oficial ni debe anunciarse como disponible: falta una prueba real desde un dispositivo Termux y desde el repositorio remoto publicado.

## Fuente única de publicación

La única definición ejecutable es `.github/workflows/publish-apt.yml`. `packaging/ci/publish-apt.yml` fue eliminado para impedir que dos workflows diverjan. El generador reutilizable es `packaging/termux/generate-apt-repo.sh`.

## Estructura mínima actual

Mientras solo exista un build Termux real, el repositorio debe contener exactamente el objetivo `aarch64`:

```text
dists/stable/Release
dists/stable/InRelease
dists/stable/Release.gpg
dists/stable/main/binary-aarch64/Packages.gz
pool/main/m/milena/milena_VERSION_aarch64.deb
milena-archive-keyring.asc
```

No se declaran índices para otras arquitecturas. Un `.deb` Debian/Ubuntu (`amd64`, por ejemplo) no puede ocupar el lugar del paquete Termux.

## Secretos de GitHub Actions

Configura únicamente como secretos del repositorio, nunca en archivos versionados:

```text
MILENA_GPG_PRIVATE_KEY
MILENA_GPG_KEY_ID
MILENA_GPG_PASSPHRASE
NETLIFY_SITE_ID
NETLIFY_AUTH_TOKEN
```

La clave privada y la contraseña se pasan solo al proceso de CI. La clave pública se publica como `milena-archive-keyring.asc`; antes de una publicación oficial debe comprobarse su huella digital en un dispositivo limpio.

## Flujo y validaciones

1. Crear una Release que contenga un `.deb` Termux/aarch64 real y su checksum.
2. Ejecutar manualmente el workflow con la etiqueta.
3. Descargar exclusivamente `milena_*_aarch64.deb`.
4. Validar nombre, versión, arquitectura y rutas bajo `$PREFIX`.
5. Generar `Packages.gz`, `Release`, `InRelease` y `Release.gpg`.
6. Verificar estructura, firma, checksum y ausencia de índices no construidos.
7. Publicar en el único hosting configurado y realizar la prueba real desde Termux.

Hasta completar el último paso, este es un artefacto de preparación y no una instrucción de instalación para usuarios.
