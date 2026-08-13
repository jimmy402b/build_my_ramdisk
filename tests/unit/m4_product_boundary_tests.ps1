[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Executable)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Arguments,
        [Parameter(Mandatory = $true)][string]$InvocationName
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Path
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    function Stop-ProcessBestEffort {
        param([Parameter(Mandatory = $true)][Diagnostics.Process]$Target)
        try {
            if (-not $Target.HasExited) { $Target.Kill() }
            [void]$Target.WaitForExit(2000)
        }
        catch { }
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw "unable to start $InvocationName" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) {
            Stop-ProcessBestEffort $process
            throw "$InvocationName timed out after 10 seconds"
        }
        $readTasks = [Threading.Tasks.Task[]]@($stdoutTask, $stderrTask)
        if (-not [Threading.Tasks.Task]::WaitAll($readTasks, 5000)) {
            Stop-ProcessBestEffort $process
            throw "$InvocationName output capture timed out"
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.Result
            Stderr = $stderrTask.Result
        }
    }
    finally {
        if ($started) { Stop-ProcessBestEffort $process }
        try { $process.Dispose() } catch { }
    }
}

function Test-ByteSubsequence {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Haystack,
        [Parameter(Mandatory = $true)][byte[]]$Needle
    )
    if ($Needle.Length -eq 0) { return $true }
    if ($Needle.Length -gt $Haystack.Length) { return $false }
    for ($start = 0; $start -le $Haystack.Length - $Needle.Length; ++$start) {
        $matches = $true
        for ($offset = 0; $offset -lt $Needle.Length; ++$offset) {
            if ($Haystack[$start + $offset] -ne $Needle[$offset]) { $matches = $false; break }
        }
        if ($matches) { return $true }
    }
    return $false
}

function Assert-HelpContract {
    param([Parameter(Mandatory = $true)][psobject]$Result)

    $usage = 'Usage: myrd run --bundle <path> --size <N> --drive-letter <R:> [options]'
    if ($Result.ExitCode -ne 0) { throw 'myrd --help must exit 0' }
    if ($Result.Stderr -ne '') { throw 'myrd --help must keep stderr empty' }
    if ($Result.Stdout -cne ($usage + [Environment]::NewLine)) {
        throw 'myrd --help stdout must exactly match contracted usage'
    }
}

function Assert-UsageErrorContract {
    param([Parameter(Mandatory = $true)][psobject]$Result)

    $usage = 'Usage: myrd run --bundle <path> --size <N> --drive-letter <R:> [options]'
    if ($Result.ExitCode -ne 2) { throw 'myrd with no arguments must exit 2' }
    if ($Result.Stdout -cne '') { throw 'myrd with no arguments must keep stdout empty' }
    if ($Result.Stderr -cne ($usage + [Environment]::NewLine)) {
        throw 'myrd with no arguments stderr must exactly match contracted usage'
    }
}

function Invoke-LineCounterFixture {
    param([Parameter(Mandatory = $true)][string]$MeasureScript)

    $fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-line-fixture-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\run') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\m3') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'tools') -Force | Out-Null
        Copy-Item -LiteralPath $MeasureScript -Destination (Join-Path $fixtureRoot 'tools\Measure-M4ProductLines.ps1')

        $utf8NoBom = [Text.UTF8Encoding]::new($false)
        $fixedFiles = @(
            'src/m3/m3_provider.h', 'src/m3/m3_provider.cpp',
            'src/m3/m3_windows_acceptance.h', 'src/m3/m3_windows_acceptance.cpp',
            'src/m3/m3_aim_server_main.cpp'
        )
        foreach ($relativePath in $fixedFiles) {
            [IO.File]::WriteAllText((Join-Path $fixtureRoot $relativePath), "// fixed base`n", $utf8NoBom)
        }
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/main.cpp'), "#include <iostream>`nint main() {`nint counter = 0;`n", $utf8NoBom)

        & git.exe -C $fixtureRoot init | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'fixture git init failed' }
        & git.exe -C $fixtureRoot config user.email 'm4-fixture@example.invalid'
        if ($LASTEXITCODE -ne 0) { throw 'fixture git email config failed' }
        & git.exe -C $fixtureRoot config user.name 'M4 Fixture'
        if ($LASTEXITCODE -ne 0) { throw 'fixture git name config failed' }
        & git.exe -C $fixtureRoot add src
        if ($LASTEXITCODE -ne 0) { throw 'fixture git add failed' }
        & git.exe -C $fixtureRoot commit -m base | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'fixture git base commit failed' }
        $base = (& git.exe -C $fixtureRoot rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($base)) { throw 'fixture base lookup failed' }

        [IO.File]::AppendAllText((Join-Path $fixtureRoot 'src/m3/m3_provider.cpp'), "#define TRACKED_CHANGE 1`r`n", $utf8NoBom)
        [IO.File]::AppendAllText((Join-Path $fixtureRoot 'src/run/main.cpp'), "+++counter;`nint following = counter;`nreturn following;`n}`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/.hidden.cpp'), "#include <vector>`n/*`n pure block comment`n*/`nint hidden = 1; /* code remains */`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/fixture.ps1'), "# pure PowerShell comment`r`nWrite-Output 'counted'`r`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/blocks.cpp'), "/*`n plain block text`n # still block text`n*/`n/* single-line block */`nint before = 0; /* trailing block */`n/* leading block */ int after = 1;`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/escaped.cpp'), "const char* text = `"escaped quote: \`" /* //`";`nint real = 1;`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/multiline.cpp'), "const char* continued = `"first " + [char]92 + "`n/* // still string`ncontinued`";`nint afterString = 1;`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/raw.cpp'), "const char* raw = R`"tag(`n/* // raw contents`n)tag`";`nint afterRaw = 1;`n", $utf8NoBom)
        [IO.File]::WriteAllBytes((Join-Path $fixtureRoot 'src/run/bom.cpp'), [byte[]]@(0xEF, 0xBB, 0xBF) + $utf8NoBom.GetBytes("// BOM comment`nint bom = 1;`n"))
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/powershell-blocks.ps1'), "<#`n plain block text`n # still block text`n#>`n<# single-line block #>`nWrite-Output 'before' <# trailing block #>`n<# leading block #> Write-Output 'after'`n", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $fixtureRoot 'src/run/powershell-quoted.ps1'), "`$single = '<# #>'`n`$double = `"<# #>`"`n`$singleHere = @'`n<#`n#>`n'@`n`$doubleHere = @`"`n<#`n#>`n`"@`nWrite-Output 'after strings'`n", $utf8NoBom)
        [IO.File]::WriteAllBytes((Join-Path $fixtureRoot 'src/run/bom.ps1'), [byte[]]@(0xEF, 0xBB, 0xBF) + $utf8NoBom.GetBytes("# BOM comment`nWrite-Output 'bom'`n"))

        $fixtureMeasure = Join-Path $fixtureRoot 'tools\Measure-M4ProductLines.ps1'
        $oldErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fixtureMeasure -Base $base 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $oldErrorActionPreference }
        if ($exitCode -ne 0) { throw "fixture line counter failed: $($output -join '; ')" }
        $expected = @{
            'src/m3/m3_provider.cpp' = 1
            'src/run/.hidden.cpp' = 2
            'src/run/blocks.cpp' = 2
            'src/run/escaped.cpp' = 2
            'src/run/multiline.cpp' = 4
            'src/run/raw.cpp' = 4
            'src/run/bom.cpp' = 1
            'src/run/fixture.ps1' = 1
            'src/run/powershell-blocks.ps1' = 2
            'src/run/powershell-quoted.ps1' = 11
            'src/run/bom.ps1' = 1
            'src/run/main.cpp' = 4
            'TOTAL' = 35
        }
        foreach ($name in $expected.Keys) {
            $line = @($output | Where-Object { $_ -match ('^' + [Regex]::Escape($name) + '=') })
            if ($line.Count -ne 1) { throw "fixture missing exact result for ${name}: $($output -join '; ')" }
            $actual = [int]($line[0] -replace '^[^=]+=', '' -replace ' MAXIMUM=.*$', '')
            if ($actual -ne $expected[$name]) { throw "fixture expected $name=$($expected[$name]), got $actual" }
        }

        $oldErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $invalidBaseOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fixtureMeasure -Base 'not-a-commit' 2>&1)
            $invalidBaseExitCode = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $oldErrorActionPreference }
        if ($invalidBaseExitCode -eq 0) { throw "fixture invalid Base unexpectedly succeeded: $($invalidBaseOutput -join '; ')" }

        [IO.File]::WriteAllBytes((Join-Path $fixtureRoot 'src/run/binary.cpp'), [byte[]]@(0x23, 0x00, 0x69, 0x00))
        $oldErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $binaryOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fixtureMeasure -Base $base 2>&1)
            $binaryExitCode = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $oldErrorActionPreference }
        if ($binaryExitCode -eq 0) { throw "fixture binary source unexpectedly succeeded: $($binaryOutput -join '; ')" }
    }
    finally {
        if (Test-Path -LiteralPath $fixtureRoot) {
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $fixtureRoot) {
                Write-Warning "unable to remove fixture directory: $fixtureRoot"
            }
        }
    }
}

function Invoke-BinaryBaseLineCounterFixture {
    param([Parameter(Mandatory = $true)][string]$MeasureScript)

    $fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-binary-base-fixture-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\m3') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'tools') -Force | Out-Null
        Copy-Item -LiteralPath $MeasureScript -Destination (Join-Path $fixtureRoot 'tools\Measure-M4ProductLines.ps1')

        $utf8NoBom = [Text.UTF8Encoding]::new($false)
        $fixedFiles = @(
            'src/m3/m3_provider.h', 'src/m3/m3_provider.cpp',
            'src/m3/m3_windows_acceptance.h', 'src/m3/m3_windows_acceptance.cpp',
            'src/m3/m3_aim_server_main.cpp'
        )
        foreach ($fixedFile in $fixedFiles) {
            $fixedFullPath = Join-Path $fixtureRoot $fixedFile
            if ($fixedFile -ceq 'src/m3/m3_windows_acceptance.cpp') {
                [IO.File]::WriteAllBytes($fixedFullPath, [byte[]]@(0x23, 0x00, 0x69, 0x00))
            }
            else {
                [IO.File]::WriteAllText($fixedFullPath, "// fixed base`n", $utf8NoBom)
            }
        }
        $relativePath = 'src/m3/m3_windows_acceptance.cpp'
        $fullPath = Join-Path $fixtureRoot $relativePath

        & git.exe -C $fixtureRoot init | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'binary-base fixture git init failed' }
        & git.exe -C $fixtureRoot config user.email 'm4-fixture@example.invalid'
        if ($LASTEXITCODE -ne 0) { throw 'binary-base fixture git email config failed' }
        & git.exe -C $fixtureRoot config user.name 'M4 Fixture'
        if ($LASTEXITCODE -ne 0) { throw 'binary-base fixture git name config failed' }
        & git.exe -C $fixtureRoot add src
        if ($LASTEXITCODE -ne 0) { throw 'binary-base fixture git add failed' }
        & git.exe -C $fixtureRoot commit -m binary-base | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'binary-base fixture git base commit failed' }
        $base = (& git.exe -C $fixtureRoot rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($base)) { throw 'binary-base fixture base lookup failed' }

        [IO.File]::WriteAllText($fullPath, "int text_source = 1;`n", $utf8NoBom)
        $fixtureMeasure = Join-Path $fixtureRoot 'tools\Measure-M4ProductLines.ps1'
        $oldErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fixtureMeasure -Base $base 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $oldErrorActionPreference }
        if ($exitCode -eq 0) {
            throw "binary-base fixture unexpectedly succeeded: $($output -join '; ')"
        }
        $combinedOutput = $output -join "`n"
        if ($combinedOutput -notmatch [Regex]::Escape('unexpected diff format for src/m3/m3_windows_acceptance.cpp')) {
            throw "binary-base fixture did not report the no-hunk fail-closed error for ${relativePath}: $combinedOutput"
        }
    }
    finally {
        if (Test-Path -LiteralPath $fixtureRoot) {
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $fixtureRoot) {
                Write-Warning "unable to remove binary-base fixture directory: $fixtureRoot"
            }
        }
    }
}

$stderrRejected = $false
try {
    Assert-HelpContract ([pscustomobject]@{
        ExitCode = 0
        Stdout = 'Usage: myrd run --bundle <path> --size <N> --drive-letter <R:> [options]' +
            [Environment]::NewLine
        Stderr = 'pollution'
    })
}
catch { $stderrRejected = $true }
if (-not $stderrRejected) { throw 'help contract self-audit accepted stderr pollution' }

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
Assert-HelpContract (Invoke-BoundedProcess -Path $executablePath -Arguments '--help' -InvocationName 'myrd --help')
Assert-UsageErrorContract (Invoke-BoundedProcess -Path $executablePath -Arguments '' -InvocationName 'myrd')

$bytes = [IO.File]::ReadAllBytes($executablePath)
$forbidden = @(
    'MyRamDiskService', 'MyRamDiskHost', 'operation_store',
    'profile_registration', 'public_pipe_server', 'worker_supervisor'
)
foreach ($token in $forbidden) {
    $asciiBytes = [Text.Encoding]::ASCII.GetBytes($token)
    $utf16Bytes = [Text.Encoding]::Unicode.GetBytes($token)
    if ((Test-ByteSubsequence -Haystack $bytes -Needle $asciiBytes) -or
        (Test-ByteSubsequence -Haystack $bytes -Needle $utf16Bytes)) {
        throw "forbidden product token: $token"
    }
}

$byteAuditToken = 'm4-byte-audit'
if (-not (Test-ByteSubsequence -Haystack ([Text.Encoding]::ASCII.GetBytes($byteAuditToken)) -Needle ([Text.Encoding]::ASCII.GetBytes($byteAuditToken)))) {
    throw 'ASCII forbidden-token scan self-audit failed'
}
if (-not (Test-ByteSubsequence -Haystack ([Text.Encoding]::Unicode.GetBytes($byteAuditToken)) -Needle ([Text.Encoding]::Unicode.GetBytes($byteAuditToken)))) {
    throw 'UTF-16LE forbidden-token scan self-audit failed'
}

$measure = Join-Path $PSScriptRoot '..\..\tools\Measure-M4ProductLines.ps1'
Invoke-LineCounterFixture -MeasureScript $measure
Invoke-BinaryBaseLineCounterFixture -MeasureScript $measure
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $measure
if ($LASTEXITCODE -ne 0) { throw 'default product line count must succeed' }

$oldErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $measure -Maximum 0
    $maximumZeroExitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $oldErrorActionPreference
}
if ($maximumZeroExitCode -eq 0) {
    throw 'line count must fail when Maximum is 0'
}

Write-Output 'M4 product boundary PASS'
