$ErrorActionPreference = 'Stop'
 $harnessText = Get-Content -Raw (Join-Path $PSScriptRoot '..\e2e\m4_vm_multi_disk.ps1')
if ($harnessText -match 'sha256\s*=\s*try\s*\{[\s\S]*\}\s*catch\s*\{\s*\$null') {
    throw 'bundle hash failures must not be silently represented as null'
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('m4-harness-' + [guid]::NewGuid().ToString('N'))
try {
    $parent = New-Item -ItemType Directory -Path (Join-Path $root 'parent') -Force
    $bundleA = Join-Path $parent.FullName 'a'
    $bundleB = Join-Path $parent.FullName 'b'
    $evidence = Join-Path $parent.FullName 'evidence'
    $script = Join-Path $PSScriptRoot '..\e2e\m4_vm_multi_disk.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Phase Preflight -Executable (Join-Path $PSScriptRoot '..\..\out\build\msvc\src\Release\myrd.exe') -BundleA $bundleA -BundleB $bundleB -EvidenceDirectory $evidence 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 2) { throw 'multi-disk preflight harness failed on empty observations' }
    Write-Output 'M4 VM multi-disk harness tests PASS'
}
finally { if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force } }
