# RecoverShutdown Empty Audit Array Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve a zero-descendant recovery audit as the JSON array `children: []`, then commit and freeze a replacement qualification kit for a new manual VM run.

**Architecture:** Keep the strict consumer contract unchanged and correct the lifecycle runner at the producer boundary. `Assert-AuditDescendants` will always emit an array object, including when there are zero descendants; the runner's production JSON path and real native self-test will prove the exact serialized shape.

**Tech Stack:** Windows PowerShell 5.1, PowerShell `ConvertTo-Json`, CMake/CTest MSVC presets, Git, existing qualification-kit packager.

---

## File map

- Modify: `tests/e2e/Invoke-M4RunLifecycle.ps1` — add the zero-descendant regression and make the audit return type stable.
- Verify only: `tools/Invoke-M4GuestQualification.ps1` — retain the strict `[Array]` evidence gate without changes.
- Verify only: `tools/New-M4GuestQualificationKit.ps1` — freeze the corrected runner with the existing exact-nine-file packager.
- Output only: `out/qualified/<new-commit>/guest-qualification-kit/` — replacement immutable kit; this directory must not exist before packaging.

### Task 1: Reproduce the empty-audit serialization defect

**Files:**
- Modify: `tests/e2e/Invoke-M4RunLifecycle.ps1:154-194`
- Test: `tests/e2e/Invoke-M4RunLifecycle.ps1` (`Native-SelfTest`)

- [ ] **Step 1: Confirm the worktree starts clean and at the approved design commit**

Run:

```powershell
git status --short
git rev-parse HEAD^
git log -1 --format=%s
```

Expected: no status output; the parent commit is exactly `39b03062e6e3e8ab10f136e109187535be7a12fc`, and the HEAD subject is `docs(m4): plan empty recovery audit fix`.

- [ ] **Step 2: Add the failing production-path regression**

Immediately after the existing reverse-order descendant assertion in `Native-SelfTest`, add:

```powershell
$emptyDescendants=Assert-AuditDescendants @() 10
Need($emptyDescendants-is[Array]-and@($emptyDescendants).Count-eq0)'audit zero descendants was not preserved as an array'
Need((Json ([ordered]@{children=$emptyDescendants}))-ceq'{"children":[]}')'audit zero descendants did not serialize as a JSON array'
```

This calls the production audit function and the production JSON function. Do not wrap the call in `@(...)`, because that would hide the defect at the caller instead of testing the function contract.

- [ ] **Step 3: Run the direct native self-test and verify RED**

Run:

```powershell
$env:MYRD_M4_HARNESS_SELFTEST='1'
try {
    & .\tests\e2e\Invoke-M4RunLifecycle.ps1 `
        -Phase CreateKill `
        -Executable 'C:\R7-NoTouch\myrd.exe' `
        -Bundle 'C:\R7-NoTouch\bundle' `
        -DriveLetter R `
        -StateFile 'C:\R7-NoTouch\state.json' `
        -SafetyAcknowledged
    $exit=$LASTEXITCODE
} finally {
    Remove-Item Env:MYRD_M4_HARNESS_SELFTEST -ErrorAction SilentlyContinue
}
if($exit-ne77){throw "expected RED exit 77, got $exit"}
```

Expected: one blocked line containing `audit zero descendants was not preserved as an array`, and exit code 77. A syntax error, unrelated failure, or PASS is not an acceptable RED.

### Task 2: Make the producer return a stable array

**Files:**
- Modify: `tests/e2e/Invoke-M4RunLifecycle.ps1:134`
- Test: `tests/e2e/Invoke-M4RunLifecycle.ps1` (`Native-SelfTest`)

- [ ] **Step 1: Apply the minimal production fix**

Change only the final return expression of `Assert-AuditDescendants` from:

```powershell
$desc
```

to:

```powershell
,@($desc)
```

The unary comma emits the array as one pipeline object, preventing PowerShell 5.1 from collapsing the empty result to `$null`. Leave all identity, command-line and prohibited-descendant checks unchanged.

- [ ] **Step 2: Re-run the direct native self-test and verify GREEN**

Run the same command as Task 1, Step 3, but require exit 0:

```powershell
$env:MYRD_M4_HARNESS_SELFTEST='1'
try {
    $output=@(& .\tests\e2e\Invoke-M4RunLifecycle.ps1 `
        -Phase CreateKill `
        -Executable 'C:\R7-NoTouch\myrd.exe' `
        -Bundle 'C:\R7-NoTouch\bundle' `
        -DriveLetter R `
        -StateFile 'C:\R7-NoTouch\state.json' `
        -SafetyAcknowledged)
    $exit=$LASTEXITCODE
} finally {
    Remove-Item Env:MYRD_M4_HARNESS_SELFTEST -ErrorAction SilentlyContinue
}
if($exit-ne0-or$output.Count-ne1-or$output[0]-cne'M4 native harness selftest PASS'){
    throw "native GREEN mismatch: exit=$exit output=$($output-join'|')"
}
```

Expected: exact single line `M4 native harness selftest PASS` and exit 0.

- [ ] **Step 3: Parse the changed script and run focused tests**

Run:

```powershell
$tokens=$null;$errors=$null
[void][Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path .\tests\e2e\Invoke-M4RunLifecycle.ps1),
    [ref]$tokens,
    [ref]$errors)
if($errors.Count){throw ($errors|Out-String)}

ctest --preset debug -R '^unit\.m4\.lifecycle_native_selftest$' --output-on-failure
ctest --preset release -R '^unit\.m4\.lifecycle_native_selftest$' --output-on-failure
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\unit\test-m4-qualification-kit.ps1 -SourceRoot .
```

Expected: AST has zero errors; Debug and Release focused CTest each pass 1/1; both direct contracts print PASS and exit 0.

- [ ] **Step 4: Inspect the exact diff**

Run:

```powershell
git diff --check
git diff -- tests/e2e/Invoke-M4RunLifecycle.ps1
git status --short
```

Expected: no whitespace errors and exactly one modified production file containing only the two regression assertions and the stable-array return change.

- [ ] **Step 5: Commit the tested source fix**

Run:

```powershell
git add -- tests/e2e/Invoke-M4RunLifecycle.ps1
git diff --cached --check
git commit -m "fix(m4): preserve empty recovery audit arrays"
git status --short
```

Expected: commit succeeds and the worktree is clean.

### Task 3: Run release-grade local verification

**Files:**
- Verify only: `CMakePresets.json`
- Verify only: registered `m4-v3` tests

- [ ] **Step 1: Configure and build with the MSVC presets**

Run from a VS2022 Build Tools developer environment:

```powershell
cmake --preset msvc
cmake --build --preset debug --parallel 1
cmake --build --preset release --parallel 1
```

Expected: configure and both builds exit 0 with no compiler or generator errors.

- [ ] **Step 2: Run the complete Debug and Release m4-v3 suites**

Run:

```powershell
ctest --preset debug -L m4-v3 --output-on-failure
ctest --preset release -L m4-v3 --output-on-failure
```

Expected: both configurations report 100% pass. Any failure must be investigated; do not package after a failing suite.

- [ ] **Step 3: Reconfirm repository integrity**

Run:

```powershell
git diff --check
git status --short
git rev-parse HEAD
```

Expected: clean status and the committed source-fix HEAD.

### Task 4: Freeze and verify the replacement qualification kit

**Files:**
- Read: `tools/New-M4GuestQualificationKit.ps1`
- Read: `out/build/msvc/src/Release/myrd.exe`
- Read: `out/build/msvc/tests/Release/myrd_m4_aim_query.exe`
- Create: `out/qualified/<new-commit>/guest-qualification-kit/`

- [ ] **Step 1: Derive a new, absent output path and prove inputs exist**

Run:

```powershell
$sourceRoot=(Resolve-Path .).Path
$commit=(git rev-parse HEAD).Trim().ToLowerInvariant()
$artifact=Join-Path $sourceRoot 'out\build\msvc\src\Release\myrd.exe'
$query=Join-Path $sourceRoot 'out\build\msvc\tests\Release\myrd_m4_aim_query.exe'
$kit=Join-Path $sourceRoot "out\qualified\$commit\guest-qualification-kit"
if(-not(Test-Path -LiteralPath $artifact -PathType Leaf)){throw 'Release myrd.exe is absent'}
if(-not(Test-Path -LiteralPath $query -PathType Leaf)){throw 'Release query helper is absent'}
if(Test-Path -LiteralPath $kit){throw "refusing to overwrite kit: $kit"}
[IO.Directory]::CreateDirectory((Split-Path $kit -Parent))|Out-Null
```

Expected: both inputs exist and the destination is absent.

- [ ] **Step 2: Create the exact-nine-file kit**

Run:

```powershell
$packagerOutput=@(& .\tools\New-M4GuestQualificationKit.ps1 `
    -SourceRoot $sourceRoot `
    -Artifact $artifact `
    -Query $query `
    -OutputDirectory $kit)
if($packagerOutput.Count-ne1-or[IO.Path]::GetFullPath([string]$packagerOutput[0])-cne[IO.Path]::GetFullPath($kit)){
    throw "packager output mismatch: $($packagerOutput-join'|')"
}
```

Expected: the packager prints the new absolute kit path and does not overwrite any existing directory.

- [ ] **Step 3: Strictly verify manifest identity, inventory and hashes**

Run:

```powershell
$all=@(Get-ChildItem -LiteralPath $kit -File -Force)
if($all.Count-ne9){throw "kit file count is $($all.Count), expected 9"}
$manifestPath=Join-Path $kit 'qualification-kit.json'
$bytes=[IO.File]::ReadAllBytes($manifestPath)
if($bytes.Length-ge3-and$bytes[0]-eq0xEF-and$bytes[1]-eq0xBB-and$bytes[2]-eq0xBF){throw 'manifest has UTF-8 BOM'}
$utf8=[Text.UTF8Encoding]::new($false,$true)
$manifest=$utf8.GetString($bytes)|ConvertFrom-Json
if($manifest.schema-cne'myrd.m4.qualification-kit.v1'){throw 'manifest schema mismatch'}
if($manifest.source_commit-cne$commit){throw 'manifest commit mismatch'}
if(@($manifest.files).Count-ne8){throw 'manifest entry count mismatch'}
foreach($entry in @($manifest.files)){
    $path=Join-Path $kit ([string]$entry.name)
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "missing kit file: $($entry.name)"}
    $file=[IO.FileInfo]$path
    if($file.Length-ne[long]$entry.length){throw "length mismatch: $($entry.name)"}
    $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
    if($hash-cne[string]$entry.sha256){throw "hash mismatch: $($entry.name)"}
}
$runnerHash=(Get-FileHash -LiteralPath (Join-Path $kit 'Invoke-M4RunLifecycle.ps1') -Algorithm SHA256).Hash
$sourceRunnerHash=(Get-FileHash -LiteralPath .\tests\e2e\Invoke-M4RunLifecycle.ps1 -Algorithm SHA256).Hash
if($runnerHash-cne$sourceRunnerHash){throw 'frozen runner differs from source'}
```

Expected: all checks complete without output or exception; the manifest binds the new commit and all eight payload files exactly.

- [ ] **Step 4: Record the handoff facts without touching the VM**

Run:

```powershell
[ordered]@{
    commit=$commit
    kit=$kit
    manifest_sha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    runner_sha256=$runnerHash
    artifact_sha256=(Get-FileHash -LiteralPath (Join-Path $kit 'myrd.exe') -Algorithm SHA256).Hash
}|ConvertTo-Json
```

Expected: a concise handoff record. Stop here; do not copy files to the VM and do not invoke the controller.

### Task 5: Manual VM restart instructions

**Files:**
- Preserve: `E:\myrd-v1-qualification-20260817-a\` — immutable blocked evidence.
- User replaces: `E:\guest-qualification-kit\` — all nine files from the newly frozen kit.
- User creates through the controller: `E:\myrd-v1-qualification-20260817-b\`.

- [ ] **Step 1: Tell the user to replace the entire guest kit**

Require replacing all nine files together, not only `Invoke-M4RunLifecycle.ps1`. The new `qualification-kit.json` must accompany the corrected runner because the controller verifies every kit hash.

- [ ] **Step 2: Tell the user to use a new qualification root**

From an elevated PowerShell window in `E:\guest-qualification-kit`, run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Invoke-M4GuestQualification.ps1 `
  -QualificationRoot E:\myrd-v1-qualification-20260817-b
```

Expected first terminal prompt: `CRASH RESET ARMED`. At that prompt, the user performs exactly one VMware UI **Reset**, waits for Windows to start, and runs the identical command again with the same `-b` root.

- [ ] **Step 3: Preserve fail-closed behavior**

If any run prints `BLOCKED`, stop immediately. Do not retry that root, delete evidence, alter the driver/device, or create another root until the new evidence is read and diagnosed.
