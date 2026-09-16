# Run elevated. Enables local development without enabling remote debugging.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$logDirectory = Join-Path $projectRoot 'artifacts/setup'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this configuration script with administrator privileges.'
}
$developmentKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
$previous = Get-ItemProperty -LiteralPath $developmentKey -ErrorAction SilentlyContinue
$previousValue = if ($null -ne $previous) { $previous.AllowDevelopmentWithoutDevLicense } else { $null }
$priorStateFile = Join-Path $logDirectory 'developer-mode-before.json'
if (-not (Test-Path -LiteralPath $priorStateFile)) {
    @{ AllowDevelopmentWithoutDevLicense = $previousValue } |
        ConvertTo-Json | Set-Content -LiteralPath $priorStateFile
}
New-Item -Path $developmentKey -Force | Out-Null
New-ItemProperty -LiteralPath $developmentKey -Name AllowDevelopmentWithoutDevLicense -Value 1 -PropertyType DWord -Force | Out-Null
$computer = Get-CimInstance Win32_ComputerSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$volumes = Get-Volume -DriveLetter C,E | Select-Object DriveLetter,FileSystem,SizeRemaining,Size
[ordered]@{
    ConfiguredAt = (Get-Date).ToString('o')
    DeveloperMode = (Get-ItemProperty -LiteralPath $developmentKey).AllowDevelopmentWithoutDevLicense
    RAMBytes = $computer.TotalPhysicalMemory
    Processor = $cpu.Name
    LogicalProcessors = $computer.NumberOfLogicalProcessors
    Volumes = $volumes
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logDirectory 'machine.json')
