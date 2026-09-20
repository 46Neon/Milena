$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Version = if ($env:MANO_VERSION) { $env:MANO_VERSION } else { '0.1.1' }
$OutputDir = Join-Path $Root 'dist/windows'
$Output = Join-Path $OutputDir 'milena.exe'
$ObjectDir = Join-Path $OutputDir 'objects'

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
New-Item -ItemType Directory -Force -Path $ObjectDir | Out-Null
& (Join-Path $PSScriptRoot 'array-link-smoke.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Falló el smoke test de portabilidad de MilenaArray' }

$SourceNames = @(
 'analysis.c', 'array.c', 'common.c', 'dataset.c', 'logger.c', 'main.c', 'metrics.c',
 'schema.c', 'script.c', 'sst_advanced.c', 'sst_contingency.c',
 'sst_correlation.c', 'sst_dates.c', 'sst_histogram.c', 'sst_inference.c',
 'sst_model.c', 'sst_normality.c', 'sst_rates.c', 'sst_report.c',
 'sst_report_advanced.c', 'sst_stats.c'
)
$Compiler = if ($env:CC) { $env:CC } else { 'clang' }
$Flags = @('-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-O2', '-Iinclude')
$Objects = @()

foreach ($SourceName in $SourceNames) {
    $Source = Join-Path $Root "src/$SourceName"
    $Object = Join-Path $ObjectDir ($SourceName -replace '\.c$', '.o')
    Write-Host "[Windows] Compilando $SourceName"
    & $Compiler @Flags '-c' $Source '-o' $Object
    if ($LASTEXITCODE -ne 0) {
        throw "Falló la compilación Windows de $SourceName"
    }
    $Objects += $Object
}

Write-Host '[Windows] Enlazando milena.exe'
& $Compiler @Objects '-o' $Output
if ($LASTEXITCODE -ne 0) {
    throw 'Falló el enlace Windows de milena.exe'
}

Write-Host "Ejecutable creado: $Output"
Write-Host 'Pendiente: crear instalador firmado y ejecutar las pruebas Windows.'
