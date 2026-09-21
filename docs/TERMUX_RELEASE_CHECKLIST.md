# Checklist de release Termux/aarch64

No marcar una casilla por inferencia; adjuntar el artefacto o log.

- [ ] El commit a publicar está identificado y la rama aprobada es PR23.
- [ ] Existe un runner real con etiquetas `self-hosted`, `termux`, `aarch64`, `milena`.
- [ ] El workflow manual se ejecutó con `confirm_device=true`; preflight, toolchain y workspace están limpios.
- [ ] `uname`, `dpkg`, PREFIX, target Clang, libc/plataforma y dependencias quedaron en evidencia.
- [ ] `make test` y las puertas compiler-boundary/experimental-isolation pasaron.
- [ ] El `.deb` tiene `Package: milena`, versión coherente, `Architecture: aarch64` y solo rutas `$PREFIX`.
- [ ] El ELF y sus dependencias no usan la plataforma Debian/Ubuntu.
- [ ] SBOM, procedencia, `SOURCE_DATE_EPOCH`, SHA-256 y `SHA256SUMS` están presentes.
- [ ] En el dispositivo real pasaron instalación, ejecución, actualización/reconfiguración y eliminación.
- [ ] La Release contiene solo `binary-aarch64`; `Packages.gz` y `Release` coinciden con sus hashes.
- [ ] `Release.gpg` e `InRelease` pasan `gpgv` con la huella documentada.
- [ ] En una sesión Termux limpia se probó descarga, instalación, actualización y eliminación desde APT.
- [ ] La publicación se ejecuta una sola vez mediante dispatch con `confirm_publish=true`.
- [ ] Si falta hardware o cualquiera de estas pruebas, se mantiene el estado no oficial y no se publica.
