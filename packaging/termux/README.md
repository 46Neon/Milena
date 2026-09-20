# Paquete Termux de Milena

## Estado

Este directorio contiene el primer flujo reproducible para construir un `.deb` local. No publica todavía un repositorio APT.

El flujo correcto es:

```text
fuente Milena
  ↓
clang + make + pruebas
  ↓
paquete .deb para una arquitectura Termux
  ↓
prueba con dpkg/apt local
  ↓
repositorio APT firmado
  ↓
pkg install milena
```

## Requisitos en Termux

```bash
pkg update
pkg install clang make dpkg
```

El compilador principal de Termux es Clang. La validación adicional con GCC debe ejecutarse en otro entorno Linux o en CI, porque un binario de Termux no debe mezclarse con un binario Debian/Ubuntu.

## Construcción local

Desde la raíz del proyecto:

```bash
./packaging/termux/build-local-deb.sh
```

El script:

1. compila Milena con Clang;
2. ejecuta las pruebas;
3. obtiene la arquitectura de Termux;
4. instala el binario y la documentación bajo `$PREFIX` dentro del staging;
5. genera `dist/termux/milena_VERSION_ARCH.deb`.

La instalación local de prueba puede hacerse con:

```bash
dpkg -i dist/termux/milena_*.deb
milena --help
```

Para desinstalar:

```bash
apt remove milena
```

## Repositorio APT

Para que un usuario pueda ejecutar `pkg install milena`, no basta con publicar el `.deb` en GitHub. El servidor debe contener la estructura APT completa:

```text
dists/stable/Release
dists/stable/InRelease
dists/stable/main/binary-aarch64/Packages.gz
dists/stable/main/binary-arm/Packages.gz
dists/stable/main/binary-i686/Packages.gz
dists/stable/main/binary-x86_64/Packages.gz
pool/main/m/milena/*.deb
```

Los índices deben generarse en CI y el repositorio debe firmarse. El usuario debe instalar o confiar en la clave pública mediante un keyring; firmar sin distribuir la clave no elimina las advertencias de confianza.

GitHub Pages puede servir archivos estáticos, pero no genera por sí mismo `Packages.gz`, `Release` ni las firmas. Esas tareas deben ejecutarse antes del despliegue.

## Arquitecturas

Cada arquitectura necesita su propio binario y paquete:

```text
aarch64
arm
x86_64
i686
```

La matriz real debe ajustarse a las arquitecturas soportadas por Termux y a las que Milena decida publicar.

## Rutas

El paquete usa `$PREFIX`, no `/usr/local/bin` ni `sudo`. El script convierte el prefijo de Termux en rutas del archivo `.deb` durante el staging.

## Pendientes antes de publicar

- ejecutar compilación real en Termux;
- compilar con Clang y validar con GCC en CI o Linux;
- ejecutar pruebas unitarias e integración;
- ejecutar ASan, UBSan y LeakSanitizer/Valgrind donde estén disponibles;
- revisar dependencias dinámicas;
- construir todas las arquitecturas objetivo;
- generar índices APT;
- firmar `Release`/`InRelease`;
- probar instalación, actualización y eliminación;
- publicar instrucciones de recuperación y verificación de hashes.
