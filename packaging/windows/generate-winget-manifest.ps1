[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')][string]$Version,
    [Parameter(Mandatory = $true)][ValidatePattern('^https://')][string]$InstallerUrl,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$InstallerSha256,
    [ValidateSet('portable', 'exe')][string]$InstallerType = 'portable',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9 ._-]*$')][string]$Publisher = '46Neon',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]+$')][string]$PackageIdentifier = '46Neon.Milena',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9 ._-]*$')][string]$PackageName = 'Milena',
    [ValidatePattern('^https://')][string]$PublisherUrl = 'https://github.com/46Neon',
    [ValidatePattern('^https://')][string]$PackageUrl = 'https://github.com/46Neon/Milena',
    [string[]]$Commands = @('milena'),
    [string]$SilentSwitch,
    [string]$SilentWithProgressSwitch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($InstallerType -eq 'portable' -and $Commands.Count -eq 0) {
    throw 'Un paquete portable debe declarar al menos un comando.'
}
if ($InstallerType -eq 'exe' -and [string]::IsNullOrWhiteSpace($SilentSwitch)) {
    throw 'Un instalador exe requiere -SilentSwitch real; no se deben inventar switches.'
}
foreach ($command in $Commands) {
    if ($command -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw "Comando WinGet no válido: $command"
    }
}

function ConvertTo-YamlScalar([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

$root = Join-Path $PSScriptRoot 'winget'
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Force -Path $root | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$id = ConvertTo-YamlScalar $PackageIdentifier
$versionValue = ConvertTo-YamlScalar $Version
$publisherValue = ConvertTo-YamlScalar $Publisher
$publisherUrlValue = ConvertTo-YamlScalar $PublisherUrl
$nameValue = ConvertTo-YamlScalar $PackageName
$packageUrlValue = ConvertTo-YamlScalar $PackageUrl
$installerUrlValue = ConvertTo-YamlScalar $InstallerUrl

$versionManifest = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.9.0.schema.json
PackageIdentifier: $id
PackageVersion: $versionValue
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.9.0
"@

$localeManifest = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.9.0.schema.json
PackageIdentifier: $id
PackageVersion: $versionValue
PackageLocale: en-US
Publisher: $publisherValue
PublisherUrl: $publisherUrlValue
PackageName: $nameValue
PackageUrl: $packageUrlValue
License: MIT
ShortDescription: 'Milena SST data analysis language'
Description: 'Milena analyzes occupational safety and health data to detect statistical patterns and support accident prevention.'
ManifestType: defaultLocale
ManifestVersion: 1.9.0
"@

$installerLines = @(
    '# yaml-language-server: $schema=https://aka.ms/winget-manifest.installer.1.9.0.schema.json',
    "PackageIdentifier: $id",
    "PackageVersion: $versionValue",
    'Installers:',
    '- Architecture: x64',
    "  InstallerType: $InstallerType",
    "  InstallerUrl: $installerUrlValue",
    "  InstallerSha256: $($InstallerSha256.ToUpperInvariant())"
)
if ($InstallerType -eq 'portable') {
    $installerLines += '  Commands:'
    foreach ($command in $Commands) {
        $installerLines += "  - $(ConvertTo-YamlScalar $command)"
    }
} else {
    $installerLines += '  InstallerSwitches:'
    $installerLines += "    Silent: $(ConvertTo-YamlScalar $SilentSwitch)"
    if (-not [string]::IsNullOrWhiteSpace($SilentWithProgressSwitch)) {
        $installerLines += "    SilentWithProgress: $(ConvertTo-YamlScalar $SilentWithProgressSwitch)"
    }
}
$installerLines += @('ManifestType: installer', 'ManifestVersion: 1.9.0')

[System.IO.File]::WriteAllText((Join-Path $root "$PackageIdentifier.yaml"), $versionManifest, $utf8NoBom)
[System.IO.File]::WriteAllText((Join-Path $root "$PackageIdentifier.locale.en-US.yaml"), $localeManifest, $utf8NoBom)
[System.IO.File]::WriteAllLines((Join-Path $root "$PackageIdentifier.installer.yaml"), $installerLines, $utf8NoBom)

Write-Host "Manifest WinGet generado en $root"
Write-Host "Publisher: $Publisher | PackageIdentifier: $PackageIdentifier"
Write-Host "Tipo: $InstallerType | SHA-256: $($InstallerSha256.ToUpperInvariant())"
