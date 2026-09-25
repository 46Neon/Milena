# Matriz de instalación de Milena

**Contrato objetivo:** instalar Milena con un solo comando en Windows, Debian/Ubuntu Linux y Android AArch64 con Termux. Esta matriz distingue el comando deseado de la disponibilidad real del canal: que exista un `.deb`, un `.exe` o un manifiesto en una GitHub Release no implica que el gestor de paquetes pueda encontrarlo.

**Estado base revisado:** `main` en `694512c99c3add66dca8405b7f2144adb9983490`, Release `v0.2.0`. Revalidar cada fila contra la Release y el índice del gestor antes de anunciar disponibilidad.

## Comandos objetivo y estado

| Plataforma | Arquitectura del artefacto | Comando objetivo | Estado comprobado |
|---|---|---|---|
| Windows | x64 | `winget install --id 46Neon.Milena --exact` | La Release `v0.2.0` contiene `milena.exe`, ZIP portable y tres manifiestos WinGet. El workflow genera y adjunta los manifiestos a la Release, pero este repositorio no presenta una publicación aceptada/indexada en la fuente pública de WinGet. El comando no se debe anunciar como disponible hasta validar una instalación desde esa fuente. |
| Debian/Ubuntu Linux | amd64 | `sudo apt install milena` | La Release `v0.2.0` conserva un `.deb` histórico con `maintainers@milena.invalid`; no hay índice APT Linux publicado ni fuente verificada en una instalación limpia. Este PR actualiza el builder para futuros paquetes a `Milena SST <j7942281@gmail.com>`. El workflow `publish-apt.yml` es para Termux/AArch64 y no satisface este canal. |
| Android con Termux | AArch64 (`aarch64`) y Bionic | `pkg install milena` | Hay una receta candidata para `termux-packages`, pero no consta aceptación en el repositorio oficial. La Release `v0.2.0` no contiene el `.deb` Termux/AArch64 ni su procedencia requeridos por el publicador APT. No hay runner self-hosted registrado para la validación física. El comando no se debe anunciar como disponible. |

Los comandos anteriores son **objetivos**, no instrucciones que garanticen una instalación hoy. La interfaz de usuario solo podrá marcarlos “disponibles” después de que el gestor pueda resolver el paquete desde una fuente configurada y se complete la prueba de instalación limpia.

## Reglas de arquitectura

- **Windows:** el artefacto actual es Windows x64. No reutilizar el `.deb` ni el ejecutable de Linux/Termux. No declarar Windows ARM64 hasta que tenga artefacto y CI propios.
- **Debian/Ubuntu:** el `.deb` Linux usa el ABI y las rutas de Linux (`/usr`). La validación actual de Release produce `amd64`; Linux ARM64 requiere un build y un gate nativos/cross-build documentados. No reutilizar el paquete Termux.
- **Android/Termux:** el host es Android AArch64; Termux es la aplicación y su entorno de usuario, con Bionic. El paquete usa `$PREFIX`, `uname -m=aarch64` y `dpkg --print-architecture=aarch64`. Un runner Ubuntu, WSL o un `.deb` Debian no prueba Bionic.

## Qué significa realmente “un comando”

`winget install`, `apt install` y `pkg install` solo resuelven Milena si el manifiesto o paquete está disponible en una fuente registrada. Para que la instalación funcione en un sistema limpio:

1. **WinGet:** el manifiesto versionado debe validarse y ser aceptado/publicado en una fuente consultable por WinGet; adjuntarlo a GitHub Releases no lo indexa automáticamente.
2. **APT Linux:** debe publicarse un repositorio Linux firmado (o incorporarse el paquete a un archivo oficial), con índice y artefactos por arquitectura. Si se usa un repositorio propio, registrar su clave y fuente también es un paso de bootstrap; `apt install milena` por sí solo no configura una fuente desconocida.
3. **Termux:** para que `pkg install milena` funcione en Termux sin pasos previos, la receta debe ser aceptada y publicada por `termux/termux-packages`. Un repositorio APT propio requiere registrar primero su fuente y clave; no equivale a estar en el repositorio oficial de Termux.

No usar scripts remotos ejecutados directamente por `curl | sh` como sustituto de una fuente firmada y verificable.

## Gates de aceptación por plataforma

### Windows / WinGet

- Construir y probar `milena.exe` x64; producir checksum y manifiestos versionados con URL HTTPS y SHA-256 del artefacto real.
- Ejecutar `winget validate` sobre los tres manifiestos y probar instalación, ejecución, actualización y desinstalación desde una máquina limpia mediante la fuente que se anunciará.
- Publicar el manifiesto en la fuente aprobada antes de presentar el comando como disponible.

### Debian/Ubuntu / APT

- Construir paquetes Linux por arquitectura declarada; verificar control, dependencias, rutas, checksum y procedencia. Este PR fija `Maintainer: Milena SST <j7942281@gmail.com>`, genera/verifica el sidecar SHA-256 del `.deb` amd64 y añade un generador local de APT firmado probado en CI con una clave efímera. El artefacto histórico `v0.2.0` conserva el correo anterior y no debe publicarse en un feed nuevo.
- Publicar índices `Packages` y metadatos `InRelease`/`Release.gpg` firmados para cada arquitectura disponible. La CI instala y elimina `milena` desde un origen `file:` firmado con clave efímera, pero eso no publica ni configura un origen para usuarios; el canal Linux debe estar separado del repositorio Termux.
- Probar `apt install`, actualización, ejecución y eliminación en una instalación limpia de cada distribución/arquitectura anunciada.

### Android AArch64 / Termux

- Construir en Android AArch64 con Clang/Bionic usando el `build-package.sh` oficial de Termux; guardar SHA, procedencia, metadatos del `.deb` y evidencia del dispositivo/commit.
- Probar `pkg install`, `pkg upgrade`, ejecución y `pkg remove` en Termux real. Verificar literalmente `uname -m=aarch64`, `dpkg --print-architecture=aarch64`, `$PREFIX` y el ELF.
- La evidencia AArch64 satisface el gate del dispositivo objetivo, pero no demuestra por sí sola compatibilidad con todos los targets del repositorio oficial de Termux. Antes de enviar la receta, confirmar su matriz de arquitecturas (Termux contempla `aarch64`, `arm`, `i686` y `x86_64`) o justificar y acordar la limitación con sus mantenedores.
- Confirmar que `TERMUX_PKG_LICENSE` usa identificadores aceptados y que el paquete instala los avisos/licencias correspondientes; reemplazar la identidad genérica del mantenedor por la aprobada para la contribución.
- Completar la aceptación/publicación de la receta o registrar y probar explícitamente un repositorio APT propio. Ubuntu no sustituye esta evidencia.

## Fuente de estado

- [`packaging/windows/README.md`](../packaging/windows/README.md): artefacto portable y proceso WinGet.
- [`packaging/debian/README.md`](../packaging/debian/README.md): paquete local Debian/Ubuntu.
- [`packaging/debian/generate-apt-repo.sh`](../packaging/debian/generate-apt-repo.sh): genera y firma un índice Linux amd64 local; no publica ni configura el origen.
- [`packaging/termux/README.md`](../packaging/termux/README.md): paquete Termux y validación Android.
- [`packaging/termux-packages/README.md`](../packaging/termux-packages/README.md): receta candidata y proceso de aceptación oficial.
- [`.github/workflows/build-release.yml`](../.github/workflows/build-release.yml): artefactos Debian amd64 y Windows x64.
- [`.github/workflows/publish-apt.yml`](../.github/workflows/publish-apt.yml): repositorio APT Termux/AArch64; no es el repositorio Linux.
- [`.github/workflows/termux-aarch64-contract.yml`](../.github/workflows/termux-aarch64-contract.yml): gate manual para un runner físico Termux/AArch64.
