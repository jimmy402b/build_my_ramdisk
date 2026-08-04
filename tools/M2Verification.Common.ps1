Set-StrictMode -Version Latest

function Confirm-M2PublishedReport {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RunDirectory,
        [Parameter(Mandatory = $true)]
        [ValidateSet('M2-V4', 'M2-V5', 'M2-V6', 'M2-V7')]
        [string]$Tier,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$Verifier
    )

    $reports = @(Get-ChildItem -LiteralPath $RunDirectory -File -Filter 'm2-report-*.json')
    if ($reports.Count -ne 1) {
        throw "expected exactly one immutable M2 report, found $($reports.Count)"
    }

    $reportPath = $reports[0].FullName
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $verificationOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier -Path $reportPath -Tier $Tier -Executable $Executable 2>&1)
        $verificationExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    [IO.File]::WriteAllLines(
        (Join-Path $RunDirectory 'verification.log'),
        @($verificationOutput | ForEach-Object { [string]$_ }))
    if ($verificationExitCode -ne 0) {
        throw "independent M2 report verification failed: $($verificationOutput -join [Environment]::NewLine)"
    }
    if (($verificationOutput -join [Environment]::NewLine) -notmatch 'M2 JSON validation PASS') {
        throw 'independent verifier did not emit PASS'
    }
    return $reportPath
}
