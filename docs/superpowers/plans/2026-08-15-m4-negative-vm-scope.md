# M4 Negative VM Scope Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep volume-ambiguity refusal covered locally while making the real-VM Negative route execute exactly eight realistic, non-destructive cases and produce Final-verifiable evidence.

**Architecture:** Production discovery gains a same-translation-unit pure classifier for zero, one or multiple exact volume matches; the existing fail-closed behavior remains unchanged and unit/composition tests bind the classifier to the production result check. The PowerShell runner and independent verifier share the same frozen ordered eight-case VM contract. Real VM evidence uses the existing resumable mechanism for the driver-stopped case, then executes the other seven cases after restoring the exact driver and clean baseline.

**Tech Stack:** C++20, Win32 volume APIs, PowerShell 5.1, CMake/CTest, VMware Workstation `vmrun`, OpenSSH, AIM `phdskmnt` 1.2.28.93.

---

### Task 1: Freeze the local zero/one/many volume defense

**Files:**
- Modify: `src/run/windows_volume.cpp:70-82,235-242`
- Test: `tests/unit/m4_windows_volume_tests.cpp:95-125`
- Test: `tests/integration/m4_product_composition_tests.cpp`

- [ ] **Step 1: Write the failing same-TU unit test**

Add this test before changing production code:

```cpp
TEST_CASE(m4_volume_match_count_accepts_exactly_one) {
    REQUIRE_EQ(myrd::classify_volume_matches(0),
               myrd::VolumeMatchClass::Missing);
    REQUIRE_EQ(myrd::classify_volume_matches(1),
               myrd::VolumeMatchClass::Exact);
    REQUIRE_EQ(myrd::classify_volume_matches(2),
               myrd::VolumeMatchClass::Ambiguous);
    REQUIRE_EQ(myrd::classify_volume_matches(64),
               myrd::VolumeMatchClass::Ambiguous);
}
```

- [ ] **Step 2: Run the focused test and observe RED**

Run from the VS 2022 BuildTools developer environment:

```powershell
cmake --build out\build\msvc --config Debug --target myrd_m4_windows_volume_tests
out\build\msvc\tests\Debug\myrd_m4_windows_volume_tests.exe
```

Expected: build fails because `VolumeMatchClass` and
`classify_volume_matches` do not exist. A quoting/setup failure is not RED.

- [ ] **Step 3: Add the minimal classifier and bind production to it**

Add beside the other private pure predicates:

```cpp
enum class VolumeMatchClass { Missing, Exact, Ambiguous };

VolumeMatchClass classify_volume_matches(std::size_t count) noexcept {
    if (count == 0) return VolumeMatchClass::Missing;
    if (count == 1) return VolumeMatchClass::Exact;
    return VolumeMatchClass::Ambiguous;
}
```

Replace only the existing count condition:

```cpp
if (classify_volume_matches(matches.size()) != VolumeMatchClass::Exact)
    return Result<ProductVolume>::failure(
        fail(L"find_recovered_volume", L"volume identity is ambiguous"));
```

Do not change enumeration, target-letter fast path, error text, candidate
collection, mixed-extent refusal or `matches[0]` handling.

- [ ] **Step 4: Bind the production call site in composition coverage**

Add a source-contract test that opens `src/run/windows_volume.cpp`, locates
`Result<ProductVolume> discover`, and requires this ordering:

```cpp
const auto begin = text.find("Result<ProductVolume> discover(");
const auto end = text.find("Result<void> rollback_exact_mount", begin);
REQUIRE(begin != std::string::npos);
REQUIRE(end != std::string::npos);
const auto body = text.substr(begin, end - begin);
const auto enumeration_end = body.find(
    "if (enumeration_error != ERROR_NO_MORE_FILES)");
const auto classify = body.find("classify_volume_matches(matches.size())");
const auto consume = body.find("matches[0]");
REQUIRE(enumeration_end != std::string::npos);
REQUIRE(classify != std::string::npos);
REQUIRE(consume != std::string::npos);
REQUIRE(enumeration_end < classify);
REQUIRE(classify < consume);
```

The test must scope `body` to `discover`; it must not match another function.

- [ ] **Step 5: Run GREEN verification for Task 1**

```powershell
cmake --build out\build\msvc --config Debug --target `
  myrd myrd_m4_windows_volume_tests myrd_m4_product_composition_tests
out\build\msvc\tests\Debug\myrd_m4_windows_volume_tests.exe
ctest --preset debug -R '^(unit\.m4\.windows_volume|integration\.m4\.product_composition)$' --output-on-failure
```

Expected: Windows-volume executable reports every case PASS and CTest is 2/2.

---

### Task 2: Reduce the VM runner contract to eight realistic cases

**Files:**
- Modify: `tests/e2e/Invoke-M4RunLifecycle.ps1:218-253,271-313`
- Test: `tests/CMakeLists.txt` existing `unit.m4.lifecycle_negative_contract`
- Test: `tests/integration/m4_product_composition_tests.cpp`

- [ ] **Step 1: Write RED assertions against the current nine-case contract**

Change `Negative-ContractSelfTest` expectations before changing the arrays:

```powershell
Need(@($NegativeContracts.Keys).Count-eq8)'negative contract case count'
Need($NegativeCases-notcontains'ambiguous_recovery)'VM negative contract retained synthetic ambiguity'
```

Extend the composition source contract to require the exact ordered literal:

```cpp
REQUIRE(text.find(
    "$NegativeCases=@('unknown_nonempty','size_mismatch','occupied_letter',"
    "'gen0','writer_lock','missing_driver','busy_volume','compact_lock')")
    != std::string::npos);
REQUIRE(text.find("'ambiguous_recovery' {") == std::string::npos);
```

- [ ] **Step 2: Run the production negative selftest and observe RED**

```powershell
$env:MYRD_M4_NEGATIVE_SELFTEST='1'
try {
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tests\e2e\Invoke-M4RunLifecycle.ps1 -Phase Negative `
    -Executable C:\missing\Release\myrd.exe -Bundle C:\missing\bundle `
    -DriveLetter R -StateFile C:\missing\state.json -SafetyAcknowledged
} finally { Remove-Item Env:MYRD_M4_NEGATIVE_SELFTEST -ErrorAction SilentlyContinue }
```

Expected: exit 77 with `negative contract case count` or the explicit retained
ambiguity message. The selftest exits before path/VM access.

- [ ] **Step 3: Remove only the VM ambiguity case**

Set the exact ordered list:

```powershell
$NegativeCases=@('unknown_nonempty','size_mismatch','occupied_letter','gen0','writer_lock','missing_driver','busy_volume','compact_lock')
```

Delete the `ambiguous_recovery` entry from `$NegativeContracts`, its special
validator block and its `switch` execution block. Replace the old detailed
hostile fixture object in `Negative-ContractSelfTest` with this probe:

```powershell
[pscustomobject]@{case='ambiguous_recovery';entry=$null}
```

It must now be rejected as an unknown VM case before inspecting its entry.

- [ ] **Step 4: Run the focused negative selftest GREEN**

Run the Step 2 command again.

Expected: exit 0 with exact `M4 negative contract selftest PASS`; no product,
VM, path or service is accessed.

---

### Task 3: Make Final require exactly the same eight cases

**Files:**
- Modify: `tools/Invoke-M4Verification.ps1:13-48,59-78`
- Test: `tests/CMakeLists.txt` existing `unit.m4.lifecycle_final_verifier`

- [ ] **Step 1: Write a synthetic Final RED for the eight-case contract**

In `SelfTest`, set `negative_cases` to the approved eight names and add this
assertion after synthetic per-case files are created:

```powershell
Need(-not(Test-Path (Join-Path $d 'm4-negative-ambiguous_recovery.v1.json'))) `
  'synthetic VM evidence retained ambiguous_recovery'
```

Do not change `Verify-Final` yet.

- [ ] **Step 2: Run Final selftest and observe RED**

```powershell
$env:MYRD_M4_FINAL_SELFTEST='1'
try {
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\Invoke-M4Verification.ps1 -Tier Final
} finally { Remove-Item Env:MYRD_M4_FINAL_SELFTEST -ErrorAction SilentlyContinue }
```

Expected: nonzero because `Verify-Final` still requires missing ambiguity
evidence or a nine-case aggregate.

- [ ] **Step 3: Narrow verifier maps, switch and frozen list**

Remove `ambiguous_recovery` from `$errorPattern`, `Check-Negative`'s switch,
the exact `$cases` array, the synthetic refusal/stderr maps and synthetic
evidence generation. The verifier's exact list becomes:

```powershell
$cases=@(
 'unknown_nonempty','size_mismatch','occupied_letter','gen0',
 'writer_lock','missing_driver','busy_volume','compact_lock'
)
```

Change the aggregate cardinality check from 9 to 8. Keep all other identity,
argv, inventory, hash, refusal and oracle checks unchanged.

- [ ] **Step 4: Add mutations for missing and extra cases**

The existing mutation harness must reject both:

```powershell
@{file='m4-lifecycle-Negative.v1.json';change={param($x)$x.negative_cases=@($x.negative_cases|Select-Object -Skip 1)}}
@{file='m4-lifecycle-Negative.v1.json';change={param($x)$x.negative_cases+=@('ambiguous_recovery')}}
```

The second mutation proves the removed VM case cannot silently return.

- [ ] **Step 5: Run Final selftest GREEN**

Run the Step 2 command again.

Expected: exit 0 with exact `M4 Final PASS` selftest output and every mutation
rejected internally.

---

### Task 4: Record the contract amendment and verify locally

**Files:**
- Modify: `docs/superpowers/plans/2026-08-13-m4-contracted-product.md:1028-1062`
- Modify: `M4_TASKLIST.md` R7 execution record
- Verify: all files from Tasks 1-3

- [ ] **Step 1: Synchronize the original implementation plan**

State that real VM Negative requires the ordered eight realistic cases, while
zero/multiple matching-volume refusal is a local unit/composition boundary and
must not cause VM repartitioning.

- [ ] **Step 2: Record Formal10 and the RED/GREEN cycle**

Add the exact Formal10 facts: commit `74f16ca`, artifact SHA
`E5E710E00823A7C8426E39BBD89D0724AF05A4B3B73DA216EED648E298F919EB`,
Create evidence SHA
`36BBCF688EAC3C0C50D4CDF89D232C83D815C355FED106F870FFA3ED0092E8E1`,
Recover evidence SHA
`EC4E361F885BF0EACDD84C3530AFE59E9389D939EB2FFD12D479BAEE7C671341`,
Offline evidence SHA
`2C062C6455D2B6A6ED227F0C877ACA2B5BB7B033F49A403B91C5A714D9D97E4D`,
and the exact Negative RED `Negative missing fixture manifest.` State that
Final was not started and post-RED AIM/R/process baseline was clean.

- [ ] **Step 3: Run focused and cumulative verification sequentially**

From the VS developer environment:

```powershell
cmake --build out\build\msvc --config Debug --target `
  myrd myrd_m4_windows_volume_tests myrd_m4_product_composition_tests `
  myrd_m4_run_policy_tests
ctest --preset debug -L m4-v2 --output-on-failure
ctest --preset debug -L m4-v3 --output-on-failure

cmake --build out\build\msvc --config Release --target `
  myrd myrd_m4_windows_volume_tests myrd_m4_product_composition_tests `
  myrd_m4_run_policy_tests
ctest --preset release -L m4-v2 --output-on-failure
ctest --preset release -L m4-v3 --output-on-failure
```

Expected: Debug and Release `m4-v2` each 3/3 and `m4-v3` each 12/12.

- [ ] **Step 4: Run non-test gates**

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  tools\Measure-M4ProductLines.ps1
git diff --check
git status --short
git diff --cached --name-only
```

Expected: total is at most 2500, diff check exits 0, dirty files equal the
approved implementation allowlist and the index is empty.

- [ ] **Step 5: Obtain independent SPEC and quality approval**

SPEC review must confirm the original and narrow specs, runner, verifier and
local ambiguity coverage agree. Quality review must check PowerShell 5.1
pipeline/null behavior, map/list consistency, production classifier wiring,
source-contract robustness and ledger accuracy. Resolve every finding with a
new RED/GREEN cycle and repeat the corresponding review.

- [ ] **Step 6: Commit the local repair**

Stage only the reviewed allowlist, run cached diff/allowlist checks, and commit:

```powershell
git commit -m "test(m4): scope VM negatives to realistic cases"
```

Require a clean post-commit worktree and record the exact commit hash.

---

### Task 5: Prepare and execute fresh Formal11 evidence

**Files:**
- Read-only source: host Release artifact and committed runner/verifier
- Guest fresh root generated by the exact PowerShell expression below
- Guest manifest: `$fixturePath`, resolved as
  `"$stateFile.negative-fixtures.json"`
- Guest derived fixtures: `$fixturePaths`, each strictly below
  `[IO.Path]::GetFullPath("$bundle.r7-negative")`
- Local evidence copy: ignored `out\formal11-evidence`

- [ ] **Step 1: Rebuild and prove the clean VM baseline**

Build Release `myrd` from the exact new commit. Require clean Git status,
matching deployed hashes, the approved VM identity/MAC, Task23 service
Disabled/Stopped, no product/helper process, R absent, exact PD0/1/2 baseline
and DirectAimAdapter `ids=[]` before creating a random fresh root.

```powershell
$commit = (git rev-parse HEAD).Trim()
$formalRoot = "E:\myrd-m4-r7-$($commit.Substring(0,7))-formal11-$([guid]::NewGuid().ToString('N').Substring(0,12))"
```

Require `Test-Path -LiteralPath $formalRoot` to be false before the one-time
creation.

- [ ] **Step 2: Run four no-ack probes and the positive lifecycle**

Require exact single-line exit 77 for CreateKill, RecoverShutdown,
OfflineCompact and Negative with no state change. Then run acknowledged
CreateKill, one password-authorized VMX hard reset, RecoverShutdown and
OfflineCompact. A real RED stops the root permanently.

- [ ] **Step 3: Prepare seven driver-backed fixtures without disk mutation**

After OfflineCompact returns baseline, derive these exact paths:

```powershell
$bundle=Join-Path $formalRoot 'bundle'
$stateFile=Join-Path $formalRoot 'm4-lifecycle.state.json'
$negativeRoot=[IO.Path]::GetFullPath("$bundle.r7-negative")
$fixturePath="$stateFile.negative-fixtures.json"
$fixturePaths=[ordered]@{
  unknown_nonempty=(Join-Path $negativeRoot 'unknown_nonempty')
  size_mismatch=(Join-Path $negativeRoot 'size_mismatch')
  occupied_letter=(Join-Path $negativeRoot 'occupied_letter')
  gen0=(Join-Path $negativeRoot 'gen0')
  writer_lock=(Join-Path $negativeRoot 'writer_lock')
  missing_driver=(Join-Path $negativeRoot 'missing_driver')
  busy_volume=(Join-Path $negativeRoot 'busy_volume')
  compact_lock=(Join-Path $negativeRoot 'compact_lock')
}
```

Require `unknown_nonempty`, `occupied_letter` and `missing_driver` to remain
absent. Copy the main bundle independently to `size_mismatch`, `writer_lock`,
`busy_volume` and `compact_lock`.

The generation-zero source is the retained proved-incomplete
`E:\myrd-m4-r7-75c15e7-filediag3-54aa2cca\bundle`. Require it to exist, remain
read-only during copying and contain `disk.rdm`; otherwise stop before
Negative. Copy the current Formal11 compacted bundle separately into each
valid-bundle target. Do not touch PD0/1/2 partitions or format any volume.

- [ ] **Step 4: Execute the missing-driver partial pass**

Write a UTF-8-no-BOM manifest with only this active case:

```powershell
$missingManifest = [ordered]@{
  schema='myrd.m4.negative-fixtures.v1'
  active_cases=@('missing_driver')
  missing_driver=[ordered]@{
    path=(Join-Path $negativeRoot 'missing_driver')
    letter='S'
  }
}
[IO.File]::WriteAllText(
  $fixturePath,
  ($missingManifest|ConvertTo-Json -Depth 8 -Compress)+[Environment]::NewLine,
  [Text.UTF8Encoding]::new($false))
```

Require its path absent. Stop only exact service `phdskmnt`, poll bounded until
Stopped, and prove AIM IDs empty/R absent/PD baseline. Run Negative once.
Require the immutable `m4-negative-missing_driver.v1.json`, exact missing-driver
refusal and the exact expected exit-77 remaining-case boundary. No aggregate
Negative evidence may exist yet.

- [ ] **Step 5: Restore the driver and execute the remaining seven cases**

Start exact `phdskmnt`, poll bounded until Running, and re-prove AIM IDs empty,
R/S absent and PD baseline. Resolve the approved baseline D volume and replace
the manifest atomically:

```powershell
$dGuid=[string](Get-Volume -DriveLetter D -ErrorAction Stop).UniqueId
if($dGuid-notmatch'^\\\\\?\\Volume\{[0-9A-Fa-f-]{36}\}\\$'){
  throw 'approved D volume GUID is malformed'
}
$manifest=[ordered]@{
  schema='myrd.m4.negative-fixtures.v1'
  active_cases=@(
    'unknown_nonempty','size_mismatch','occupied_letter','gen0',
    'writer_lock','busy_volume','compact_lock')
  unknown_nonempty=[ordered]@{path=(Join-Path $negativeRoot 'unknown_nonempty');letter='S'}
  size_mismatch=[ordered]@{path=(Join-Path $negativeRoot 'size_mismatch');letter='S';expected_size='64M'}
  occupied_letter=[ordered]@{path=(Join-Path $negativeRoot 'occupied_letter');letter='D';volume_guid=$dGuid}
  gen0=[ordered]@{path=(Join-Path $negativeRoot 'gen0');letter='S';expected_size='64M'}
  writer_lock=[ordered]@{path=(Join-Path $negativeRoot 'writer_lock');letter='S';expected_size='64M'}
  busy_volume=[ordered]@{path=(Join-Path $negativeRoot 'busy_volume');letter='S';expected_size='64M';holder_relative_path='busy.lock'}
  compact_lock=[ordered]@{path=(Join-Path $negativeRoot 'compact_lock');letter='S';expected_size='64M'}
}
$json=($manifest|ConvertTo-Json -Depth 8 -Compress)+[Environment]::NewLine
$temp="$fixturePath.new"
[IO.File]::WriteAllText($temp,$json,[Text.UTF8Encoding]::new($false))
[IO.File]::Move($temp,$fixturePath)
```

Require `$fixturePath` to be absent immediately before the atomic replacement;
the first-pass manifest is moved to a current-owned audit filename rather than
overwritten. Run Negative once and require exit 0, aggregate PASS, all eight
immutable case files and clean AIM/R/PD inventory.

- [ ] **Step 6: Run host Final and independently audit evidence**

Copy only the four phase JSON files and eight case JSON files to ignored
`out\formal11-evidence`, preserving SHA-256. Run on the host:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  tools\Invoke-M4Verification.ps1 -Tier Final `
  -HostArtifact out\build\msvc\src\Release\myrd.exe `
  -EvidenceDirectory out\formal11-evidence
```

Expected exact success prefix: `M4 Final PASS: same-commit lifecycle evidence`.
An independent read-only audit must verify commit/artifact identity, immutable
hashes, generation transitions, recovery process audit, all eight refusal
oracles and final clean baseline.

---

### Task 6: Close R7 and run R8 regression

**Files:**
- Modify: `M4_TASKLIST.md`
- Verify: repository and all M4 test labels

- [ ] **Step 1: Record Formal11 evidence and mark R7 DONE**

Record exact root, commit, artifact SHA, four phase evidence hashes, eight case
evidence hashes, reset down/up/new-boot facts, Final output and independent
review verdict. Mark R7 DONE only when no required evidence is missing.

- [ ] **Step 2: Run R8 Debug and Release regression**

Rebuild all M4 targets and run `m4-v0`, `m4-v1`, `m4-v2` and `m4-v3`
sequentially in Debug then Release. Run product-line, architecture/prohibited
token, diff, allowlist and repository cleanliness gates required by R8.

- [ ] **Step 3: Obtain final GO review and commit the ledger**

Require independent SPEC and quality approval of the complete same-commit VM
evidence and R8 results. Stage only `M4_TASKLIST.md`, run cached diff check,
commit the R7/R8 ledger, and require a clean worktree.
