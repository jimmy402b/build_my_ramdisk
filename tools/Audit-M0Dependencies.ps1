param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$forbidden = @('aimapi.dll','aim_ll.exe','RamDyn.exe','AIMtk-Dlg.exe',
    'AIMtk-svc.exe','aim_cli.exe','MountImg.exe','RamDiskUI.exe','deviosvc.exe')
$forbiddenLower = $forbidden | ForEach-Object { $_.ToLowerInvariant() }
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)
if (-not $dumpbin) {
    . $PSScriptRoot\Enter-MsvcEnvironment.ps1
}
$out = & dumpbin.exe /dependents $Executable 2>&1
$lines = $out -split "`n"
$found = @()
foreach ($line in $lines) {
    $trimmed = $line.Trim().ToLowerInvariant()
    if ($forbiddenLower -contains $trimmed) { $found += $line.Trim() }
}
if ($found.Count -gt 0) {
    Write-Output "FORBIDDEN DEPENDENCIES FOUND:"
    $found | ForEach-Object { Write-Output "  $_" }
    exit 1
}
Write-Output "Import audit PASS: no forbidden dependencies"
