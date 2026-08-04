$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'M2Harness.Common.ps1')

$temporary = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m2-native-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $marker = Join-Path $temporary 'child-completed.txt'
    $child = Join-Path $temporary 'child.ps1'
    @"
`$ErrorActionPreference = 'Stop'
[Console]::Error.WriteLine('native-stderr-probe')
Start-Sleep -Milliseconds 100
[IO.File]::WriteAllText('$($marker.Replace("'", "''"))', 'completed')
[Console]::Out.WriteLine('native-stdout-probe')
exit 7
"@ | Set-Content -LiteralPath $child -Encoding utf8

    $result = Invoke-M2NativeProcess -FilePath (Join-Path $PSHOME 'powershell.exe') -Arguments @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $child)

    if ([int]$result.ExitCode -ne 7) { throw "unexpected exit code: $($result.ExitCode)" }
    if ([string]$result.StandardError -notmatch 'native-stderr-probe') { throw 'stderr was not captured' }
    if ([string]$result.StandardOutput -notmatch 'native-stdout-probe') { throw 'stdout was not captured' }
    if (-not [IO.File]::Exists($marker) -or [IO.File]::ReadAllText($marker) -ne 'completed') {
        throw 'parent did not wait for the stderr-writing child to complete'
    }
    Write-Output 'M2 native process: 4/4 PASS'
}
finally {
    if ([IO.Directory]::Exists($temporary)) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
