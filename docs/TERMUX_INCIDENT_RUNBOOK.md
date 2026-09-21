# Runbook de incidentes Termux

## Artefacto o firma inválida

Detener publicación, conservar `.deb`, checksum, procedencia/SBOM, `Release` y
logs. Revocar el artefacto o firma afectada, verificar la clave con `gpgv`, y no
reintentar hasta identificar el commit y regenerar metadatos.

## Runner no conforme

Cancelar el job, retirar el runner de las etiquetas y revisar el informe de
preflight. Un runner sin aarch64, bionic, `$PREFIX` válido o herramientas
esperadas no puede producir un paquete ni una Release.
