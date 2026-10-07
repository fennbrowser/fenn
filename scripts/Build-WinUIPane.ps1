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
$output = Join-Path $projectRoot 'build/winui-pane/x64/Release'
# Framework-dependent on Microsoft.WindowsAppRuntime.2: hosts join the shared
# runtime before loading the pane (the WinUI activation decision),
# so the build needs no activation manifests, app-local runtime or merged PRI.
if (-not (Test-Path (Join-Path $output 'Microsoft.WindowsAppRuntime.Bootstrap.dll'))) {
    throw 'The build did not produce Microsoft.WindowsAppRuntime.Bootstrap.dll, which unpackaged hosts load.'
}
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
