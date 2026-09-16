param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MachArguments
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$bash = 'C:\mozilla-build\msys2\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw 'Install MozillaBuild before running mach.' }
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
$env:MSYS2_PATH_TYPE = 'inherit'
$env:EXTERNAL_TO_MOZILLABUILD_SSH_DIR = 'C:\Windows\System32\OpenSSH'
$machScript = (Join-Path $PSScriptRoot 'mach.sh').Replace('\', '/')
& $bash --login $machScript @MachArguments
exit $LASTEXITCODE
