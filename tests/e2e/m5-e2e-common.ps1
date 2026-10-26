# Shared helpers for the M5 guest-side end-to-end scripts.
#
# Dot-source this from a script that has already set `$cliExe` and `$serviceName`.
# The CLI is always reached as a *user* reaches it: a fresh process per command, and
# nothing but its exit code and its text.
#
# Deliberately not a refactor of `test-m5-cli-e2e.ps1`: that script is the accepted
# T1-05/T1-07/T1-08 evidence, and moving its helpers out would change the file those
# readings came from.  The duplication is the cheaper of the two risks.

function Write-Step([string]$message) {
    Write-Host "--- $message"
}

function Fail([string]$message) {
    [Console]::Error.WriteLine("FAIL: $message")
    exit 1
}

function Invoke-Cli {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $started = Get-Date
        $output = & $cliExe @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
        return [pscustomobject]@{
            output    = $output
            exit      = $exit
            ms        = [int]((Get-Date) - $started).TotalMilliseconds
            arguments = ($Arguments -join ' ')
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Invoke-Sc {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & sc.exe @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($exit -ne 0) {
        Fail "sc.exe $($Arguments -join ' ') failed ($exit): $output"
    }
    return $output
}

function Get-ServiceState {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & sc.exe query $serviceName 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $previous
    }
    return $output
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text, '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
    if (-not $match.Success) { return $null }
    return $match.Value
}

function Get-OperationState([string]$operationId) {
    # Exit 1 is expected: an operation that *failed* is a product failure and the CLI
    # reports it as one.  Only an unreadable answer is fatal.
    $result = Invoke-Cli @('operation', 'show', $operationId)
    foreach ($word in @('queued', 'running', 'reconciling', 'succeeded', 'failed', 'cancelled')) {
        if ($result.output -match "operation $operationId`: $word") { return $word }
    }
    Fail "operation show $operationId did not report a state (exit $($result.exit)): $($result.output)"
}

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    $state = 'unknown'
    while ((Get-Date) -lt $until) {
        $state = Get-OperationState $operationId
        if (@('succeeded', 'failed', 'cancelled') -contains $state) { return $state }
        Start-Sleep -Milliseconds 500
    }
    Fail "the operation did not reach a terminal state within $seconds s (last state: $state)"
}

function Wait-ForService([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $probe = Invoke-Cli @('disk', 'list')
        if ($probe.exit -eq 0) { return $probe }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Remove-ScService {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & sc.exe query $serviceName 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            if ((Get-ServiceState) -match 'RUNNING') {
                & sc.exe stop $serviceName 2>&1 | Out-Null
                for ($i = 0; $i -lt 120; $i++) {
                    if ((Get-ServiceState) -match 'STOPPED') { break }
                    Start-Sleep -Milliseconds 500
                }
            }
            & sc.exe delete $serviceName 2>&1 | Out-Null
            Start-Sleep -Milliseconds 500
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

# A drive letter the product will accept: free in the volume namespace *and* the DOS
# device namespace.  `Test-Path` alone is not enough -- a failed create can leave a
# letter mapped to a device that no longer exists, and only `QueryDosDevice` sees it.
function Select-FreeLetter {
    if (-not ([System.Management.Automation.PSTypeName]'Myrd.DosDevice').Type) {
        Add-Type -Namespace Myrd -Name DosDevice -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern int QueryDosDevice(string name, System.Text.StringBuilder target, int max);
'@
    }
    $occupied = @()
    foreach ($volume in (Get-CimInstance -ClassName Win32_Volume -ErrorAction Stop)) {
        if ($volume.DriveLetter) { $occupied += $volume.DriveLetter.ToUpper() }
    }
    foreach ($disk in (Get-CimInstance -ClassName Win32_LogicalDisk -ErrorAction Stop)) {
        if ($disk.DeviceID) { $occupied += $disk.DeviceID.ToUpper() }
    }
    foreach ($letter in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if ($occupied -contains $letter) { continue }
        $buffer = New-Object System.Text.StringBuilder 512
        if ([Myrd.DosDevice]::QueryDosDevice($letter, $buffer, 512) -ne 0) { continue }
        return $letter
    }
    return $null
}
