param(
    [Parameter(Mandatory = $true)][string]$ServiceExe,
    [Parameter(Mandatory = $true)][string]$HostExe,
    [ValidateSet('Model', 'Scm', 'Product', 'ProductCrashReattach')]
    [string]$Mode = 'Model',
    [string]$AimFixture = '',
    [string]$BundlePath = '',
    [string]$ProfileFixture = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-Json([string[]]$Arguments) {
    $output = & $ServiceExe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Service command failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
    return ($output | Out-String | ConvertFrom-Json)
}

function Wait-Until(
    [scriptblock]$Condition,
    [int]$TimeoutSeconds,
    [string]$FailureMessage) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $FailureMessage
}

if ($Mode -eq 'Model') {
    $contract = Invoke-Json @('--engineering-contract')
    Assert-True ($contract.service_name -eq 'MyRamDiskService') 'wrong service name'
    Assert-True ($contract.account -eq 'LocalSystem') 'wrong service account'
    Assert-True ($contract.start_type -eq 'automatic') 'service must use automatic start'
    Assert-True (-not $contract.delayed_auto_start) 'delayed start was not frozen'
    Assert-True ($contract.service_sid -eq 'unrestricted') 'service SID missing'
    Assert-True (-not $contract.kill_on_job_close) 'worker would die with the service job handle'
    Assert-True ($contract.failure_reset_seconds -eq 86400) 'wrong failure reset'
    Assert-True (($contract.restart_delays_ms -join ',') -eq '5000,15000,60000') 'wrong restart delays'
    Assert-True ($contract.preshutdown_timeout_ms -eq 120000) 'wrong preshutdown bound'
    Assert-True ($contract.suspend_timeout_ms -eq 10000) 'wrong suspend bound'

    $startup = Invoke-Json @('--engineering-startup-model')
    Assert-True (($startup.stages -join ',') -eq 'logging,singleton,worker_identity,driver,profiles,reattach,auto_start') 'startup order drifted'
    Assert-True (($startup.attached -join ',') -eq '11111111-1111-4111-8111-111111111111') 'transient reattachment did not recover on the bounded final attempt'
    Assert-True (($startup.started -join ',') -eq '33333333-3333-4333-8333-333333333333') 'reattached profile was launched again or healthy profiles were not serially isolated'
    Assert-True (($startup.failed -join ',') -eq '22222222-2222-4222-8222-222222222222,44444444-4444-4444-8444-444444444444') 'bad or stale-runtime profile isolation missing'
    Assert-True ($startup.transient_reattach_attempts -eq 3) 'transient reattachment did not retry exactly three times'
    Assert-True ($startup.permanent_reattach_attempts -eq 3) 'permanent reattachment failure was not bounded to three attempts'

    $startupFailure = Invoke-Json @('--engineering-startup-failure-model')
    Assert-True (($startupFailure.calls -join ',') -eq 'logging,singleton') 'fatal startup failure executed later stages'

    $shutdown = Invoke-Json @('--engineering-shutdown-model')
    Assert-True ($shutdown.disabled_attempts -eq 0) 'disabled policy attempted a checkpoint'
    Assert-True ($shutdown.best_effort_attempts -eq 2) 'best effort did not attempt bounded profiles'
    Assert-True ($shutdown.best_effort_confirmed -eq 1) 'timeout/failure was claimed as committed'
    Assert-True ($shutdown.late_checkpoint_confirmed -eq 0) 'checkpoint completing after deadline was claimed as committed'
    Assert-True ($shutdown.controlled_stop_attempts -eq 3) 'controlled stop did not visit every disk'
    Assert-True ($shutdown.controlled_stop_state -eq 'running') 'unsafe controlled stop did not return to Running'

    & $HostExe --profile '11111111-1111-4111-8111-111111111111' --engineering-once 2>$null
    Assert-True ($LASTEXITCODE -ne 0) 'worker accepted startup without inherited secret capability'

    $launch = Invoke-Json @(
        '--engineering-launch-worker',
        '--host', (Resolve-Path -LiteralPath $HostExe).Path,
        '--profile', '11111111-1111-4111-8111-111111111111',
        '--image', 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
        '--allow-unprotected-test-path')
    Assert-True $launch.started 'worker did not consume the staged startup record'
    Assert-True ($launch.exit_code -eq 0) 'engineering worker did not exit cleanly'
    Assert-True ($launch.command_line -eq 'MyRamDiskHost.exe --profile 11111111-1111-4111-8111-111111111111 --engineering-once') 'worker argv leaked or drifted'
    Assert-True ($launch.inherited_handle_count -eq 3) 'child handle allowlist drifted'
    Assert-True $launch.process_pin_verified 'live process HANDLE/creation-time pin failed'
    Assert-True ($launch.worker_token_mode -eq 'inherited_full_localsystem') `
        'worker did not inherit the frozen full-LocalSystem token mode'

    $authenticated = Invoke-Json @(
        '--engineering-authenticate-worker',
        '--host', (Resolve-Path -LiteralPath $HostExe).Path,
        '--profile', '44444444-4444-4444-8444-444444444444',
        '--image', 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb',
        '--allow-unprotected-test-path')
    Assert-True $authenticated.authenticated 'real private named-pipe authentication failed'
    Assert-True $authenticated.startup_status_returned `
        'authenticated startup did not return its already-verified status'
    Assert-True ($authenticated.exit_code -eq 0) 'authenticated worker did not exit cleanly'
    Assert-True ($authenticated.inherited_handle_count -eq 3) 'authenticated launch allowlist drifted'

    Write-Host 'm4 windows service model: PASS'
    exit 0
}

if ($env:MYRD_RUN_WINDOWS_SERVICE_E2E -ne '1') {
    Write-Host 'SKIP: set MYRD_RUN_WINDOWS_SERVICE_E2E=1 in the disposable Windows VM'
    exit 77
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
Assert-True ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'SCM test requires elevation'

if ($Mode -eq 'ProductCrashReattach') {
    $evidence = [ordered]@{}
    trap {
        $evidence.Failure = $_.Exception.Message
        [pscustomobject]@{ Ok = $false; Evidence = $evidence } |
            ConvertTo-Json -Depth 8
        exit 1
    }
    Assert-True (-not [string]::IsNullOrWhiteSpace($BundlePath)) `
        'ProductCrashReattach mode requires an explicit BundlePath'
    Assert-True (-not [string]::IsNullOrWhiteSpace($ProfileFixture)) `
        'ProductCrashReattach mode requires an explicit ProfileFixture'
    $serviceName = 'MyRamDiskService'
    $servicePath = (Resolve-Path -LiteralPath $ServiceExe).Path
    $hostPath = (Resolve-Path -LiteralPath $HostExe).Path
    $aimPath = (Resolve-Path -LiteralPath $AimFixture).Path
    $profileFixturePath = (Resolve-Path -LiteralPath $ProfileFixture).Path
    $bundleRoot = (Resolve-Path -LiteralPath $BundlePath).Path.TrimEnd('\', '/')
    Assert-True ((Get-Item -LiteralPath $bundleRoot).PSIsContainer) `
        'BundlePath must resolve to a directory'
    $profilesRoot = Join-Path $env:ProgramData 'MyRamDisk\profiles'

    function Get-PrefixSha256([string]$Path, [int64]$Length) {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, $share)
        $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
            [Security.Cryptography.HashAlgorithmName]::SHA256)
        try {
            $buffer = [byte[]]::new(1MB)
            [int64]$remaining = $Length
            while ($remaining -gt 0) {
                $requested = [int][Math]::Min([int64]$buffer.Length, $remaining)
                $read = $stream.Read($buffer, 0, $requested)
                if ($read -le 0) {
                    throw "Unexpected EOF hashing $Path; $remaining bytes remain"
                }
                $hash.AppendData($buffer, 0, $read)
                $remaining -= $read
            }
            return ([BitConverter]::ToString($hash.GetHashAndReset()) -replace '-', '')
        }
        finally {
            $hash.Dispose()
            $stream.Dispose()
        }
    }

    function Get-BundleInventory {
        @(Get-ChildItem -LiteralPath $bundleRoot -File -Recurse -Force |
            Sort-Object -Property FullName |
            ForEach-Object {
                [pscustomobject]@{
                    RelativePath = $_.FullName.Substring($bundleRoot.Length).TrimStart('\', '/')
                    Length = [int64]$_.Length
                    Sha256 = Get-PrefixSha256 -Path $_.FullName -Length ([int64]$_.Length)
                }
            })
    }

    function Get-ProductProcesses {
        @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -in @($servicePath, $hostPath)
        } | Select-Object ProcessId, Name, ExecutablePath, CreationDate)
    }

    function Get-AimLines {
        @(& $aimPath list 2>&1 | ForEach-Object { [string]$_ })
    }

    function Get-ProcessOwnerSid([uint32]$ProcessId) {
        $process = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId"
        $owner = Invoke-CimMethod -InputObject $process -MethodName GetOwnerSid
        Assert-True ($owner.ReturnValue -eq 0) 'Host process owner SID query failed'
        return $owner.Sid
    }

    function Invoke-ProfileInspect([string]$ProfileId) {
        $output = & $profileFixturePath inspect --program-data $env:ProgramData `
            --profile-id $ProfileId
        if ($LASTEXITCODE -ne 0) {
            throw "Profile fixture inspect failed ($LASTEXITCODE)"
        }
        return ($output | Out-String | ConvertFrom-Json)
    }

    try {
        $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        Assert-True ($service.State -eq 'Stopped') 'product service must begin stopped'
        Assert-True ($service.PathName -eq ('"' + $servicePath + '"')) 'unexpected product service binary'
        Assert-True (@(Get-ProductProcesses).Count -eq 0) 'product process exists before crash-reattach test'
        Assert-True (@(Get-AimLines | Where-Object { $_ -like "device`t*" }).Count -eq 0) 'AIM device exists before crash-reattach test'

        $profiles = @(Get-ChildItem -LiteralPath $profilesRoot -Directory -Force)
        Assert-True ($profiles.Count -eq 1) 'crash-reattach test requires exactly one disposable profile'
        $profileRoot = $profiles[0].FullName
        $profileId = $profiles[0].Name.ToLowerInvariant()
        $bindingBefore = Invoke-ProfileInspect -ProfileId $profileId
        Assert-True ($bindingBefore.valid -eq $true) 'profile binding inspect was not valid'
        Assert-True ($bindingBefore.profile_id -ceq $profileId) 'profile UUID does not match its directory'
        $boundRoot = (Resolve-Path -LiteralPath ([string]$bindingBefore.bundle_root)).Path.TrimEnd('\', '/')
        Assert-True ($boundRoot -ieq $bundleRoot) 'profile bundle_root does not match BundlePath'
        Assert-True ([string]$bindingBefore.manifest_name -ceq 'disk.rdm') 'profile manifest_name is not disk.rdm'
        $evidence.ProfileBindingBefore = $bindingBefore

        $bundleBefore = @(Get-BundleInventory)
        Assert-True ($bundleBefore.Count -gt 0) 'initial bundle inventory is empty'
        $manifestRelativePath = [string]$bindingBefore.manifest_name
        Assert-True (@($bundleBefore | Where-Object {
            $_.RelativePath -ceq $manifestRelativePath -and $_.Length -gt 0
        }).Count -eq 1) 'bound manifest is missing or empty'
        Assert-True (@($bundleBefore | Where-Object {
            [IO.Path]::GetExtension($_.RelativePath) -ieq '.vhdx' -and $_.Length -gt 0
        }).Count -ge 1) 'bundle has no non-empty .vhdx'
        Assert-True (@($bundleBefore | Where-Object {
            [IO.Path]::GetExtension($_.RelativePath) -ieq '.rdj' -and $_.Length -gt 0
        }).Count -ge 1) 'bundle has no non-empty .rdj'
        $evidence.BundleBefore = [ordered]@{
            Root = $bundleRoot
            FileCount = $bundleBefore.Count
            Files = $bundleBefore
        }
        $leasePath = Join-Path $profileRoot 'runtime.lease'
        $identityPath = Join-Path $profileRoot 'runtime.identity'

        Start-Service -Name $serviceName
        (Get-Service -Name $serviceName).WaitForStatus(
            'Running', [TimeSpan]::FromSeconds(90))
        Wait-Until {
            @(Get-ProductProcesses | Where-Object Name -eq 'MyRamDiskHost.exe').Count -eq 1
        } 90 'Host did not start'
        Wait-Until {
            @(Get-AimLines | Where-Object { $_ -like "device`t*" }).Count -eq 1
        } 90 'AIM device did not appear'
        Wait-Until {
            (Test-Path -LiteralPath $leasePath) -and
            (Test-Path -LiteralPath $identityPath)
        } 30 'runtime metadata did not appear'

        $serviceBefore = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        $hostBefore = @(Get-ProductProcesses | Where-Object Name -eq 'MyRamDiskHost.exe')
        $hostOwnerSidBefore = Get-ProcessOwnerSid `
            -ProcessId ([uint32]$hostBefore.ProcessId)
        Assert-True ($hostOwnerSidBefore -eq 'S-1-5-18') `
            'Host did not inherit the LocalSystem service identity'
        $leaseBefore = Get-Item -LiteralPath $leasePath
        $identityBefore = Get-Item -LiteralPath $identityPath
        $evidence.Before = [ordered]@{
            ServicePid = [uint32]$serviceBefore.ProcessId
            HostPid = [uint32]$hostBefore.ProcessId
            HostCreated = $hostBefore.CreationDate
            HostOwnerSid = $hostOwnerSidBefore
            LeaseWriteUtc = $leaseBefore.LastWriteTimeUtc
            LeaseLength = $leaseBefore.Length
            IdentityWriteUtc = $identityBefore.LastWriteTimeUtc
            IdentityLength = $identityBefore.Length
            Aim = Get-AimLines
        }

        Stop-Process -Id ([int]$serviceBefore.ProcessId) -Force
        Wait-Until {
            $current = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
            $current.State -eq 'Running' -and
            [uint32]$current.ProcessId -ne [uint32]$serviceBefore.ProcessId -and
            [uint32]$current.ProcessId -ne 0
        } 90 'SCM did not restart Service with a new PID'
        Wait-Until {
            (Test-Path -LiteralPath $leasePath) -and
            (Test-Path -LiteralPath $identityPath) -and
            ((Get-Item -LiteralPath $leasePath).LastWriteTimeUtc -gt $leaseBefore.LastWriteTimeUtc) -and
            ((Get-Item -LiteralPath $identityPath).LastWriteTimeUtc -gt $identityBefore.LastWriteTimeUtc)
        } 120 'reattachment did not rotate durable lease and refresh identity'

        $serviceAfter = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        $hostAfter = @(Get-ProductProcesses | Where-Object Name -eq 'MyRamDiskHost.exe')
        Assert-True ($hostAfter.Count -eq 1) 'SCM recovery lost or duplicated the surviving Host'
        Assert-True ([uint32]$hostAfter.ProcessId -eq [uint32]$hostBefore.ProcessId) 'SCM recovery replaced the surviving Host'
        Assert-True ($hostAfter.CreationDate -eq $hostBefore.CreationDate) 'SCM recovery changed Host process identity'
        Assert-True (@(Get-AimLines | Where-Object { $_ -like "device`t*" }).Count -eq 1) 'SCM recovery changed device cardinality'
        & sc.exe interrogate $serviceName | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'reattached Service control handler remained busy'
        $evidence.AfterReattach = [ordered]@{
            ServicePid = [uint32]$serviceAfter.ProcessId
            HostPid = [uint32]$hostAfter.ProcessId
            HostCreated = $hostAfter.CreationDate
            LeaseWriteUtc = (Get-Item -LiteralPath $leasePath).LastWriteTimeUtc
            LeaseLength = (Get-Item -LiteralPath $leasePath).Length
            IdentityWriteUtc = (Get-Item -LiteralPath $identityPath).LastWriteTimeUtc
            IdentityLength = (Get-Item -LiteralPath $identityPath).Length
            Aim = Get-AimLines
        }

        Stop-Service -Name $serviceName
        (Get-Service -Name $serviceName).WaitForStatus(
            'Stopped', [TimeSpan]::FromSeconds(150))
        Wait-Until {
            @(Get-ProductProcesses | Where-Object Name -eq 'MyRamDiskHost.exe').Count -eq 0
        } 30 'Host survived controlled stop'
        Wait-Until {
            @(Get-AimLines | Where-Object { $_ -like "device`t*" }).Count -eq 0
        } 30 'AIM device survived controlled stop'
        $remainingRuntime = @(Get-ChildItem -LiteralPath $profileRoot -Force |
            Where-Object { $_.Name -in @(
                'runtime.lease', 'runtime.identity', 'runtime.identity.prev',
                'worker.restart', 'worker.restart.prev') } |
            Select-Object -ExpandProperty Name)
        Assert-True ($remainingRuntime.Count -eq 0) 'runtime metadata survived controlled stop'
        $bundleRootPresent = Test-Path -LiteralPath $bundleRoot -PathType Container
        $bundleAfter = @()
        if ($bundleRootPresent) {
            $bundleAfter = @(Get-BundleInventory)
        }
        $bundleAfterByPath = [System.Collections.Generic.Dictionary[string,object]]::new(
            [StringComparer]::Ordinal)
        foreach ($file in $bundleAfter) {
            $bundleAfterByPath.Add([string]$file.RelativePath, $file)
        }
        $deletedBundleArtifacts = @($bundleBefore | Where-Object {
            -not $bundleAfterByPath.ContainsKey($_.RelativePath)
        } | Select-Object -ExpandProperty RelativePath)
        $truncatedBundleArtifacts = @($bundleBefore | Where-Object {
            $bundleAfterByPath.ContainsKey($_.RelativePath) -and
            [int64]$bundleAfterByPath[$_.RelativePath].Length -lt [int64]$_.Length
        } | ForEach-Object {
            [pscustomobject]@{
                RelativePath = $_.RelativePath
                BeforeLength = [int64]$_.Length
                AfterLength = [int64]$bundleAfterByPath[$_.RelativePath].Length
            }
        })
        $corruptedBundleArtifacts = @($bundleBefore | Where-Object {
            $_.RelativePath -cne $manifestRelativePath -and
            $bundleAfterByPath.ContainsKey($_.RelativePath) -and
            [int64]$bundleAfterByPath[$_.RelativePath].Length -ge [int64]$_.Length
        } | ForEach-Object {
            $afterPath = Join-Path $bundleRoot $_.RelativePath
            $prefixHash = Get-PrefixSha256 -Path $afterPath -Length ([int64]$_.Length)
            if ($prefixHash -cne $_.Sha256) {
                [pscustomobject]@{
                    RelativePath = $_.RelativePath
                    BeforeLength = [int64]$_.Length
                    BeforeSha256 = $_.Sha256
                    AfterPrefixSha256 = $prefixHash
                }
            }
        })
        $evidence.BundleAfter = [ordered]@{
            Root = $bundleRoot
            RootPresent = $bundleRootPresent
            FileCount = $bundleAfter.Count
            Files = $bundleAfter
        }
        $evidence.DeletedPreExistingArtifacts = $deletedBundleArtifacts
        $evidence.TruncatedPreExistingArtifacts = $truncatedBundleArtifacts
        $evidence.CorruptedPreExistingArtifacts = $corruptedBundleArtifacts
        $bindingAfter = Invoke-ProfileInspect -ProfileId $profileId
        Assert-True ($bindingAfter.valid -eq $true) 'final profile binding inspect was not valid'
        Assert-True ($bindingAfter.profile_id -ceq $profileId) 'final profile UUID changed'
        $boundRootAfter = (Resolve-Path -LiteralPath ([string]$bindingAfter.bundle_root)).Path.TrimEnd('\', '/')
        Assert-True ($boundRootAfter -ieq $bundleRoot) 'final profile bundle_root changed'
        Assert-True ([string]$bindingAfter.manifest_name -ceq $manifestRelativePath) 'final manifest binding changed'
        Assert-True ([string]$bindingAfter.image_id -ceq [string]$bindingBefore.image_id) 'final image binding changed'
        $evidence.ProfileBindingAfter = $bindingAfter
        $evidence.Final = [ordered]@{
            ServiceState = (Get-Service -Name $serviceName).Status.ToString()
            Processes = Get-ProductProcesses
            Aim = Get-AimLines
            RemainingRuntime = $remainingRuntime
            ProfilePresent = Test-Path -LiteralPath (Join-Path $profileRoot 'profile.myrd')
            Bundle = [ordered]@{
                RootPresent = $bundleRootPresent
                FileCount = $bundleAfter.Count
                Files = $bundleAfter
                DeletedPreExistingArtifacts = $deletedBundleArtifacts
                TruncatedPreExistingArtifacts = $truncatedBundleArtifacts
                CorruptedPreExistingArtifacts = $corruptedBundleArtifacts
            }
        }
        Assert-True $bundleRootPresent `
            'image bundle root was deleted during crash reattachment or controlled stop'
        Assert-True ($deletedBundleArtifacts.Count -eq 0) `
            'pre-existing image bundle artifact was deleted'
        Assert-True ($truncatedBundleArtifacts.Count -eq 0) `
            'pre-existing image bundle artifact was truncated'
        Assert-True ($corruptedBundleArtifacts.Count -eq 0) `
            'pre-existing non-manifest image bundle artifact bytes changed'
        [pscustomobject]@{ Ok = $true; Evidence = $evidence } |
            ConvertTo-Json -Depth 8
        exit 0
    }
    catch {
        $evidence.Failure = $_.Exception.Message
        $evidence.CurrentService = Get-CimInstance Win32_Service `
            -Filter "Name='$serviceName'" |
            Select-Object Name, State, ProcessId, PathName
        $evidence.CurrentProcesses = Get-ProductProcesses
        $evidence.CurrentAim = Get-AimLines
        [pscustomobject]@{ Ok = $false; Evidence = $evidence } |
            ConvertTo-Json -Depth 8
        exit 1
    }
}

if ($Mode -eq 'Product') {
    $serviceName = 'MyRamDiskService'
    $existingService = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    Assert-True ($null -eq $existingService) 'product service already exists; refusing to replace it'

    $profilesRoot = Join-Path $env:ProgramData 'MyRamDisk\profiles'
    if (Test-Path -LiteralPath $profilesRoot) {
        $profiles = @(Get-ChildItem -LiteralPath $profilesRoot -Force)
        Assert-True ($profiles.Count -eq 0) 'product probe requires an empty profile directory'
    }

    $installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) `
        ('MyRamDisk-Task23-' + [Guid]::NewGuid().ToString('N'))
    $stagedService = Join-Path $installRoot 'MyRamDiskService.exe'
    $stagedHost = Join-Path $installRoot 'MyRamDiskHost.exe'
    try {
        New-Item -ItemType Directory -Path $installRoot | Out-Null
        Copy-Item -LiteralPath (Resolve-Path -LiteralPath $ServiceExe).Path `
            -Destination $stagedService
        Copy-Item -LiteralPath (Resolve-Path -LiteralPath $HostExe).Path `
            -Destination $stagedHost
        $binaryPath = '"' + $stagedService + '"'
        New-Service -Name $serviceName -BinaryPathName $binaryPath `
            -StartupType Automatic | Out-Null
        & sc.exe sidtype $serviceName unrestricted | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'product service SID configuration failed'

        Start-Service -Name $serviceName
        (Get-Service -Name $serviceName).WaitForStatus(
            'Running', [TimeSpan]::FromSeconds(30))
        Assert-True ((Get-Service -Name $serviceName).Status -eq 'Running') `
            'product service with no profiles did not reach Running'

        Stop-Service -Name $serviceName
        (Get-Service -Name $serviceName).WaitForStatus(
            'Stopped', [TimeSpan]::FromSeconds(30))
        Assert-True ((Get-Service -Name $serviceName).Status -eq 'Stopped') `
            'product service with no profiles did not stop cleanly'
        Write-Host 'm4 windows product service empty-profile SCM: PASS'
    }
    finally {
        $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
        if ($null -ne $service) {
            if ($service.Status -ne 'Stopped') {
                Stop-Service -Name $serviceName -Force -ErrorAction SilentlyContinue
            }
            & sc.exe delete $serviceName | Out-Null
        }
        Remove-Item -LiteralPath $stagedService -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stagedHost -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $installRoot -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

$serviceName = 'MyRamDiskTest-' + [Guid]::NewGuid().ToString('N')
try {
    $installed = Invoke-Json @('--engineering-install-service', '--service-name', $serviceName)
    Assert-True $installed.installed 'service installation failed'

    $query = Invoke-Json @('--engineering-query-service', '--service-name', $serviceName)
    Assert-True ($query.account -eq 'LocalSystem') 'installed account drifted'
    Assert-True ($query.start_type -eq 'automatic') 'installed start type drifted'
    Assert-True (-not $query.delayed_auto_start) 'installed delayed-auto flag drifted'
    Assert-True ($query.service_sid -eq 'unrestricted') 'installed service SID drifted'
    Assert-True ($query.failure_reset_seconds -eq 86400) 'installed reset drifted'
    Assert-True $query.failure_actions_on_non_crash 'non-crash failure actions flag drifted'
    Assert-True (($query.restart_delays_ms -join ',') -eq '5000,15000,60000') 'installed failure actions drifted'

    Start-Service -Name $serviceName
    (Get-Service -Name $serviceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
    Assert-True ((Get-Service -Name $serviceName).Status -eq 'Running') 'service did not reach Running'
    $running = Invoke-Json @('--engineering-query-service', '--service-name', $serviceName)
    Assert-True ($running.current_state -eq 'running') 'SCM status is not Running'
    Assert-True $running.accept_stop 'service does not accept controlled stop'
    Assert-True $running.accept_preshutdown 'service does not accept preshutdown'
    Assert-True $running.accept_power_event 'service does not accept suspend notifications'

    & $ServiceExe --engineering-console --once
    Assert-True ($LASTEXITCODE -ne 0) 'service singleton allowed a second manager'

    Stop-Service -Name $serviceName
    (Get-Service -Name $serviceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
    Assert-True ((Get-Service -Name $serviceName).Status -eq 'Stopped') 'service did not stop safely'
    & $ServiceExe --engineering-console --once
    Assert-True ($LASTEXITCODE -eq 0) 'service singleton was not released after safe stop'
    Write-Host 'm4 windows service SCM: PASS'
}
finally {
    & $ServiceExe --engineering-delete-service --service-name $serviceName | Out-Null
}
