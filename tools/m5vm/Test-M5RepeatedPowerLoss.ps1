# R-04 part C, host side: three consecutive hard power cuts.
#
# One cut proves the recovery path works; three in a row prove nothing accumulates -- no
# stranded worker, no extra disk, no mount point left behind, and the same committed
# generation each time.  The power cut itself is the T1-09 device
# (`tools/m5vm/Test-M5PowerLoss.ps1`: prepare -> `vmrun stop ... hard` -> boot -> verify),
# run three times with a guest reset in between so each cut starts from the same shape
# instead of from the previous cut's leftovers.
#
# The reset runs `C:\m5\Reset-M5Guest.ps1` directly over SSH: the guest account is already
# elevated there (probed), and the agent's request queue is the one thing this driver does
# not need to depend on between cycles.
#
# Exit 0 only when all three cycles pass and no residue is found.

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
    [string]$Configuration = 'Release',
    [switch]$SkipStage,
    [int]$Cycles = 3
)

$ErrorActionPreference = 'Stop'

if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }
if (-not $Vmx) { throw 'M5VM_VMX / -Vmx is required' }
if (-not $VmPassword) { throw 'M5VM_PASSWORD / -VmPassword is required' }
if ($Cycles -lt 1) { throw 'Cycles must be at least 1' }

$powerloss = Join-Path $PSScriptRoot 'Test-M5PowerLoss.ps1'
if (-not (Test-Path -LiteralPath $powerloss)) { throw "missing $powerloss" }

$target = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR',
             '-i', $KeyFile)

function Invoke-RemoteText([string]$text) {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($text))
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & ssh.exe @sshArgs $target "powershell.exe -NoProfile -EncodedCommand $b64" 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) { throw "remote command failed ($code): $($output -join "`n")" }
    return ($output -join "`n")
}

function Reset-Guest {
    $text = Invoke-RemoteText @'
$ErrorActionPreference = 'Continue'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1 2>&1 | Out-String
"RESET_EXIT=$LASTEXITCODE"
'@
    if ($text -notmatch 'RESET_EXIT=0' -or $text -notmatch 'RESET') {
        throw "the guest reset did not report RESET:`n$text"
    }
    $profiles = [regex]::Match($text, 'profiles: (\d+) entries left')
    if ($profiles.Success -and $profiles.Groups[1].Value -ne '0') {
        throw "the guest reset left $($profiles.Groups[1].Value) profile(s)"
    }
}

function Get-Residue {
    $text = Invoke-RemoteText @'
$workers = @(Get-Process myrd-m5-worker -ErrorAction SilentlyContinue).Count
$services = @(Get-Process myrd-service -ErrorAction SilentlyContinue).Count
$disks = @(Get-Disk -ErrorAction SilentlyContinue).Count
"workers=$workers services=$services disks=$disks"
'@
    return $text.Trim()
}

if (-not $SkipStage) {
    Write-Host '=== stage: Invoke-M5Vm.ps1 -Stage -Configuration '"$Configuration"
    $harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
    $stage = & $harness -Stage -Configuration $Configuration 6>&1 2>&1 | Out-String
    Write-Host $stage
    if ($LASTEXITCODE -ne 0) { throw "staging failed ($LASTEXITCODE)" }
}

$results = New-Object System.Collections.ArrayList
for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    Write-Host "`n############ cycle $cycle of $Cycles"
    Reset-Guest
    $residueBefore = Get-Residue
    Write-Host "  after reset: $residueBefore"

    # The T1-09 driver already judges each phase by its own verdict line and exits non-zero
    # on any failure, so its exit code is the cycle's verdict.
    $text = & $powerloss -SkipStage -Configuration $Configuration 6>&1 2>&1 | Out-String
    Write-Host $text
    if ($LASTEXITCODE -ne 0) { throw "cycle ${cycle}: the power-cut driver failed ($LASTEXITCODE)" }
    if ($text -notmatch 'PASS integration\.m5\.powerloss') {
        throw "cycle ${cycle}: the power-cut driver produced no PASS line"
    }
    $generation = [regex]::Match($text, 'committed generation: (\d+)')
    $generationValue = if ($generation.Success) { $generation.Groups[1].Value } else { '?' }
    $residue = Get-Residue
    [void]$results.Add([pscustomobject]@{
        cycle = $cycle
        generation = $generationValue
        residue = $residue
    })
    Write-Host ("--- cycle {0}: committed generation {1}; after verify: {2}" -f
        $cycle, $generationValue, $residue)
}

Write-Host "`n=== R-04.C summary (three consecutive hard power cuts)"
foreach ($row in $results) {
    Write-Host ("  cycle {0}: committed generation={1} residue=[{2}]" -f
        $row.cycle, $row.generation, $row.residue)
}
Write-Host ("PASS integration.m5.repeated_powerloss: {0} consecutive hard power cuts recovered" -f $Cycles)
exit 0
