# Seguridad

## Qué no publicar

No incluyas en issues, Pull Requests, commits ni archivos del proyecto:

- contraseñas;
- claves privadas;
- tokens de GitHub, Netlify o GPG;
- archivos `.env`;
- datos reales de clientes, pacientes, trabajadores o empresas;
- reportes con información personal.

## Reportar una vulnerabilidad

No publiques una vulnerabilidad sensible en un issue público. Si el repositorio tiene habilitados los avisos privados de seguridad de GitHub, utilízalos. Si no están disponibles, contacta al mantenedor mediante su perfil de GitHub y proporciona únicamente la información necesaria para reproducir el problema.

Incluye:

- versión afectada;
- plataforma;
- pasos para reproducir;
- impacto observado;
- una posible corrección, si la tienes.

No incluyas secretos reales en el informe.

## Termux release boundary

La distribución Termux/aarch64 usa el preflight fail-closed y la evidencia
criptográfica descritos en `docs/TERMUX_SECURITY.md`. Para publicar o responder
a un incidente se deben seguir `docs/TERMUX_RELEASE_CHECKLIST.md` y
`docs/TERMUX_INCIDENT_RUNBOOK.md`; la ausencia de pruebas en un dispositivo
real mantiene el canal en estado no oficial.
