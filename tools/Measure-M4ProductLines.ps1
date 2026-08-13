[CmdletBinding()]
param(
    [string]$Base = '4f9f912de56160062a007483fc56d5a02d5ff613',
    [int]$Maximum = 2500
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Test-SafeRelativePath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    foreach ($character in $RelativePath.ToCharArray()) {
        if ([char]::IsControl($character) -or [int][char]$character -gt 127) {
            throw "unsafe product path cannot be counted: $RelativePath"
        }
    }
}

function Get-SourceLines {
    param([Parameter(Mandatory = $true)][string]$FullPath)
    $bytes = [IO.File]::ReadAllBytes($FullPath)
    if ($bytes -contains [byte]0) { throw "binary product file cannot be counted: $FullPath" }
    try {
        $text = ([Text.UTF8Encoding]::new($false, $true)).GetString($bytes)
    }
    catch { throw "non-UTF-8 product file cannot be counted: $FullPath" }
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    return @($text -split "`r?`n", 0)
}

function Get-CountedLineMap {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Lines,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    $result = @{}
    $extension = [IO.Path]::GetExtension($RelativePath).ToLowerInvariant()
    $isCStyle = $extension -in @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.rc')
    $isPowerShell = $extension -in @('.ps1', '.psm1', '.psd1')
    $inBlockComment = $false
    $powerShellQuote = [char]0
    $powerShellHereQuote = [char]0
    $cQuote = [char]0
    $rawEnd = $null
    $rawUnknown = $false
    for ($lineNumber = 1; $lineNumber -le $Lines.Count; ++$lineNumber) {
        $line = $Lines[$lineNumber - 1]
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($isPowerShell) {
            if ($powerShellHereQuote -ne [char]0) {
                $result[$lineNumber] = $true
                if ($line.StartsWith(([string]$powerShellHereQuote + '@'))) { $powerShellHereQuote = [char]0 }
                continue
            }
            $hasCode = $false
            for ($index = 0; $index -lt $line.Length; ++$index) {
                $character = $line[$index]
                $next = if ($index + 1 -lt $line.Length) { $line[$index + 1] } else { [char]0 }
                if ($inBlockComment) {
                    if ($character -eq '#' -and $next -eq '>') { $inBlockComment = $false; ++$index }
                    continue
                }
                if ($powerShellQuote -ne [char]0) {
                    $hasCode = $true
                    if ($powerShellQuote -eq '"' -and $character -eq '`' -and $index + 1 -lt $line.Length) { ++$index; continue }
                    if ($character -eq $powerShellQuote) { $powerShellQuote = [char]0 }
                    continue
                }
                if ($character -eq '#') { break }
                if ($character -eq '<' -and $next -eq '#') { $inBlockComment = $true; ++$index; continue }
                if ($character -eq '@' -and ($next -eq '"' -or $next -eq "'")) { $hasCode = $true; $powerShellHereQuote = $next; ++$index; continue }
                if ($character -eq '"' -or $character -eq "'") { $hasCode = $true; $powerShellQuote = $character; continue }
                if (-not [char]::IsWhiteSpace($character)) { $hasCode = $true }
            }
            if ($hasCode) { $result[$lineNumber] = $true }
            continue
        }
        if (-not $isCStyle) { $result[$lineNumber] = $true; continue }

        $hasCode = $false
        if ($rawUnknown) { $result[$lineNumber] = $true; continue }
        for ($index = 0; $index -lt $line.Length; ++$index) {
            $character = $line[$index]
            $next = if ($index + 1 -lt $line.Length) { $line[$index + 1] } else { [char]0 }
            if ($inBlockComment) {
                if ($character -eq '*' -and $next -eq '/') { $inBlockComment = $false; ++$index }
                continue
            }
            if ($null -ne $rawEnd) {
                $hasCode = $true
                if ($index + $rawEnd.Length -le $line.Length -and $line.Substring($index, $rawEnd.Length) -ceq $rawEnd) {
                    $index += $rawEnd.Length - 1
                    $rawEnd = $null
                }
                continue
            }
            if ($cQuote -ne [char]0) {
                $hasCode = $true
                if ($character -eq [char]92 -and $index + 1 -lt $line.Length) { ++$index; continue }
                if ($character -eq $cQuote) { $cQuote = [char]0 }
                continue
            }
            if ($character -eq '/' -and $next -eq '/') { break }
            if ($character -eq '/' -and $next -eq '*') { $inBlockComment = $true; ++$index; continue }
            if ($character -eq 'R' -and $next -eq '"') {
                $openParenthesis = $line.IndexOf('(', $index + 2)
                if ($openParenthesis -ge 0) {
                    $delimiter = $line.Substring($index + 2, $openParenthesis - ($index + 2))
                    if ($delimiter.Length -le 16 -and $delimiter -notmatch '[\s()\\]') {
                        $hasCode = $true
                        $rawEnd = ')' + $delimiter + '"'
                        $index = $openParenthesis
                        continue
                    }
                }
                $hasCode = $true
                $rawUnknown = $true
                continue
            }
            if ($character -eq '"' -or $character -eq "'") { $hasCode = $true; $cQuote = $character; continue }
            if (-not [char]::IsWhiteSpace($character)) { $hasCode = $true }
        }
        if ($cQuote -ne [char]0) {
            $backslashCount = 0
            for ($tail = $line.Length - 1; $tail -ge 0 -and $line[$tail] -eq [char]92; --$tail) { ++$backslashCount }
            if (($backslashCount % 2) -eq 0) { $cQuote = [char]0 }
        }
        if ($hasCode) { $result[$lineNumber] = $true }
    }
    return $result
}

function Invoke-GitLines {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = @(& git.exe @Arguments 2>&1); $exitCode = $LASTEXITCODE }
    finally { $ErrorActionPreference = $oldErrorActionPreference }
    return [pscustomobject]@{ Output = $output; ExitCode = $exitCode }
}

function Get-AddedCount {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$FullPath,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot
    )
    Test-SafeRelativePath $RelativePath
    $baseListing = Invoke-GitLines @('-C', $RepositoryRoot, 'ls-tree', '-r', '--name-only', $Base, '--', $RelativePath)
    if ($baseListing.ExitCode -ne 0) { throw "unable to inspect base tree for ${RelativePath}: $($baseListing.Output -join '; ')" }
    $listing = @($baseListing.Output | Where-Object { $_ -is [string] -and $_ -ne '' })
    if ($listing.Count -eq 0) { $existsAtBase = $false }
    elseif ($listing.Count -eq 1 -and [string]$listing[0] -ceq $RelativePath) { $existsAtBase = $true }
    else { throw "unexpected base tree result for ${RelativePath}: $($listing -join '; ')" }

    $countedLines = Get-CountedLineMap -Lines (Get-SourceLines $FullPath) -RelativePath $RelativePath
    if (-not $existsAtBase) { return $countedLines.Count }

    $diff = Invoke-GitLines @('-C', $RepositoryRoot, 'diff', '--no-ext-diff', '--no-color', '--unified=0', '--ignore-cr-at-eol', $Base, '--', $RelativePath)
    if ($diff.ExitCode -gt 1) { throw "unable to diff product file ${RelativePath}: $($diff.Output -join '; ')" }
    $diffLines = @($diff.Output | Where-Object { $_ -is [string] })
    if ($diffLines.Count -eq 0) { return 0 }
    $newLine = $null
    $seenHunk = $false
    $count = 0
    foreach ($line in $diff.Output) {
        if ($line -match '^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@') { $newLine = [int]$Matches[1]; $seenHunk = $true; continue }
        if (-not $seenHunk) { continue }
        if ($null -eq $newLine) { continue }
        if ($line -isnot [string]) { throw "non-text diff output for ${RelativePath}" }
        if ($line.StartsWith('+')) {
            if ($countedLines.ContainsKey($newLine)) { ++$count }
            ++$newLine
        }
        elseif ($line.StartsWith('-')) { continue }
        elseif ($line.StartsWith(' ')) { ++$newLine }
        elseif ($line.StartsWith('\\ No newline at end of file')) { continue }
        else { throw "unexpected diff line for ${RelativePath}: $line" }
    }
    if (-not $seenHunk) { throw "unexpected diff format for ${RelativePath}" }
    return $count
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$baseVerify = Invoke-GitLines @('-C', $repositoryRoot, 'rev-parse', '--verify', "$Base^{commit}")
if ($baseVerify.ExitCode -ne 0) { throw "base commit is unavailable: $Base" }

$relativePaths = @()
$runRoot = Join-Path $repositoryRoot 'src\run'
if (Test-Path -LiteralPath $runRoot) {
    $relativePaths += Get-ChildItem -LiteralPath $runRoot -File -Recurse -Force |
        ForEach-Object { $_.FullName.Substring($repositoryRoot.Length + 1).Replace('\', '/') }
}
$relativePaths += @('src/m3/m3_provider.h', 'src/m3/m3_provider.cpp', 'src/m3/m3_windows_acceptance.h', 'src/m3/m3_windows_acceptance.cpp', 'src/m3/m3_aim_server_main.cpp')

$total = 0
foreach ($relativePath in $relativePaths | Sort-Object -Unique) {
    $fullPath = Join-Path $repositoryRoot ($relativePath.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) { throw "product file disappeared while counting: $relativePath" }
    $count = Get-AddedCount -RelativePath $relativePath -FullPath $fullPath -RepositoryRoot $repositoryRoot
    Write-Output "$relativePath=$count"
    $total += $count
}
Write-Output "TOTAL=$total MAXIMUM=$Maximum"
if ($total -gt $Maximum) { throw "product line budget exceeded: $total > $Maximum" }
