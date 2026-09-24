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
    'analysis.c', 'script.c', 'entrypoints.c', 'interpreter.c', 'main.c', 'lexer.c', 'ast.c', 'parser.c',
    'symbol_table.c', 'symbol.c', 'language_semantic.c', 'arrow_ipc.c', 'language_runtime.c', 'query_plan.c', 'language_grouped_spill.c', 'grouped_aggregate.c', 'mergeable_aggregate.c', 'spill_store.c', 'canonical_compiler.c', 'stream.c', 'source_reader.c', 'group_key_codec.c', 'sst_dates.c', 'sst_model.c',
    'sst_stats.c', 'sst_histogram.c', 'sst_rates.c', 'sst_report.c',
    'sst_report_advanced.c', 'sst_advanced.c', 'sst_contingency.c',
    'sst_inference.c', 'sst_correlation.c', 'sst_normality.c',
    'logger.c', 'metrics.c', 'function_parser.c', 'user_functions.c',
    'third_party/nanoarrow/src/nanoarrow.c', 'third_party/nanoarrow/src/nanoarrow_ipc.c',
    'third_party/nanoarrow/src/flatcc.c',
    'sqlite_backend.c', 'third_party/sqlite/sqlite3.c'
)
$Compiler = if ($env:CC) { $env:CC } else { 'clang' }
$VersionHeader = Join-Path $ObjectDir 'milena-version.h'
("#define MILENA_VERSION `"$Version`"") | Set-Content -Encoding ascii -Path $VersionHeader
$Flags = @(
    '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wconversion',
    '-O2', '-Iinclude', '-Ithird_party/nanoarrow/include', '-Ithird_party/sqlite',
    '-DSQLITE_THREADSAFE=1', '-DSQLITE_DQS=0', '-DSQLITE_OMIT_LOAD_EXTENSION',
    '-include', $VersionHeader
)
$Objects = @()

foreach ($SourceName in $SourceNames) {
    $Source = if ($SourceName -like 'third_party/*') {
        Join-Path $Root $SourceName
    } else {
        Join-Path $Root "src/$SourceName"
    }
    if (-not (Test-Path $Source)) { throw "Fuente Windows ausente: $SourceName" }
    $Object = Join-Path $ObjectDir ($SourceName -replace '\.c$', '.o')
    $ObjectParent = Split-Path -Parent $Object
    New-Item -ItemType Directory -Force -Path $ObjectParent | Out-Null
    Write-Host "[Windows] Compilando $SourceName"
    $SourceFlags = @()
    if ($SourceName -like 'third_party/sqlite/*') { $SourceFlags += '-w' }
    & $Compiler @Flags @SourceFlags '-c' $Source '-o' $Object
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
$LicenseDir = Join-Path $OutputDir 'licenses'
New-Item -ItemType Directory -Force -Path $LicenseDir | Out-Null
Copy-Item (Join-Path $Root 'LICENSE') (Join-Path $LicenseDir 'Milena-LICENSE') -Force
Copy-Item (Join-Path $Root 'third_party/nanoarrow/LICENSE.txt') (Join-Path $LicenseDir 'nanoarrow-LICENSE.txt') -Force
Copy-Item (Join-Path $Root 'third_party/nanoarrow/NOTICE.txt') (Join-Path $LicenseDir 'nanoarrow-NOTICE.txt') -Force
Copy-Item (Join-Path $Root 'third_party/nanoarrow/FLATCC-LICENSE.txt') (Join-Path $LicenseDir 'flatcc-LICENSE.txt') -Force
Copy-Item (Join-Path $Root 'third_party/sqlite/README.md') (Join-Path $LicenseDir 'sqlite-PROVENANCE-LICENSE.md') -Force
