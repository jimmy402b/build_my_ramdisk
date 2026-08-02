param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$forbidden = @('aimapi.dll','devio.dll','aim_ll.exe','RamDyn.exe','AIMtk.exe',
    'AIMtk-Dlg.exe','AIMtk-svc.exe','aim_cli.exe','MountImg.exe',
    'RamDiskUI.exe','deviosvc.exe')
$forbiddenLower = $forbidden | ForEach-Object { $_.ToLowerInvariant() }
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)
if (-not $dumpbin) {
    . $PSScriptRoot\Enter-MsvcEnvironment.ps1
}
$out = & dumpbin.exe /dependents $Executable 2>&1
$dumpbinExitCode = $LASTEXITCODE
if ($dumpbinExitCode -ne 0) {
    $out | ForEach-Object { Write-Output $_ }
    throw "dumpbin dependency inspection failed (exit $dumpbinExitCode)"
}
$lines = $out -split "`n"
$found = @()
foreach ($line in $lines) {
    $trimmed = $line.Trim().ToLowerInvariant()
    if ($forbiddenLower -contains $trimmed) { $found += $line.Trim() }
}
if ($found.Count -gt 0) {
    Write-Output "FORBIDDEN DEPENDENCIES FOUND:"
    $found | ForEach-Object { Write-Output "  $_" }
    throw 'forbidden dependency detected'
}
Write-Output "Import audit PASS: no forbidden dependencies"
