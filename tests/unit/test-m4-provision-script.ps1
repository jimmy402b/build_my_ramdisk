[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($SourceRoot)
$scriptPath = Join-Path $root 'src\run\provision_volume.ps1'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# This is deliberately a contract test.  The implementation is invoked only
# after every storage cmdlet has been replaced by a local fake function.
Require (Test-Path -LiteralPath $scriptPath) `
    "Missing provisioning script: $scriptPath"

$bytes = [IO.File]::ReadAllBytes($scriptPath)
Require (-not ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE)) `
    'provision_volume.ps1 must not be UTF-16LE encoded'
Require (-not ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF)) `
    'provision_volume.ps1 must not be UTF-16BE encoded'
Require (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) `
    'provision_volume.ps1 must not contain a UTF-8 BOM'
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($bytes[$i] -eq 10 -and ($i -eq 0 -or $bytes[$i - 1] -ne 13)) {
        throw 'provision_volume.ps1 must use deterministic CRLF line endings'
    }
}

$source = [Text.Encoding]::UTF8.GetString($bytes)
Require ($source -match '(?m)^param\s*\(') 'script must declare a param block'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $scriptPath, [ref]$tokens, [ref]$parseErrors)
Require ($parseErrors.Count -eq 0) `
    "provision_volume.ps1 has PowerShell parse errors: $($parseErrors.Message -join '; ')"
$paramAst = $ast.ParamBlock
Require ($null -ne $paramAst) 'script must declare a parameter block'
$paramNames = @($paramAst.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
Require ($paramNames.Count -eq 2) `
    "provisioning script must expose exactly two parameters; found: $($paramNames -join ', ')"
Require (($paramNames -ccontains 'DiskNumber') -and ($paramNames -ccontains 'DriveLetter')) `
    'provisioning parameters must be exactly DiskNumber and DriveLetter'
Require ($paramAst.Parameters[0].Name.VariablePath.UserPath -eq 'DiskNumber' -and
        $paramAst.Parameters[1].Name.VariablePath.UserPath -eq 'DriveLetter') `
    'provisioning parameters must be ordered DiskNumber, DriveLetter'
foreach ($parameter in $paramAst.Parameters) {
    $parameterAttribute = @($parameter.Attributes | Where-Object {
        $_.TypeName.Name -ieq 'Parameter'
    }) | Select-Object -First 1
    Require ($null -ne $parameterAttribute) `
        "parameter $($parameter.Name) must declare [Parameter(Mandatory=`$true)]"
    $mandatory = @($parameterAttribute.NamedArguments | Where-Object {
        $_.ArgumentName -ieq 'Mandatory'
    }) | Select-Object -First 1
    Require ($null -ne $mandatory -and $mandatory.Argument -is [Management.Automation.Language.ConstantExpressionAst] -and
        [bool]$mandatory.Argument.Value) `
        "parameter $($parameter.Name) must require Mandatory=`$true"
}
Require ($source -match '\[UInt32\]\s*\$DiskNumber') 'DiskNumber must be UInt32'
Require ($source -match '\[char\]\s*\$DriveLetter') 'DriveLetter must be char'
Require ($source -match '(?im)^\s*\$ErrorActionPreference\s*=\s*["'']Stop["'']') `
    'provisioning script must use terminating errors'
Require ($source -notmatch '(?im)(?:Set-Disk|Initialize-Disk|New-Partition|Format-Volume|Set-Partition|Set-Volume)[^\r\n]*-ErrorAction\s+SilentlyContinue') `
    'provisioning mutations must not suppress storage errors'
Require ($source -notmatch '(?m)^\s*\[Parameter\([^)]*Mandatory\s*=\s*\$false') `
    'provisioning inputs must not be optional'
foreach ($forbidden in @('DiskPart', 'vmrun', 'pnputil', 'Restart-Computer', 'shutdown.exe',
                         'Format-Disk', 'Reset-Disk')) {
    Require ($source -notmatch [regex]::Escape($forbidden)) `
        "script must not delegate to forbidden command: $forbidden"
}

$commands = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.CommandAst]
}, $true))
Require ($commands.Count -gt 0) 'provisioning script must contain executable commands'
$commandNames = @($commands | ForEach-Object { $_.GetCommandName() })
$allowedCommands = @(
    'Get-Disk', 'Get-Partition', 'Get-Volume', 'Get-PSDrive',
    'Set-Disk', 'Initialize-Disk', 'New-Partition', 'Format-Volume',
    'Set-Partition', 'Set-Volume', 'Write-Output', 'Write-Error',
    'ConvertTo-Json', 'Out-Null', 'Test-Path', 'Remove-Partition', 'Clear-Disk')
foreach ($command in $commands) {
    Require ($command.InvocationOperator -eq [Management.Automation.Language.TokenKind]::Unknown -or
        $command.InvocationOperator.ToString() -eq 'Unknown' -or
        $command.InvocationOperator.ToString() -eq 'None') `
        'dynamic/call-operator invocation is forbidden'
    Require ($command.CommandElements.Count -gt 0 -and
        $command.CommandElements[0] -is [Management.Automation.Language.StringConstantExpressionAst]) `
        'dynamic command names are forbidden in the provisioning script'
    $literalName = [string]$command.CommandElements[0].Value
    Require ($literalName -notmatch '\\' -and
        ($allowedCommands | Where-Object { $_ -ieq $literalName }).Count -eq 1) `
        "command is not in the safe provisioning allowlist: $literalName"
    if ($literalName -ieq 'Set-Disk') {
        $extent = $command.Extent.Text
        Require ($extent -match '(?i)-Number\s+\$DiskNumber') `
            'Set-Disk must target the requested DiskNumber'
        Require ($extent -match '(?i)-IsOffline\s+\$false|-IsReadOnly\s+\$false') `
            'Set-Disk must explicitly pass one safe state transition'
    }
}
$mutationNames = @('Set-Disk', 'Initialize-Disk', 'New-Partition', 'Format-Volume', 'Set-Partition', 'Set-Volume')
$readonlyNames = @('Get-Disk', 'Get-Partition', 'Get-Volume', 'Get-PSDrive')
$firstMutation = [int]::MaxValue
for ($i = 0; $i -lt $commandNames.Count; $i++) {
    if ($mutationNames | Where-Object { $_ -ieq $commandNames[$i] }) { $firstMutation = $i; break }
}
Require ($firstMutation -lt [int]::MaxValue) 'script lacks required provisioning mutation commands'
foreach ($readonly in $readonlyNames) {
    $index = -1
    for ($i = 0; $i -lt $commandNames.Count; $i++) {
        if ($commandNames[$i] -ieq $readonly) { $index = $i; break }
    }
    Require ($index -ge 0 -and $index -lt $firstMutation) `
        "$readonly preflight check must occur before any disk mutation"
}

function Invoke-FakeProvision {
    param(
        [UInt32]$DiskNumber = 7,
        [char]$DriveLetter = 'R',
        [ValidateSet('Valid', 'Filesystem', 'MultiplePartitions', 'OccupiedLetter', 'BootDisk', 'SystemDisk')]
        [string]$Scenario = 'Valid'
    )

    $global:fakeCalls = [Collections.Generic.List[string]]::new()
    $global:fakeContract = [pscustomobject]@{ GetDisk=$false; GetPartition=$false; GetVolume=$false; GetPSDrive=$false; SetDisk=$false; InitializeDisk=$false; NewPartition=$false; FormatVolume=$false; SetPartition=$false; SetVolume=$false }
    $global:fakeDriveLetter = $DriveLetter
    $global:fakeDisk = [pscustomobject]@{
        Number = $DiskNumber; PartitionStyle = 'RAW'; NumberOfPartitions = 0
        IsBoot = $false; IsSystem = $false; IsReadOnly = $false; IsOffline = $false
    }
    $global:fakePartitions = @()
    $global:fakeVolume = $null
    if ($Scenario -eq 'Filesystem') {
        $global:fakeDisk.PartitionStyle = 'GPT'
    } elseif ($Scenario -eq 'MultiplePartitions') {
        $global:fakePartitions = @([pscustomobject]@{ Number = 1 }, [pscustomobject]@{ Number = 2 })
        $global:fakeDisk.NumberOfPartitions = 2
    } elseif ($Scenario -eq 'OccupiedLetter') {
        $global:fakeVolume = [pscustomobject]@{ DriveLetter = $DriveLetter }
    } elseif ($Scenario -eq 'BootDisk') {
        $global:fakeDisk.IsBoot = $true
    } elseif ($Scenario -eq 'SystemDisk') {
        $global:fakeDisk.IsSystem = $true
    }

    function global:Get-Disk { param([UInt32]$Number) $global:fakeContract.GetDisk = ($Number -eq $DiskNumber); $global:fakeCalls.Add('Get-Disk'); $global:fakeDisk }
    function global:Get-Partition { param([UInt32]$DiskNumber, [char]$DriveLetter) $global:fakeContract.GetPartition = ($DiskNumber -eq 7); $global:fakeCalls.Add('Get-Partition'); $global:fakePartitions }
    function global:Get-Volume { param([char]$DriveLetter, $Partition) $global:fakeContract.GetVolume = $true; $global:fakeCalls.Add('Get-Volume'); if ($global:fakeVolume) { $global:fakeVolume } }
    function global:Get-PSDrive { param([char]$Name) $global:fakeContract.GetPSDrive = ($Name -ieq $global:fakeDriveLetter); $global:fakeCalls.Add('Get-PSDrive'); if ($global:fakeVolume) { [pscustomobject]@{ Name = $Name } } }
    function global:Set-Disk { param([UInt32]$Number, [bool]$IsOffline, [bool]$IsReadOnly, [Parameter(ValueFromRemainingArguments)]$Args) $global:fakeContract.SetDisk = ($Number -eq $DiskNumber -and $IsOffline -eq $false -and $IsReadOnly -eq $false); $global:fakeCalls.Add("Set-Disk:$Number") }
    function global:Initialize-Disk { param([UInt32]$Number, [string]$PartitionStyle, [Parameter(ValueFromRemainingArguments)]$Args) $global:fakeContract.InitializeDisk = ($Number -eq $DiskNumber -and $PartitionStyle -ceq 'GPT'); $global:fakeCalls.Add("Initialize-Disk:$PartitionStyle"); $global:fakeDisk.PartitionStyle = $PartitionStyle; $global:fakeDisk }
    function global:New-Partition { param([UInt32]$DiskNumber, [switch]$UseMaximumSize, [char]$DriveLetter, [Parameter(ValueFromRemainingArguments)]$Args) $global:fakeContract.NewPartition = ($DiskNumber -eq 7 -and $UseMaximumSize); $global:fakeCalls.Add('New-Partition:single'); [pscustomobject]@{ Number = 1; DriveLetter = $null } }
    function global:Format-Volume { param([object]$Partition, [string]$FileSystem, [Parameter(ValueFromRemainingArguments)]$Args) $global:fakeContract.FormatVolume = ($FileSystem -ceq 'NTFS'); $global:fakeCalls.Add("Format-Volume:$FileSystem") }
    function global:Set-Partition { param([UInt32]$DiskNumber, [UInt32]$PartitionNumber, [char]$NewDriveLetter, [char]$DriveLetter, [Parameter(ValueFromRemainingArguments)]$Args) $assigned = if ($PSBoundParameters.ContainsKey('NewDriveLetter')) { $NewDriveLetter } else { $DriveLetter }; $global:fakeContract.SetPartition = ($assigned -ieq $global:fakeDriveLetter); $global:fakeCalls.Add('Set-Partition:letter') }
    function global:Set-Volume { param([object]$InputObject, [char]$DriveLetter, [Parameter(ValueFromRemainingArguments)]$Args) $global:fakeContract.SetVolume = ($DriveLetter -ieq $global:fakeDriveLetter); $global:fakeCalls.Add('Set-Volume:letter') }

    try {
        $output = @(& $scriptPath -DiskNumber $DiskNumber -DriveLetter $DriveLetter 2>&1 | ForEach-Object { $_.ToString() })
        [pscustomobject]@{ Output = $output; Calls = @($global:fakeCalls); Contract = $global:fakeContract }
    } finally {
        foreach ($name in @('Get-Disk','Get-Partition','Get-Volume','Get-PSDrive','Set-Disk','Initialize-Disk','New-Partition','Format-Volume','Set-Partition','Set-Volume')) {
            Remove-Item -Path "Function:\global:$name" -ErrorAction SilentlyContinue
        }
    }
}

function Assert-Rejected([string]$Scenario, [string]$Message) {
    $result = $null
    $thrown = $null
    try { $result = Invoke-FakeProvision -Scenario $Scenario }
    catch { $thrown = $_ }
    $calls = if ($null -ne $result) { @($result.Calls) } else { @($global:fakeCalls) }
    $output = if ($null -ne $result) { @($result.Output) } else { @($thrown.ToString()) }
    Require ($output.Count -gt 0 -and ($output -join "`n") -match '(?i)(reject|refus|error|raw|partition|occup|boot|system)') $Message
    $mutations = @($calls | Where-Object { $_ -match '^(?i:Set-Disk|Initialize-Disk|New-Partition|Format-Volume|Set-(Partition|Volume))' })
    Require ($mutations.Count -eq 0) "$Message (must reject before any mutation; got: $($mutations -join ', '))"
}

$valid = Invoke-FakeProvision
Require ($valid.Output.Count -eq 1) 'valid provisioning must emit exactly one result line'
$mutationSequence = @($valid.Calls | Where-Object { $_ -match '^(?i:Set-Disk|Initialize-Disk|New-Partition|Format-Volume|Set-(Partition|Volume)):' })
Require ($mutationSequence.Count -ge 4 -and $mutationSequence.Count -le 6) `
    "valid provisioning must have four required mutations and at most two Set-Disk calls; got: $($mutationSequence -join ', ')"
Require (($mutationSequence -join '|') -match '^(?i:(Set-Disk:7\|){0,2}Initialize-Disk:GPT\|New-Partition:single\|Format-Volume:NTFS\|Set-(Partition|Volume):letter)$') `
    "provisioning mutation order/arguments are not frozen: $($mutationSequence -join ' -> ')"
Require $valid.Contract.InitializeDisk 'Initialize-Disk must target requested disk and GPT'
Require $valid.Contract.NewPartition 'New-Partition must target requested disk and full-size single partition'
Require $valid.Contract.FormatVolume 'Format-Volume must format NTFS'
Require ($valid.Contract.SetPartition -or $valid.Contract.SetVolume) 'drive-letter assignment must target requested letter'
Require $valid.Contract.GetDisk 'Get-Disk must inspect the requested disk'
Require $valid.Contract.GetPartition 'Get-Partition must inspect the requested disk'
Require $valid.Contract.GetPSDrive 'drive-letter occupancy check must inspect requested letter'
foreach ($call in @($mutationSequence | Where-Object { $_ -like 'Set-Disk:*' })) {
    Require ($call -ceq 'Set-Disk:7') 'Set-Disk must target the requested disk'
Require $valid.Contract.SetDisk 'Set-Disk must explicitly pass safe state transitions'
}
$json = $valid.Output[0] | ConvertFrom-Json
foreach ($field in @('schema', 'disk_number', 'drive_letter', 'partition_style', 'filesystem', 'partition_count')) {
    Require ($null -ne $json.$field) "result is missing fixed field: $field"
}
Require ($json.schema -eq 'myrd.m4.provisioning.result.v1') 'result schema is not frozen'
Require ([UInt32]$json.disk_number -eq 7) 'result disk_number is not deterministic'
Require ([string]$json.drive_letter -eq 'R') 'result drive_letter is not deterministic'
Require ([string]$json.partition_style -eq 'GPT') 'result partition_style must be GPT'
Require ([string]$json.filesystem -eq 'NTFS') 'result filesystem must be NTFS'
Require ([UInt32]$json.partition_count -eq 1) 'result partition_count must be one'

$again = Invoke-FakeProvision
Require (($valid.Output[0]) -ceq ($again.Output[0])) `
    'identical fake inputs must produce byte-identical output'

Assert-Rejected 'Filesystem' 'existing filesystem must be rejected'
Assert-Rejected 'MultiplePartitions' 'multiple partitions must be rejected'
Assert-Rejected 'OccupiedLetter' 'occupied drive letter must be rejected'
Assert-Rejected 'BootDisk' 'boot disk must be rejected'
Assert-Rejected 'SystemDisk' 'system disk must be rejected'

foreach ($bad in @(
        @{ Disk = [UInt32]::MaxValue; Letter = 'R' },
        @{ Disk = 7; Letter = '1' },
        @{ Disk = 7; Letter = ':' },
        @{ Disk = 7; Letter = '[' })) {
    $failed = $false
    $invalidResult = $null
    $global:fakeCalls = [Collections.Generic.List[string]]::new()
    try {
        $invalidResult = Invoke-FakeProvision -DiskNumber $bad.Disk -DriveLetter ([char]$bad.Letter)
    }
    catch { $failed = $true }
    $invalidCalls = if ($null -ne $invalidResult) { @($invalidResult.Calls) } else { @($global:fakeCalls) }
    $invalidMutations = @($invalidCalls | Where-Object { $_ -match '^(?i:Set-Disk|Initialize-Disk|New-Partition|Format-Volume|Set-(Partition|Volume))' })
    Require ($invalidMutations.Count -eq 0) "invalid input performed mutations: $($invalidMutations -join ', ')"
    Require $failed "invalid input was accepted: disk=$($bad.Disk), letter=$($bad.Letter)"
}

Write-Output 'M4 provisioning script contract RED/GREEN checks PASS'
