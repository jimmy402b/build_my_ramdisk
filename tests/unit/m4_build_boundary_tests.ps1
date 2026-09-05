[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$ProbeBaseDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Generator,

    [Parameter(Mandatory = $true)]
    [string]$Compiler,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedExecutableNames
)

$ErrorActionPreference = 'Stop'

$sourceRoot = [System.IO.Path]::GetFullPath($SourceDirectory)
$probeBaseRoot = [System.IO.Path]::GetFullPath($ProbeBaseDirectory)
$expectedProbeBaseRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'out\build\m4b'))

if (-not $probeBaseRoot.Equals(
        $expectedProbeBaseRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use unexpected M4-boundary probe base: $probeBaseRoot"
}

if (-not (Test-Path -LiteralPath $probeBaseRoot)) {
    New-Item -ItemType Directory -Path $probeBaseRoot -Force | Out-Null
}

$probeBaseItem = Get-Item -LiteralPath $probeBaseRoot -Force
if (($probeBaseItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing to use reparse-point M4-boundary probe base: $probeBaseRoot"
}

$probeRunName = 'r-{0}-{1}' -f $PID, ([System.Guid]::NewGuid().ToString('N').Substring(0, 8))
$probeRoot = [System.IO.Path]::GetFullPath((Join-Path $probeBaseRoot $probeRunName))
$probePrefix = $probeBaseRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $probeRoot.StartsWith(
        $probePrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Generated M4-boundary probe path escaped its base: $probeRoot"
}

New-Item -ItemType Directory -Path $probeRoot | Out-Null

$requiredTargets = @(
    'myrd_product_contracts'
    'myrd_product_ipc'
    'myrd_profile'
    'myrd_management_core'
    'myrd_host_core'
    'myrd_diagnostics'
    'myrd_cli_core'
)

$lowerLayerTargets = @(
    'myrd_storage'
    'myrd_proxy'
    'myrd_driver'
    'myrd_persistence'
    'myrd_poc'
    'myrd_m1'
    'myrd_m1_report'
    'myrd_m2'
    'myrd_m2_report'
    'myrd_m2_windows'
    'myrd_m3_management'
    'myrd_m3_windows'
)

$forbiddenBoundaries = @{}
foreach ($targetName in $requiredTargets) {
    if ($targetName -ne 'myrd_host_core') {
        $forbiddenBoundaries[$targetName] = $lowerLayerTargets
    }
}

$m3Facing = @('myrd_persistence','myrd_m3_management','myrd_m3_windows','myrd_m1','myrd_m2')

function Parse-CMakeGraphViz {
    param([string]$DotPath)

    if (-not (Test-Path -LiteralPath $DotPath)) {
        throw "GraphViz output not found: $DotPath"
    }

    $lines = Get-Content -LiteralPath $DotPath
    if ($lines.Count -eq 0) {
        throw 'GraphViz output is empty'
    }

    $idToLabel = [System.Collections.Generic.Dictionary[string, string]]::new(
        [System.StringComparer]::Ordinal)
    $labelToId = [System.Collections.Generic.Dictionary[string, string]]::new(
        [System.StringComparer]::Ordinal)
    $edges = [System.Collections.Generic.List[object]]::new()

    # Header must start with digraph or graph
    $header = $lines[0].Trim()
    if ($header -notmatch '^(di)?graph\s') {
        throw "GraphViz output missing expected header; first line: $header"
    }

    $inLegend = $false
    for ($i = 1; $i -lt $lines.Count; $i++) {
        $line = $lines[$i].Trim()
        if ($line.Length -eq 0) { continue }

        # CMake emits an unquoted legend subgraph before the quoted target graph.
        # It is presentation metadata, not part of the dependency model.
        if ($line -match '^subgraph\s+clusterLegend\s*\{$') {
            $inLegend = $true
            continue
        }
        if ($inLegend) {
            if ($line -eq '}') { $inLegend = $false }
            continue
        }

        # Edge endpoints are GraphViz node IDs. Translate them to labels only
        # after all node declarations have been parsed.
        if ($line -match '->') {
            if ($line -match '^"(?<src>[^"]+)"\s*->\s*"(?<dst>[^"]+)"(?:\s*\[[^\]]*\])?\s*;?\s*(?://.*)?$') {
                $edges.Add(@($Matches['src'], $Matches['dst']))
            } else {
                throw "Malformed edge line $($i+1): $line"
            }
        }
        # Target nodes use an opaque ID and carry the CMake target name in the
        # label attribute. Other attributes and their order are presentation
        # details and may vary between CMake versions.
        elseif ($line -match '^"(?<id>[^"]+)"\s*\[(?<attributes>[^\]]*)\]\s*;?\s*$') {
            $id = $Matches['id']
            $attributes = $Matches['attributes']
            if ($attributes -notmatch '(?:^|,)\s*label\s*=\s*"(?<label>[^"]+)"\s*(?=,|$)') {
                throw "Target node lacks a valid label attribute at line $($i+1): $line"
            }
            $label = $Matches['label']
            if ($idToLabel.ContainsKey($id)) {
                throw "Duplicate node ID at line $($i+1): $id"
            }
            if ($labelToId.ContainsKey($label)) {
                throw "Duplicate node label at line $($i+1): $label"
            }
            $idToLabel[$id] = $label
            $labelToId[$label] = $id
        }
        elseif ($line.StartsWith('"')) {
            throw "Malformed target node line $($i+1): $line"
        }
        # Root graph syntax and default node attributes are structural only.
        elseif ($line -eq '{' -or $line -eq '}' -or $line -eq '];' -or
                $line -match '^(node|edge)\s*\[$' -or
                $line -match '^(fontsize|label|color)\s*=.*;?\s*$') {
            continue
        }
        else {
            throw "Unrecognized GraphViz line $($i+1): $line"
        }
    }

    if ($labelToId.Count -eq 0) {
        throw 'GraphViz graph contains no nodes'
    }

    $adjacency = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($edge in $edges) {
        $srcId = $edge[0]
        $dstId = $edge[1]
        if (-not $idToLabel.ContainsKey($srcId) -or
                -not $idToLabel.ContainsKey($dstId)) {
            throw "GraphViz edge references an undeclared node: $srcId -> $dstId"
        }

        $srcLabel = $idToLabel[$srcId]
        $dstLabel = $idToLabel[$dstId]
        if (-not $adjacency.ContainsKey($srcLabel)) {
            $adjacency[$srcLabel] = @()
        }
        if ($dstLabel -notin $adjacency[$srcLabel]) {
            $adjacency[$srcLabel] += $dstLabel
        }
    }

    return @{ Nodes = $labelToId; Adjacency = $adjacency }
}

function Get-TransitiveReach {
    param($startName, $adjacency, [hashtable]$visited)
    if ($visited.ContainsKey($startName)) { return }
    $visited[$startName] = $true
    if (-not $adjacency.ContainsKey($startName)) { return }
    foreach ($neighbor in $adjacency[$startName]) {
        Get-TransitiveReach $neighbor $adjacency $visited
    }
}

try {
    . (Join-Path $sourceRoot 'tools\Enter-MsvcEnvironment.ps1')

    $savedErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $configureArguments = @(
        '-S', $sourceRoot,
        '-B', $probeRoot,
        '-G', $Generator,
        '-DBUILD_TESTING:BOOL=OFF',
        "-DCMAKE_CXX_COMPILER:FILEPATH=$Compiler"
    )
    $configureOutput = @(& cmake.exe @configureArguments 2>&1)
    $configureExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorPreference
    $configureOutput | ForEach-Object { Write-Output $_ }

    if ($configureExitCode -ne 0) {
        throw "M4-boundary probe configure failed with exit code $configureExitCode"
    }

    # --- Generate CMake dependency graph via GraphViz ---
    $dotFile = Join-Path $probeRoot 'm4_deps.dot'
    $ErrorActionPreference = 'Continue'
    $graphVizOutput = @(& cmake.exe $probeRoot --graphviz="$dotFile" 2>&1)
    $graphVizExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorPreference
    $graphVizOutput | ForEach-Object { Write-Output $_ }

    if ($graphVizExitCode -ne 0) {
        throw "M4-boundary GraphViz generation failed with exit code $graphVizExitCode"
    }

    $graph = Parse-CMakeGraphViz -DotPath $dotFile
    $nodes = $graph.Nodes
    $adjacency = $graph.Adjacency

    Write-Output "GraphViz parsed: $($nodes.Count) nodes, $($adjacency.Count) nodes with outgoing edges."

    # --- Gate 1: every required target must exist in the graph ---
    $missingTargets = @()
    foreach ($name in $requiredTargets) {
        if (-not $nodes.ContainsKey($name)) {
            $missingTargets += $name
        }
    }
    if ($missingTargets.Count -gt 0) {
        throw "MISSING_M4_TARGETS: $($missingTargets -join ', ')"
    }
    Write-Output 'Gate 1 PASS: all seven M4 library targets present in GraphViz graph.'

    # --- Gate 2: executable output-name contract ---
    $expectedExeNames = $ExpectedExecutableNames -split ';' |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne '' }
    # Deliberate double entry: CMake supplies the implementation contract and
    # this independent list detects accidental renames or reordering.
    $canonicalExeNames = @(
        'myrd.exe'
        'MyRamDiskService.exe'
        'MyRamDiskHost.exe'
        'MyRamDiskDriverBootstrap.exe'
    )
    if ($expectedExeNames.Count -ne $canonicalExeNames.Count) {
        throw ("Executable-name count mismatch: expected $($canonicalExeNames.Count), " +
            "got $($expectedExeNames.Count): $($expectedExeNames -join ', ')")
    }
    for ($i = 0; $i -lt $canonicalExeNames.Count; $i++) {
        if ($expectedExeNames[$i] -ne $canonicalExeNames[$i]) {
            throw ("Executable-name mismatch at position ${i}: " +
                "expected '$($canonicalExeNames[${i}])', got '$($expectedExeNames[${i}])'")
        }
    }
    Write-Output 'Gate 2 PASS: executable output-name contract matches exact ordered names.'

    # --- Gate 3: forbidden dependency boundaries (transitive) ---
    foreach ($subject in $forbiddenBoundaries.Keys) {
        if (-not $nodes.ContainsKey($subject)) { continue }
        $visited = @{}
        Get-TransitiveReach $subject $adjacency $visited
        $forbidden = $forbiddenBoundaries[$subject]
        $violations = @($visited.Keys | Where-Object { $_ -in $forbidden })
        if ($violations.Count -gt 0) {
            throw "FORBIDDEN_DEPENDENCY: $subject transitively depends on $($violations -join ', ')"
        }
    }
    Write-Output 'Gate 3 PASS: no forbidden dependency boundaries violated.'

    # --- Gate 4: only myrd_host_core may transitively reach M3/AIM-facing libraries ---
    foreach ($name in $requiredTargets) {
        if ($name -eq 'myrd_host_core') { continue }
        if (-not $nodes.ContainsKey($name)) { continue }
        $visited = @{}
        Get-TransitiveReach $name $adjacency $visited
        $m3Violations = @($visited.Keys | Where-Object { $_ -in $m3Facing })
        if ($m3Violations.Count -gt 0) {
            throw "M3_BOUNDARY_VIOLATION: $name transitively reaches M3-facing $($m3Violations -join ', ')"
        }
    }
    Write-Output 'Gate 4 PASS: only myrd_host_core may reach M3-facing libraries.'

    Write-Output 'M4 build boundary probe PASS: all target and dependency gates satisfied.'
}
finally {
    if (Test-Path -LiteralPath $probeRoot) {
        $probeItem = Get-Item -LiteralPath $probeRoot -Force
        if (($probeItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to recursively remove reparse-point probe run: $probeRoot"
        }
        Remove-Item -LiteralPath $probeRoot -Recurse -Force
    }
}
