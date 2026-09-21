# Repositorio APT de Milena para Termux

La publicación es manual, con confirmación explícita y `concurrency` sin
cancelación, en `.github/workflows/publish-apt.yml`. No se publica en cada tag:
una etiqueta por sí sola no es autorización. Si no existe un artefacto Termux
real acompañado por checksum, SBOM y procedencia, el workflow falla antes de
firmar o publicar.

## Aislamiento de arquitectura y plataforma

El repositorio contiene exactamente `aarch64`:

```text
dists/stable/Release
dists/stable/InRelease
dists/stable/Release.gpg
dists/stable/main/binary-aarch64/Packages.gz
pool/main/m/milena/milena_VERSION_aarch64.deb
milena-archive-keyring.asc
repository-provenance.json
SHA256SUMS
```

No se crean índices para otras arquitecturas. El validador comprueba nombre,
versión, arquitectura, rutas bajo `$PREFIX`, ausencia de dependencias Debian,
hash del pool, índice `Packages.gz`, arquitectura declarada en `Release`,
checksum del índice y que no haya artefactos extra.

La clave pública se exporta como `milena-archive-keyring.asc`; `gpgv` verifica
`Release.gpg` y `InRelease`, y se compara la huella esperada con la clave
importada antes de publicar. Las claves privadas y contraseñas solo viven en
los secretos de CI. El hosting no se considera una prueba de instalación.

## Secretos requeridos

```text
MILENA_GPG_PRIVATE_KEY
MILENA_GPG_KEY_ID
MILENA_GPG_PASSPHRASE
NETLIFY_SITE_ID
NETLIFY_AUTH_TOKEN
```

Antes de solicitar cualquier incorporación a un repositorio oficial, un
operador debe probar en un dispositivo Termux/aarch64 limpio la clave, descarga,
instalación, actualización, ejecución y eliminación desde el repositorio
publicado, y conservar la evidencia asociada al commit.
