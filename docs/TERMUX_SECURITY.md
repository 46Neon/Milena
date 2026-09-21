# Seguridad de la distribución Termux

## Trust boundary

El runner `self-hosted/termux/aarch64/milena` es hardware administrado por el
proyecto, no una afirmación derivada de etiquetas. El preflight rechaza hosts
no Termux, arquitecturas distintas, PREFIX incorrecto, workspace sucio,
toolchain incompleto y objetivos que no sean aarch64. No se guardan secretos en
el dispositivo ni en los artefactos.

La distribución Termux se mantiene separada del constructor Debian/Ubuntu. El
paquete solo puede instalar bajo `$PREFIX`, no puede declarar dependencias
`libc6`, `libstdc++6` o equivalentes Debian, y no enlaza módulos compiler/IR/VM
experimentales en el binario oficial. La frontera canónica sigue siendo
lexer, parser, AST, semántica, runtime y `MilenaTable`.

## Release evidence

Una publicación necesita, como mínimo, commit, versión, arquitectura, target,
`SOURCE_DATE_EPOCH`, SBOM, procedencia, checksum del paquete, `Packages.gz`,
`Release`, `InRelease`, `Release.gpg` y keyring. `gpgv` debe validar ambas firmas
con la huella esperada; cualquier ausencia, mezcla de arquitecturas o diferencia
de hash detiene el proceso.

## Limitación

Hasta ejecutar el workflow manual en un dispositivo real no existe evidencia de
Android/bionic, permisos, almacenamiento, instalación, actualización,
eliminación ni descarga desde un repositorio remoto. CI Linux y paquetes
fixture solo prueban el contrato estático y criptográfico.
