# Runbook de incidentes Termux/APT

## Artefacto o firma inválida

1. Detener la publicación y conservar los artefactos de evidencia del run.
2. No reemplazar un paquete publicado ni regenerar firmas sobre el mismo nombre.
3. Revocar o rotar la clave solo con el responsable de release; registrar huella,
   commit, versión y hora UTC.
4. Retirar el índice del hosting si la política del repositorio lo permite y
   publicar una comunicación de incidente.
5. Rehacer desde un commit revisado, con nuevo `SOURCE_DATE_EPOCH` y nueva
   evidencia; ejecutar `gpgv` y la validación completa antes de cualquier
   reintento.

## Runner no conforme

Cancelar la ejecución, aislar el dispositivo y no usar sus artefactos. Revisar
etiquetas, `termux-info`, arquitectura, PREFIX, toolchain, dependencias,
workspace y secretos persistentes. El workflow es fail-closed: no se debe
relajar el preflight para desbloquear una cola.

## Paquete incompatible

No instalarlo sobre un dispositivo de usuario. Mantener la versión anterior si
la hay, abrir un incidente con el checksum y la procedencia, y reconstruir tras
corregir el origen. Una prueba fixture o Linux nunca autoriza a declarar
compatibilidad Android.
