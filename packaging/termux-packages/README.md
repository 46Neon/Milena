# Plantilla `termux-packages` para Milena

`milena/build.sh` es una plantilla con la forma esperada por el ecosistema
`termux-packages`; no es una receta aceptada oficialmente ni afirma que Milena
esté en los repositorios de Termux. El placeholder de SHA-256 es deliberado:
no se debe sustituir hasta disponer de un tarball de Release, una construcción
real aarch64/bionic y pruebas de instalación y eliminación.

Proceso honesto para preparar una propuesta:

1. Construir el `.deb` en un dispositivo o runner Termux/aarch64 registrado.
2. Registrar commit, versión, `$PREFIX`, arquitectura, checksum y procedencia.
3. Crear una Release existente con el código fuente y reemplazar el placeholder
   por el SHA-256 calculado del `SRCURL` exacto.
4. Ejecutar la receta con el checkout oficial de `termux-packages` y corregir
   sus políticas, lint y dependencias; no copiar la receta al repositorio
   oficial antes de esa revisión.
5. Abrir la solicitud siguiendo las políticas vigentes de Termux. La aceptación,
   nombre final y disponibilidad de `pkg install milena` dependen de ese proceso.

La receta instala únicamente `milena` y `README.md`; no incorpora tests,
headers, fuentes, ejemplos ni los módulos experimentales compiler/IR/VM.
