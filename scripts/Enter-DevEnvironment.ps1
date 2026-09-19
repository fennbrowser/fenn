# Dot-source this script before running project build commands.
$projectRoot = Split-Path -Parent $PSScriptRoot
$env:MOZBUILD_STATE_PATH = Join-Path $projectRoot '.cache/mozbuild'
# Installed toolchains and package caches are read-mostly and safe to share between
# checkouts: set FIREFOX_WINUI_SHARED_CACHE to another checkout's .cache to reuse them.
# mozbuild state and sccache stay per-checkout so builds cannot interfere.
$sharedCache = if ($env:FIREFOX_WINUI_SHARED_CACHE) { $env:FIREFOX_WINUI_SHARED_CACHE } else { Join-Path $projectRoot '.cache' }
$env:CARGO_HOME = Join-Path $sharedCache 'cargo'
$env:RUSTUP_HOME = Join-Path $sharedCache 'rustup'
$env:NUGET_PACKAGES = Join-Path $sharedCache 'nuget/packages'
$env:NUGET_HTTP_CACHE_PATH = Join-Path $sharedCache 'nuget/http'
$env:PIP_CACHE_DIR = Join-Path $sharedCache 'pip'
$env:SCCACHE_DIR = Join-Path $projectRoot '.cache/sccache'
foreach ($cachePath in @($env:MOZBUILD_STATE_PATH, $env:CARGO_HOME, $env:RUSTUP_HOME,
    $env:NUGET_PACKAGES, $env:NUGET_HTTP_CACHE_PATH, $env:PIP_CACHE_DIR, $env:SCCACHE_DIR)) {
    New-Item -ItemType Directory -Force -Path $cachePath | Out-Null
}
$toolPaths = @(
    (Join-Path $env:CARGO_HOME 'bin'),
    'C:\mozilla-build\python3',
    'C:\mozilla-build\python3\Scripts',
    'C:\mozilla-build\msys2\usr\bin',
    'C:\Program Files\dotnet'
) | Where-Object { Test-Path -LiteralPath $_ }
$env:PATH = ($toolPaths -join ';') + ';' + $env:PATH
if (Test-Path -LiteralPath 'C:\mozilla-build') {
    $env:MOZILLABUILD = 'C:\mozilla-build\'
}
Write-Host "Project caches configured on E: for $projectRoot"
