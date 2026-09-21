# Checklist de Release Termux

- [ ] Registrar un runner real `self-hosted`, `termux`, `aarch64`, `milena`.
- [ ] Ejecutar manualmente el contrato con `confirm_device=true`.
- [ ] Revisar ELF bionic, versión, `$PREFIX`, checksum y procedencia/SBOM.
- [ ] Probar instalación, actualización y eliminación en un dispositivo limpio.
- [ ] Configurar la clave y el repositorio; verificar `gpgv`.
- [ ] Publicar APT solo con `confirm_publish=true` y una Release existente.
- [ ] Ejecutar `pkg install milena` únicamente tras aceptación oficial; no anunciarlo antes.
