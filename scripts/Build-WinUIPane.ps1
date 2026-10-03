param([switch]$Stage, [switch]$Restore, [switch]$Test, [switch]$ContractTest,
      [switch]$AddressTest, [switch]$MenuTest, [switch]$TabStripTest)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$installation = & 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe' -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($Restore) {
    & (Join-Path $projectRoot 'artifacts/setup/nuget.exe') restore (Join-Path $projectRoot 'native/WinUIPane/packages.config') -PackagesDirectory (Join-Path $projectRoot '.cache/nuget/native') -Source https://api.nuget.org/v3/index.json -NonInteractive
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
& (Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe') (Join-Path $projectRoot 'native/WinUIPane/FirefoxWinUIPane.vcxproj') /m:2 /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$manifestArgs = @('--check')
$firefoxManifest = Join-Path $projectRoot 'upstream/firefox/browser/app/firefox.exe.manifest'
if ($ContractTest -and -not (Test-Path $firefoxManifest)) {
    # A source-only hosted checkout can still verify the generated canonical
    # activation manifest. Local Firefox checkouts retain the full comparison.
    $manifestArgs += '--canonical-only'
}
& python (Join-Path $PSScriptRoot 'Export-WinUIPaneManifest.py') @manifestArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$output = Join-Path $projectRoot 'build/winui-pane/x64/Release'
$priInput = Join-Path $projectRoot 'build/winui-pane/pri-input'
New-Item -ItemType Directory -Force $priInput | Out-Null
foreach ($name in @('Microsoft.UI.Xaml.Controls.pri', 'Microsoft.UI.pri', 'Microsoft.WindowsAppRuntime.pri')) {
    Copy-Item (Join-Path $output $name) $priInput -Force
}
$priLog = Join-Path $projectRoot 'build/winui-pane/makepri.log'
& 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/makepri.exe' new /pr $priInput /cf (Join-Path $projectRoot 'configs/winui-pane-pri.xml') /of (Join-Path $output 'resources.pri') /o > $priLog
if ($LASTEXITCODE) {
    Get-Content $priLog -Encoding Unicode
    exit $LASTEXITCODE
}
Write-Host "Merged native theme resources: $output/resources.pri"
if ($Test -or $ContractTest -or $AddressTest -or $MenuTest -or $TabStripTest) {
    if ($TabStripTest) {
        & (Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe') (Join-Path $projectRoot 'tests/native/ExternalDragSource.vcxproj') /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    & (Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe') (Join-Path $projectRoot 'tests/native/PaneContractTests.vcxproj') /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($ContractTest) {
        # Hosted CI has no interactive desktop. This production-DLL lane uses
        # programmatic ABI/lifecycle/accessibility contracts only; physical
        # SendInput suites continue to run through Invoke-UiaTestVM.ps1.
        $productionDll = (Resolve-Path (Join-Path $output 'FirefoxWinUIPane.dll')).Path
        & (Join-Path $output 'PaneContractTests.exe') $productionDll
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
}
if ($Test -or $AddressTest -or $MenuTest -or $TabStripTest) {
    if ($Test) {
        & (Join-Path $output 'PaneContractTests.exe') (Join-Path $output 'FirefoxWinUIPane.dll')
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    # Inspect live XAML only in a separate test DLL; never stage test exports.
    & (Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe') (Join-Path $projectRoot 'native/WinUIPane/FirefoxWinUIPane.vcxproj') /m:2 /p:Configuration=Release /p:Platform=x64 /p:WinUIPaneTesting=true /verbosity:minimal
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $testMode = if ($TabStripTest) { '--strip' } elseif ($MenuTest) { '--menu' } elseif ($AddressTest) { '--address' } else { '--inspect' }
    & (Join-Path $output 'PaneContractTests.exe') (Join-Path $output 'FirefoxWinUIPaneTest.dll') $testMode
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($TabStripTest) {
        # Motion needs loaded visuals. Run this in a fresh process: the earlier
        # hide/show/teardown fixture can leave recreated rows measured but unloaded.
        & (Join-Path $output 'PaneContractTests.exe') (Join-Path $output 'FirefoxWinUIPaneTest.dll') --inline
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
}
if ($Stage) {
    & python (Join-Path $PSScriptRoot 'Stage-WinUIPane.py')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
