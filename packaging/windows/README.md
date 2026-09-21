# Distribución Windows

Windows no utiliza `apt` ni `pkg`. El canal de distribución será:

1. `milena.exe` compilado y probado en un runner Windows;
2. archivo `.zip` portable y su checksum externo;
3. manifest para WinGet después de validar una versión pública;
4. instalador `.exe` o `.msi` únicamente cuando exista uno real y probado.

El código debe validarse con LLVM/Clang en Windows. No se debe asumir que un binario Linux o Termux funciona en Windows.

## Validación y artefactos

El script `build.ps1` compila también `src/entrypoints.c`: `milena.exe` conserva
los comandos históricos, pero sus entradas `analizar`, `perfil` e `inspect`
pasan por el frontend canónico antes de usar sus backends de salida.

El workflow de Windows verifica:

- ejecución desde PowerShell, CMD y `PATH`;
- scripts válidos e inválidos;
- pruebas de arrays, bosques y finanzas;
- checksum SHA-256 del ejecutable;
- estructura y ejecución del ZIP portable;
- checksum externo del ZIP.

Los archivos `.sha256` usan el formato compatible con `sha256sum`:

```text
<hash> *<nombre-del-archivo>
```

La versión del tag se incorpora al ejecutable, al paquete Debian, al ZIP y a los manifiestos WinGet. Las compilaciones de CI sin tag usan una versión de desarrollo controlada.

## WinGet

El identificador previsto es `46Neon.Milena`. Antes de publicarlo debe confirmarse que será la identidad permanente del paquete, porque cambiarlo posteriormente crea un paquete distinto en WinGet.

El manifest requiere una URL HTTPS pública de GitHub Release y el SHA-256 definitivo del artefacto. No se deben inventar URLs, hashes ni switches de instalación.

Mientras el artefacto sea un ejecutable directo sin instalador, el generador usa `InstallerType: portable` y declara el comando `milena`.

Para generar los manifiestos:

```powershell
$hash = (Get-FileHash .\milena.exe -Algorithm SHA256).Hash
.\generate-winget-manifest.ps1 `
  -Version '0.1.1' `
  -InstallerUrl 'https://github.com/46Neon/Milena/releases/download/v0.1.1/milena.exe' `
  -InstallerSha256 $hash
```

El modo `-InstallerType exe` solo debe usarse cuando exista un instalador real y se conozcan sus switches silenciosos. En ese caso es obligatorio proporcionar `-SilentSwitch`.

Antes de proponer el paquete a WinGet hay que ejecutar `winget validate` sobre los tres YAML generados y verificar una instalación desde cero. La Release se publica únicamente después de generar los artefactos, comprobar sus checksums y crear los tres manifiestos.
