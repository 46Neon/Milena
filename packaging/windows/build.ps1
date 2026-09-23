$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Version = if ($env:MILENA_VERSION) { $env:MILENA_VERSION.TrimStart('v') } else { '0.2.0' }
if ($Version -notmatch '^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$') {
    throw "MILENA_VERSION no es una versión válida: $Version"
}
$OutputDir = Join-Path $Root 'dist/windows'
$Output = Join-Path $OutputDir 'milena.exe'
$ObjectDir = Join-Path $OutputDir 'objects'

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (Test-Path $ObjectDir) { Remove-Item -Recurse -Force $ObjectDir }
New-Item -ItemType Directory -Force -Path $ObjectDir | Out-Null

& (Join-Path $PSScriptRoot 'array-link-smoke.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Falló el smoke test de portabilidad de MilenaArray' }

$SourceNames = @(
    'common.c', 'array.c', 'table.c', 'finance.c', 'schema.c', 'dataset.c',
    'analysis.c', 'script.c', 'entrypoints.c', 'main.c', 'lexer.c', 'ast.c', 'parser.c',
    'symbol_table.c', 'symbol.c', 'language_semantic.c', 'language_runtime.c', 'language_grouped_spill.c', 'grouped_aggregate.c', 'mergeable_aggregate.c', 'spill_store.c', 'canonical_compiler.c', 'stream.c', 'sst_dates.c', 'sst_model.c',
    'sst_stats.c', 'sst_histogram.c', 'sst_rates.c', 'sst_report.c',
    'sst_report_advanced.c', 'sst_advanced.c', 'sst_contingency.c',
    'sst_inference.c', 'sst_correlation.c', 'sst_normality.c',
    'logger.c', 'metrics.c', 'function_parser.c', 'user_functions.c' 
)
$Compiler = if ($env:CC) { $env:CC } else { 'clang' }
$VersionHeader = Join-Path $ObjectDir 'milena-version.h'
("#define MILENA_VERSION `"$Version`"") | Set-Content -Encoding ascii -Path $VersionHeader
$Flags = @(
    '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wconversion',
    '-O2', '-Iinclude', '-include', $VersionHeader
)
$Objects = @()

foreach ($SourceName in $SourceNames) {
    $Source = Join-Path $Root "src/$SourceName"
    if (-not (Test-Path $Source)) { throw "Fuente Windows ausente: $SourceName" }
    $Object = Join-Path $ObjectDir ($SourceName -replace '\.c$', '.o')
    Write-Host "[Windows] Compilando $SourceName"
    & $Compiler @Flags '-c' $Source '-o' $Object
    if ($LASTEXITCODE -ne 0) { throw "Falló la compilación Windows de $SourceName" }
    $Objects += $Object
}

Write-Host '[Windows] Enlazando milena.exe'
& $Compiler @Objects '-o' $Output
if ($LASTEXITCODE -ne 0) { throw 'Falló el enlace Windows de milena.exe' }
if (-not (Test-Path $Output)) { throw "No se generó el ejecutable: $Output" }

$Hash = (Get-FileHash -Algorithm SHA256 -Path $Output).Hash.ToLowerInvariant()
"$Hash *milena.exe" | Set-Content -NoNewline (Join-Path $OutputDir 'milena.exe.sha256')
Write-Host "Milena $Version creado: $Output"
Write-Host "SHA-256: $Hash"
