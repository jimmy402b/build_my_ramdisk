[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Control', 'Runner')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$ChildFile,
    [ValidatePattern('^[0-9A-Fa-f]{40}$')][string]$SourceCommit,
    [string]$Phase,
    [string]$Executable,
    [string]$Bundle,
    [string]$DriveLetter,
    [string]$StateFile,
    [switch]$SafetyAcknowledged,
    [ValidateRange(1000, 600000)][int]$TimeoutMs = 20000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Quote-ChildArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    '"' + $Value.Replace('\\', '\\').Replace('"', '\\"') + '"'
}

if (-not [IO.File]::Exists($ChildFile)) { throw 'child file is absent' }
if ($Mode -eq 'Runner') {
    foreach ($value in @($Phase, $Executable, $Bundle, $DriveLetter, $StateFile)) {
        if ([string]::IsNullOrWhiteSpace($value)) { throw 'runner argument is missing' }
    }
}

$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Quote-ChildArgument $ChildFile))
if ($Mode -eq 'Runner') {
    $arguments += @('-Phase', (Quote-ChildArgument $Phase), '-Executable', (Quote-ChildArgument $Executable), '-Bundle', (Quote-ChildArgument $Bundle), '-DriveLetter', (Quote-ChildArgument $DriveLetter), '-StateFile', (Quote-ChildArgument $StateFile))
    if ($SafetyAcknowledged) { $arguments += '-SafetyAcknowledged' }
}

$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = 'powershell.exe'
$psi.Arguments = $arguments -join ' '
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
if (-not [string]::IsNullOrWhiteSpace($SourceCommit)) { $psi.EnvironmentVariables['MYRD_M4_SOURCE_COMMIT'] = $SourceCommit }
if ($Mode -eq 'Runner' -and $SafetyAcknowledged) { $psi.EnvironmentVariables['MYRD_M4_APPROVED_VM'] = '1' }

$process = New-Object Diagnostics.Process
$process.StartInfo = $psi
$started = [datetime]::UtcNow
$stdout = ''
$stderr = ''
$exit = -1
try {
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutMs)) { $process.Kill() }
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $exit = $process.ExitCode
}
finally {
    $ended = [datetime]::UtcNow
    $record = [ordered]@{mode = $Mode; child_pid = $process.Id; start_utc = $started.ToString('o'); end_utc = $ended.ToString('o'); child_exit = $exit; child_stdout = $stdout; child_stderr = $stderr}
    [Console]::Out.WriteLine(($record | ConvertTo-Json -Compress))
    $process.Dispose()
}
