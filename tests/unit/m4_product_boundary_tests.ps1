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

function Assert-SafeBoundaryInvocations {
    param([Parameter(Mandatory = $true)][string]$Source)

    $parseErrors = $null
    $tokens = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $Source, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) { throw 'boundary invocation safety source did not parse' }
    $boundedCommandName = 'Invoke-' + 'BoundedProcess'
    $calls = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq $boundedCommandName
    }, $true))
    if ($calls.Count -eq 0) { throw 'boundary invocation safety source has no bounded product calls' }
    $dynamicCommands = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            ($null -eq $node.GetCommandName() -or
             ($node.GetCommandName() -cne $boundedCommandName -and
              $node.GetCommandName().IndexOf($boundedCommandName, [StringComparison]::OrdinalIgnoreCase) -ge 0))
    }, $true))
    if ($dynamicCommands.Count -ne 0) {
        throw 'boundary invocation safety found a dynamic or qualified command invocation'
    }
    $commandNameTokens = @($tokens | Where-Object {
        $_.Text -ceq $boundedCommandName -and
        ($_.TokenFlags -band [Management.Automation.Language.TokenFlags]::CommandName)
    })
    $definitionCount = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $boundedCommandName
    }, $true)).Count
    $definitionTokens = @($tokens | Where-Object {
        $_.Text -ceq $boundedCommandName -and
        -not($_.TokenFlags -band [Management.Automation.Language.TokenFlags]::CommandName)
    })
    if ($commandNameTokens.Count -ne $calls.Count -or
        $definitionTokens.Count -ne $definitionCount) {
        throw 'boundary invocation safety found an unaudited bounded-process identifier'
    }
    if ($definitionCount -eq 1 -and $calls.Count -ne 7) {
        throw 'ordinary boundary must contain exactly seven audited bounded product calls'
    }

    $unknownAssignments = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
            $node.Left.VariablePath.UserPath -ceq 'unknownBundle'
    }, $true))
    $unknownUses = @($calls | Where-Object {
        $_.Extent.Text -match '\$unknownBundle\b'
    })
    if ($unknownUses.Count -gt 0) {
        if ($unknownUses.Count -ne 1 -or $unknownAssignments.Count -ne 1) {
            throw 'unknown bundle must have exactly one safe assignment and one bounded product call'
        }
        $safeUnknownAssignment = @'
$unknownBundle = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-unknown-' + [Guid]::NewGuid().ToString('N'))
'@.Trim()
        $normalizedAssignment = $unknownAssignments[0].Extent.Text -replace '\s+', ' '
        if ($unknownAssignments[0].Operator -ne [Management.Automation.Language.TokenKind]::Equals -or
            $normalizedAssignment -cne $safeUnknownAssignment) {
            throw 'unknown bundle assignment is not the frozen safe temporary path expression'
        }
        if ($unknownAssignments[0].Extent.EndOffset -ge $unknownUses[0].Extent.StartOffset) {
            throw 'unknown bundle assignment must precede its bounded product call'
        }
        if ($definitionCount -eq 1) {
            $gates = @($ast.FindAll({
                param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                    $node.GetCommandName() -ceq 'Assert-SafeBoundaryInvocations' -and
                    ($node.Extent.Text -replace '\s+', ' ') -ceq 'Assert-SafeBoundaryInvocations $boundarySource'
            }, $true))
            if ($gates.Count -ne 1 -or
                $gates[0].Extent.EndOffset -ge $unknownAssignments[0].Extent.StartOffset) {
                throw 'boundary source audit must run before the unknown bundle assignment'
            }
        }
    }
    elseif ($unknownAssignments.Count -ne 0) {
        throw 'unknown bundle assignment exists without its audited product call'
    }

    $safeLiterals = @(
        '', '--help', 'run --help', 'compact --help',
        'compact --bundle C:\missing-m4-product-bundle',
        'run --bundle x --size 64M --drive-letter X'
    )
    foreach ($call in $calls) {
        $elements = @($call.CommandElements)
        $argumentsAst = $null
        for ($i = 1; $i -lt $elements.Count - 1; ++$i) {
            if ($elements[$i] -is [Management.Automation.Language.CommandParameterAst] -and
                $elements[$i].ParameterName -ceq 'Arguments') {
                $argumentsAst = $elements[$i + 1]
                break
            }
        }
        if ($null -eq $argumentsAst) { throw 'bounded product call lacks an explicit -Arguments expression' }

        $literal = $null
        if ($argumentsAst -is [Management.Automation.Language.StringConstantExpressionAst]) {
            $literal = $argumentsAst.Value
        }
        elseif ($argumentsAst -is [Management.Automation.Language.ExpandableStringExpressionAst] -and
                $argumentsAst.NestedExpressions.Count -eq 0) {
            $literal = $argumentsAst.Value
        }
        if ($null -ne $literal) {
            if ($safeLiterals -cnotcontains $literal) {
                throw "unsafe bounded product arguments: $literal"
            }
            continue
        }

        $variables = @($argumentsAst.FindAll({
            param($node)
            $node -is [Management.Automation.Language.VariableExpressionAst]
        }, $true))
        $normalized = $argumentsAst.Extent.Text -replace '\s+', ' '
        $safeUnknownExpression = @'
('run --bundle "' + $unknownBundle + '" --size 64M --drive-letter R:')
'@.Trim()
        $safeUnknown = $variables.Count -eq 1 -and
            $variables[0].VariablePath.UserPath -ceq 'unknownBundle' -and
            $normalized -ceq $safeUnknownExpression
        if (-not $safeUnknown) {
            throw "unsafe nonliteral bounded product arguments: $($argumentsAst.Extent.Text)"
        }
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

    $usage = "Usage:`r`n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]`r`n  myrd compact --bundle <path>"
    if ($Result.ExitCode -ne 0) { throw 'myrd --help must exit 0' }
    if ($Result.Stderr -ne '') { throw 'myrd --help must keep stderr empty' }
    if ($Result.Stdout -cne ($usage + [Environment]::NewLine)) {
        throw 'myrd --help stdout must exactly match contracted usage'
    }
}

function Assert-UsageErrorContract {
    param([Parameter(Mandatory = $true)][psobject]$Result)

    $usage = "Usage:`r`n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]`r`n  myrd compact --bundle <path>"
    if ($Result.ExitCode -ne 2) { throw 'usage error must exit 2' }
    if ($Result.Stdout -cne '') { throw 'myrd with no arguments must keep stdout empty' }
    if ($Result.Stderr -cne ($usage + [Environment]::NewLine)) {
        throw 'no-argument stderr must exactly match contracted usage'
    }
}

function Assert-ExactProcessContract {
    param(
        [Parameter(Mandatory = $true)][psobject]$Result,
        [Parameter(Mandatory = $true)][int]$ExitCode,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Stdout,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Stderr,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Result.ExitCode -ne $ExitCode -or $Result.Stdout -cne $Stdout -or $Result.Stderr -cne $Stderr) {
        throw "$Name output purity or exit contract failed"
    }
}

function Remove-CppNonCode {
    param([Parameter(Mandatory = $true)][string]$Text)
    $out = [Text.StringBuilder]::new(); $state = 'code'; $escape = $false; $rawEnd = ''
    for ($i = 0; $i -lt $Text.Length; ++$i) {
        $c = $Text[$i]; $next = if ($i + 1 -lt $Text.Length) { $Text[$i + 1] } else { [char]0 }
        if ($state -eq 'line') { if ($c -eq "`n") { $state='code'; [void]$out.Append($c) }; continue }
        if ($state -eq 'block') { if ($c -eq '*' -and $next -eq '/') { ++$i; $state='code' }; continue }
        if ($state -eq 'raw') {
            if ($Text.Substring($i, [Math]::Min($Text.Length - $i, $rawEnd.Length)) -ceq $rawEnd) { $i += $rawEnd.Length - 1; $state='code' }
            continue
        }
        if ($state -eq 'string' -or $state -eq 'char') {
            if ($escape) { $escape=$false; continue }
            if ($c -eq '\') { $escape=$true; continue }
            if (($state -eq 'string' -and $c -eq '"') -or ($state -eq 'char' -and $c -eq "'")) { $state='code' }
            continue
        }
        if ($c -eq '/' -and $next -eq '/') { ++$i; $state='line'; continue }
        if ($c -eq '/' -and $next -eq '*') { ++$i; $state='block'; continue }
        if ($c -eq 'R' -and $next -eq '"' -and $i + 2 -lt $Text.Length) {
            $open = $Text.IndexOf('(', $i + 2)
            if ($open -lt 0 -or $open - $i - 2 -gt 16) { throw 'malformed or unterminated C++ raw literal' }
            $delimiter = $Text.Substring($i + 2, $open - ($i + 2))
            if ($delimiter.IndexOfAny([char[]]@(' ', '\', '(', ')', '"')) -ge 0 -or
                @($delimiter.ToCharArray() | Where-Object { [char]::IsControl($_) }).Count -ne 0) { throw 'malformed C++ raw literal delimiter' }
            $rawEnd = ')' + $delimiter + '"'; $i = $open; $state='raw'; continue
        }
        if ($c -eq '"') { $state='string'; continue }
        if ($c -eq "'") { $state='char'; continue }
        [void]$out.Append($c)
    }
    if ($state -eq 'raw') { throw 'unterminated C++ raw literal' }
    return $out.ToString()
}

function Find-CliRuntimeApi {
    param([Parameter(Mandatory = $true)][string]$Text)
    $code = Remove-CppNonCode $Text
    $patterns = @(
        '\bstd\s*::\s*filesystem\s*::\s*(status|exists|is_directory|directory_iterator|recursive_directory_iterator|canonical|weakly_canonical|create_[A-Za-z_]*|remove(_all)?|rename|copy(_[A-Za-z_]*)?|file_size|permissions|space|current_path|temp_directory_path)\s*\(',
        '\b(CreateFile[AW]?|GetFileAttributes[AW]?|FindFirstFile[AW]?|FindNextFile[AW]?|Reg[A-Za-z0-9_]*|DeviceIoControl|SetupDi[A-Za-z0-9_]*|CM_[A-Za-z0-9_]*|DefineDosDevice[AW]?|SetVolumeMountPoint[AW]?|GetVolume[A-Za-z0-9_]*|OpenSCManager[AW]?|OpenService[AW]?)\s*\(')
    foreach ($pattern in $patterns) { $match = [Regex]::Match($code, $pattern); if ($match.Success) { return $match.Value } }
    return $null
}

function Get-ExactlyOneIndex {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Needle,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $first = $Text.IndexOf($Needle, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "product main must call $Name" }
    if ($Text.IndexOf($Needle, $first + $Needle.Length, [StringComparison]::Ordinal) -ge 0) {
        throw "product main must call $Name exactly once"
    }
    return $first
}

function Measure-LiteralOccurrence {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Needle
    )
    $count = 0
    $index = $Text.IndexOf($Needle, [StringComparison]::Ordinal)
    while ($index -ge 0) {
        ++$count
        $index = $Text.IndexOf($Needle, $index + $Needle.Length, [StringComparison]::Ordinal)
    }
    return $count
}

# The service preflight is the only cheap, known-result driver gate. It must
# sit after elevation (so an unelevated run still reports elevation first) and
# before every bundle, image, provider and volume entry point, so that a
# disabled or absent phdskmnt produces zero bundle writes.
function Assert-DriverPreflightBoundary {
    param(
        [Parameter(Mandatory = $true)][string]$MainSource,
        [Parameter(Mandatory = $true)][string]$PreflightSource
    )

    $mainCode = Remove-CppNonCode $MainSource
    $wmainOffset = $mainCode.IndexOf('int wmain(', [StringComparison]::Ordinal)
    if ($wmainOffset -lt 0) { throw 'product main must define wmain' }
    $wmainCode = $mainCode.Substring($wmainOffset)

    $preflightCall = Get-ExactlyOneIndex $wmainCode 'require_phdskmnt_service_available()' 'the phdskmnt service preflight'
    if ((Measure-LiteralOccurrence $mainCode 'require_phdskmnt_service_available(') -ne 1) {
        throw 'create and recover must share exactly one phdskmnt preflight entry'
    }
    $elevationCall = $wmainCode.IndexOf('require_elevation()', [StringComparison]::Ordinal)
    if ($elevationCall -lt 0) { throw 'product main must call require_elevation' }
    if ($preflightCall -lt $elevationCall) {
        throw 'phdskmnt preflight must run after require_elevation so elevation keeps error priority'
    }
    foreach ($guarded in @(
        'classify_bundle(', 'create_image(', 'recover_complete_image(',
        'M3AimRuntime::create(', 'find_recovered_volume(', 'provision_new_volume(')) {
        $index = $wmainCode.IndexOf($guarded, [StringComparison]::Ordinal)
        if ($index -lt 0) { throw "product main must still call $guarded" }
        if ($preflightCall -ge $index) {
            throw "phdskmnt preflight must run before $guarded"
        }
    }

    $normalizedMain = $wmainCode -replace '\s+', ' '
    $expectedFailure = 'const auto driver_ready = myrd::require_phdskmnt_service_available(); if (!driver_ready.ok()) { print_run_error(options.bundle, driver_ready.error()); return 1; }'
    if ($normalizedMain.IndexOf($expectedFailure, [StringComparison]::Ordinal) -lt 0) {
        throw 'phdskmnt preflight failure must print through print_run_error and return 1'
    }

    # The frozen usage protocol lives in exactly one place, so a Win32 SCM
    # failure can never borrow it.
    if ((Measure-LiteralOccurrence $PreflightSource 'L"driver_unavailable"') -ne 1 -or
        (Measure-LiteralOccurrence $PreflightSource 'L"phdskmnt is disabled or absent"') -ne 1) {
        throw 'driver_unavailable operation and message must be frozen in exactly one place'
    }
    if ((Remove-CppNonCode $PreflightSource) -notmatch 'ErrorDomain::usage,\s*1,') {
        throw 'driver_unavailable must stay ErrorDomain::usage with code 1'
    }
    foreach ($scmOperation in @('L"OpenSCManagerW"', 'L"OpenServiceW"', 'L"QueryServiceConfigW"')) {
        if ((Measure-LiteralOccurrence $PreflightSource $scmOperation) -ne 1) {
            throw "SCM failures must keep exactly one distinct operation name: $scmOperation"
        }
    }
    $unavailableOffset = $PreflightSource.IndexOf('L"driver_unavailable"', [StringComparison]::Ordinal)
    foreach ($scmOperation in @('L"OpenSCManagerW"', 'L"OpenServiceW"', 'L"QueryServiceConfigW"')) {
        $operationOffset = $PreflightSource.IndexOf($scmOperation, [StringComparison]::Ordinal)
        if ([Math]::Abs($operationOffset - $unavailableOffset) -lt 8) {
            throw "SCM failure $scmOperation must not share the driver_unavailable error construction"
        }
    }
}

function Invoke-LineCounterFixture {
    param([Parameter(Mandatory = $true)][string]$MeasureScript)

    $fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-line-fixture-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\run') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\m3') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\persistence') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'tools') -Force | Out-Null
        Copy-Item -LiteralPath $MeasureScript -Destination (Join-Path $fixtureRoot 'tools\Measure-M4ProductLines.ps1')

        $utf8NoBom = [Text.UTF8Encoding]::new($false)
        $fixedFiles = @(
            'src/m3/m3_provider.h', 'src/m3/m3_provider.cpp',
            'src/m3/m3_windows_acceptance.h', 'src/m3/m3_windows_acceptance.cpp',
            'src/m3/m3_aim_server_main.cpp',
            'src/m3/m3_lifecycle_runner.h', 'src/m3/m3_lifecycle_runner.cpp',
            'src/persistence/persistence_types.h',
            'src/persistence/journal_writer.h', 'src/persistence/journal_writer.cpp'
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
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'src\persistence') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'tools') -Force | Out-Null
        Copy-Item -LiteralPath $MeasureScript -Destination (Join-Path $fixtureRoot 'tools\Measure-M4ProductLines.ps1')

        $utf8NoBom = [Text.UTF8Encoding]::new($false)
        $fixedFiles = @(
            'src/m3/m3_provider.h', 'src/m3/m3_provider.cpp',
            'src/m3/m3_windows_acceptance.h', 'src/m3/m3_windows_acceptance.cpp',
            'src/m3/m3_aim_server_main.cpp',
            'src/m3/m3_lifecycle_runner.h', 'src/m3/m3_lifecycle_runner.cpp',
            'src/persistence/persistence_types.h',
            'src/persistence/journal_writer.h', 'src/persistence/journal_writer.cpp'
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
        Stdout = "Usage:`r`n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]`r`n  myrd compact --bundle <path>" +
            [Environment]::NewLine
        Stderr = 'pollution'
    })
}
catch { $stderrRejected = $true }
if (-not $stderrRejected) { throw 'help contract self-audit accepted stderr pollution' }

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$cliSource = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\run\cli.cpp')
$mainSource = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\run\main.cpp')
$boundarySource = Get-Content -Raw $PSCommandPath
$boundedCommandName = 'Invoke-' + 'BoundedProcess'
foreach ($dangerousFixture in @(
    ($boundedCommandName + " -Path `$executablePath -Arguments 'run --bundle C:\danger --size 64M --drive-letter R:' -InvocationName 'renamed'"),
    ($boundedCommandName + ' -Path $executablePath -Arguments "run --bundle C:\danger --size 64M --drive-letter R:" -InvocationName "double quoted"'),
    ('$dangerousArguments = ''run --bundle C:\danger --size 64M --drive-letter R:''; ' + $boundedCommandName + ' -Path $executablePath -Arguments $dangerousArguments -InvocationName ''variable''')
)) {
    $rejected = $false
    try { Assert-SafeBoundaryInvocations $dangerousFixture } catch { $rejected = $true }
    if (-not $rejected) { throw 'boundary invocation safety self-audit missed a renamed, double-quoted, or variable mutating run' }
}
$safeUnknownCall = $boundedCommandName + @'
 -Path $executablePath -Arguments ('run --bundle "' + $unknownBundle + '" --size 64M --drive-letter R:') -InvocationName 'unknown nonempty bundle'
'@.TrimEnd()
foreach ($sourceMutation in @(
    ('$unknownBundle = ''C:\danger''; ' + $safeUnknownCall),
    ('$unknownBundle = Join-Path ([IO.Path]::GetTempPath()) (''myrd-m4-unknown-'' + [Guid]::NewGuid().ToString(''N'')); $unknownBundle += ''\danger''; ' + $safeUnknownCall),
    ('$unknownBundle = Join-Path ([IO.Path]::GetTempPath()) (''myrd-m4-unknown-'' + [Guid]::NewGuid().ToString(''N'')); ' + $safeUnknownCall + '; & $function:' + $boundedCommandName + ' -Path $executablePath -Arguments ''--help'' -InvocationName ''dynamic''')
)) {
    $rejected = $false
    try { Assert-SafeBoundaryInvocations $sourceMutation } catch { $rejected = $true }
    if (-not $rejected) { throw 'boundary invocation safety self-audit missed unsafe unknown source or indirect invocation' }
}
Assert-SafeBoundaryInvocations $boundarySource
if ($mainSource -match '\(void\)runtime\.value\(\)->stop\(\)') {
    throw 'product main must not ignore runtime cleanup after start failure'
}
$lifecycleSource = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\m3\m3_lifecycle_runner.cpp')
if ($lifecycleSource -match 'outcome\.generation\s*!=\s*image\.value\(\)\.checkpoint_generation\s*\+\s*1') {
    throw 'offline product wrapper must not compare against stale pre-lock discovery generation'
}
if ($mainSource -notmatch 'info\(\)\.virtual_bytes\s*!=\s*options\.virtual_bytes' -or
    $mainSource -notmatch 'maintenance_stats\(\)' -or
    $mainSource -notmatch 'recover\s*&&\s*stats\.value\(\)\.generation\s*==\s*0') {
    throw 'product main must validate runtime size and nonzero durable generation after start'
}
$preflightSource = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\run\driver_preflight.cpp')
Assert-DriverPreflightBoundary -MainSource $mainSource -PreflightSource $preflightSource
$cliRuntimeApi = Find-CliRuntimeApi $cliSource
if ($null -ne $cliRuntimeApi) { throw "CLI parser must not access runtime APIs: $cliRuntimeApi" }
foreach ($fixture in @(
    'std::filesystem::exists(path);',
    'CreateFileW(path, 0, 0, 0, 0, 0, 0);',
    'RegOpenKeyW(0, 0, 0, 0);',
    'DeviceIoControl(0, 0, 0, 0, 0, 0, 0, 0);')) {
    if ($null -eq (Find-CliRuntimeApi $fixture)) { throw "CLI runtime API scan self-audit missed: $fixture" }
}
foreach ($fixture in @(
    'std::filesystem::path path(value);',
    '// CreateFileW(path, 0, 0, 0, 0, 0, 0, 0);',
    'const char* text = "DeviceIoControl(";',
    'const char* raw = R"tag(embedded " quote DeviceIoControl()tag";',
    "wchar_t quote = 'R';")) {
    if ($null -ne (Find-CliRuntimeApi $fixture)) { throw "CLI runtime API scan false positive: $fixture" }
}
foreach ($delimiterControl in @([char]12, [char]11)) {
    $malformed = 'const char* raw = R"a' + $delimiterControl + 'b(DeviceIoControl()a' + $delimiterControl + 'b";'
    $rejected = $false
    try { [void](Find-CliRuntimeApi $malformed) } catch { $rejected = $true }
    if (-not $rejected) { throw 'CLI raw delimiter control-character self-audit did not fail closed' }
}
Assert-HelpContract (Invoke-BoundedProcess -Path $executablePath -Arguments '--help' -InvocationName 'myrd --help')
Assert-UsageErrorContract (Invoke-BoundedProcess -Path $executablePath -Arguments '' -InvocationName 'myrd')
$globalUsage = "Usage:`r`n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]`r`n  myrd compact --bundle <path>" + [Environment]::NewLine
$runUsage = "Usage:`r`n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]" + [Environment]::NewLine
$compactUsage = "Usage:`r`n  myrd compact --bundle <path>" + [Environment]::NewLine
Assert-ExactProcessContract (Invoke-BoundedProcess -Path $executablePath -Arguments 'run --help' -InvocationName 'myrd run --help') 0 $runUsage '' 'run help'
Assert-ExactProcessContract (Invoke-BoundedProcess -Path $executablePath -Arguments 'compact --help' -InvocationName 'myrd compact --help') 0 $compactUsage '' 'compact help'
$missingCompact = Invoke-BoundedProcess -Path $executablePath -Arguments 'compact --bundle C:\missing-m4-product-bundle' -InvocationName 'valid compact'
if ($missingCompact.ExitCode -ne 1 -or $missingCompact.Stdout -cne '' -or
    $missingCompact.Stderr -notmatch '^Error: compact failed for C:\\missing-m4-product-bundle: .+\r?\n$') {
    throw 'compact failure must include the bundle path and M3 diagnostic'
}
Assert-ExactProcessContract (Invoke-BoundedProcess -Path $executablePath -Arguments 'run --bundle x --size 64M --drive-letter X' -InvocationName 'invalid run') 2 '' ('invalid drive letter' + [Environment]::NewLine + $globalUsage) 'invalid run'
$unknownBundle = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-unknown-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($unknownBundle)
$marker = Join-Path $unknownBundle 'keep.txt'
[IO.File]::WriteAllText($marker, 'keep')
try {
    $unknownError = 'Error: run failed for ' + $unknownBundle + ': classify_bundle: non-empty bundle has no disk.rdm' + [Environment]::NewLine
    Assert-ExactProcessContract (Invoke-BoundedProcess -Path $executablePath -Arguments ('run --bundle "' + $unknownBundle + '" --size 64M --drive-letter R:') -InvocationName 'unknown nonempty bundle') 1 '' $unknownError 'unknown nonempty bundle'
    if (@(Get-ChildItem -LiteralPath $unknownBundle -Force).Count -ne 1 -or -not (Test-Path -LiteralPath $marker -PathType Leaf)) { throw 'unknown bundle was mutated' }
}
finally { Remove-Item -LiteralPath $marker -Force; Remove-Item -LiteralPath $unknownBundle -Force }

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
