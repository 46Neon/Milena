$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$tag = 'v0.2.0'
$repository = $env:GITHUB_REPOSITORY
$apiRoot = $env:GITHUB_API_URL
if ([string]::IsNullOrWhiteSpace($repository) -or [string]::IsNullOrWhiteSpace($apiRoot)) {
    throw 'GitHub Actions repository/API environment is missing.'
}
$apiRoot = $apiRoot.TrimEnd('/')

$releaseUri = "$apiRoot/repos/$repository/releases/tags/$tag"
$headers = @{
    Accept = 'application/vnd.github+json'
    Authorization = "Bearer $env:GITHUB_TOKEN"
    'X-GitHub-Api-Version' = '2022-11-28'
}
$release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
if ($release.tag_name -ne $tag -or $release.draft -or $release.prerelease) {
    throw "Expected the published stable release $tag."
}

$manifestDirectory = Join-Path $env:RUNNER_TEMP 'milena-winget-v0.2.0'
New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
$requiredAssets = @(
    '46Neon.Milena.yaml',
    '46Neon.Milena.locale.en-US.yaml',
    '46Neon.Milena.installer.yaml',
    'milena.exe.sha256'
)
$downloaded = @{}
foreach ($assetName in $requiredAssets) {
    $matches = @($release.assets | Where-Object { $_.name -eq $assetName })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one release asset named $assetName; found $($matches.Count)."
    }
    $asset = $matches[0]
    $assetUri = [Uri]$asset.browser_download_url
    if ($assetUri.Scheme -ne 'https' -or $assetUri.Host -ne 'github.com') {
        throw "Unexpected release asset host for $assetName."
    }
    $destinationDirectory = $manifestDirectory
    if ($assetName -eq 'milena.exe.sha256') {
        $destinationDirectory = $env:RUNNER_TEMP
    }
    $destination = Join-Path $destinationDirectory $assetName
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $destination
    $downloaded[$assetName] = $destination
}

$installerPath = $downloaded['46Neon.Milena.installer.yaml']
$installerText = Get-Content -Raw -Path $installerPath
$urlMatch = [Regex]::Match($installerText, '(?m)^\s*InstallerUrl:\s*''(?<url>[^'']+)''\s*$')
$hashMatch = [Regex]::Match($installerText, '(?m)^\s*InstallerSha256:\s*(?<hash>[0-9A-Fa-f]{64})\s*$')
if (-not $urlMatch.Success -or -not $hashMatch.Success) {
    throw 'Could not read InstallerUrl and InstallerSha256 from the WinGet installer manifest.'
}
$binaryAssets = @($release.assets | Where-Object { $_.name -eq 'milena.exe' })
if ($binaryAssets.Count -ne 1 -or $urlMatch.Groups['url'].Value -ne $binaryAssets[0].browser_download_url) {
    throw 'WinGet InstallerUrl does not match the published milena.exe asset for v0.2.0.'
}
$checksumText = (Get-Content -Raw -Path $downloaded['milena.exe.sha256']).Trim()
$checksumMatch = [Regex]::Match($checksumText, '^(?<hash>[0-9A-Fa-f]{64})\s+\*?milena\.exe$')
if (-not $checksumMatch.Success -or
    $checksumMatch.Groups['hash'].Value -ne $hashMatch.Groups['hash'].Value) {
    throw 'WinGet InstallerSha256 does not match the published milena.exe SHA-256 sidecar.'
}

$winget = Get-Command winget.exe -ErrorAction Stop
$wingetVersion = (& $winget.Source --version 2>&1 | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($wingetVersion)) {
    throw 'Could not determine the WinGet CLI version.'
}
Write-Host "WinGet version: $wingetVersion"

& $winget.Source validate --manifest $manifestDirectory --disable-interactivity
if ($LASTEXITCODE -ne 0) {
    throw "winget validate failed with exit code $LASTEXITCODE."
}

& $winget.Source settings --enable LocalManifestFiles
if ($LASTEXITCODE -ne 0) {
    throw "Could not enable local manifest installation (exit code $LASTEXITCODE)."
}
# This is a disposable CI runner; remove Store to avoid its agreement and region lookup.
& $winget.Source source remove --name msstore --disable-interactivity
if ($LASTEXITCODE -ne 0) {
    throw "Could not remove the Microsoft Store source from the disposable runner (exit code $LASTEXITCODE)."
}
& $winget.Source install --manifest $manifestDirectory `
    --accept-package-agreements --disable-interactivity
if ($LASTEXITCODE -ne 0) {
    throw "winget install --manifest failed with exit code $LASTEXITCODE."
}

$linkPath = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\milena.exe'
if (-not (Test-Path -LiteralPath $linkPath)) {
    $packageRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    $installedFiles = @(Get-ChildItem -Path $packageRoot -Filter 'milena.exe' -File -Recurse -ErrorAction SilentlyContinue)
    if ($installedFiles.Count -ne 1) {
        throw "Expected one installed milena.exe after WinGet installation; found $($installedFiles.Count)."
    }
    $linkPath = $installedFiles[0].FullName
}

$stdoutPath = Join-Path $env:RUNNER_TEMP 'milena-winget.stdout.txt'
$stderrPath = Join-Path $env:RUNNER_TEMP 'milena-winget.stderr.txt'
$process = Start-Process -FilePath $linkPath -Wait -PassThru -NoNewWindow `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
$programOutput = @(
    Get-Content -Raw -Path $stdoutPath -ErrorAction SilentlyContinue
    Get-Content -Raw -Path $stderrPath -ErrorAction SilentlyContinue
) -join "`n"
if ($process.ExitCode -ne 2 -or $programOutput -notmatch 'Milena 0\.2\.0') {
    throw "Installed WinGet package failed its version smoke test (exit $($process.ExitCode)): $programOutput"
}

# This is a disposable GitHub-hosted runner. WinGet cannot reliably uninstall
# a package installed from an unindexed local manifest; avoid a false cleanup
# gate or querying other package sources. The runner is discarded after this job.
Write-Host 'WinGet manifest validation, install, and smoke test passed; runner will be discarded.'
