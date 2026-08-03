[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'

function Invoke-M2 {
    param([string[]]$Arguments)

    $output = @(& $Executable @Arguments 2>&1)
    [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Text = ($output | Out-String)
    }
}

$help = Invoke-M2 -Arguments @('--help')
if ($help.ExitCode -ne 0 -or $help.Text -notmatch 'Usage:') {
    throw "M2 --help failed: exit=$($help.ExitCode), output=$($help.Text)"
}

$schema = Invoke-M2 -Arguments @('--print-schema')
if ($schema.ExitCode -ne 0 -or
    $schema.Text -notmatch '"schema_version"') {
    throw "M2 --print-schema failed: exit=$($schema.ExitCode)"
}

$root = Join-Path ([IO.Path]::GetTempPath()) (
    'myrd-m2-cli-smoke-{0}' -f [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null

try {
    $fake = Invoke-M2 -Arguments @(
        '--local-fake',
        '--evidence-dir',
        $root)
    if ($fake.ExitCode -ne 0) {
        throw "M2 --local-fake failed: exit=$($fake.ExitCode), output=$($fake.Text)"
    }
    $match = [regex]::Match(
        $fake.Text,
        'M2 local fake report:\s*(.+\.json)\s*$')
    if (-not $match.Success) {
        throw "M2 --local-fake did not print a report path: $($fake.Text)"
    }
    $reportPath = $match.Groups[1].Value.Trim()
    if (-not [IO.File]::Exists($reportPath)) {
        throw "M2 --local-fake report does not exist: $reportPath"
    }

    $validation = Invoke-M2 -Arguments @(
        '--validate-report',
        $reportPath)
    if ($validation.ExitCode -ne 0 -or
        $validation.Text -notmatch 'M2 report is valid') {
        throw "M2 report validation failed: exit=$($validation.ExitCode), output=$($validation.Text)"
    }
}
finally {
    if ([IO.Directory]::Exists($root)) {
        $item = Get-Item -LiteralPath $root -Force
        if (($item.Attributes -band
             [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to remove reparse-point test directory: $root"
        }
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
