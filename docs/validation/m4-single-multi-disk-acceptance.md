# M4 单盘与多盘 VM 验收报告模板

> 本文件是人工执行的验收记录模板，不是自动化控制器。任何电源边界、进程终止、驱动配置和产品命令都由验收人员在批准的 Windows 11 VMware 来宾中单独执行。`tests/e2e/m4_vm_lifecycle.ps1` 与 `tests/e2e/m4_vm_multi_disk.ps1` 只做只读观察和证据复制，不启动产品、不改变设备状态。

## 0. 验收元数据

| 字段 | 值 |
|---|---|
| Git commit | `<commit>` |
| Release executable SHA-256 | `<sha256>` |
| Debug executable SHA-256 | `<sha256>` |
| Windows caption/build/version | `<value>` |
| VM UUID/vendor/product | `<value>` |
| Active MAC addresses | `<value>` |
| `phdskmnt.sys` path/version/service state/start mode | `<value>` |
| AIM version/path | `<value>` |
| Administrator token | `PASS / FAIL` |
| Bundle parent A/B and filesystem | `<path> / NTFS` |
| Target letters | `R: / S:` |
| Operator/date (UTC) | `<value>` |

Evidence root must be a new directory on a persistent NTFS volume. Never place a
bundle or evidence root on a RAM disk. Preserve `preflight.json`, each command's
`command.json`, `stdout.log`, `stderr.log`, `postrun.json`, `result.json`, and
the final file tree/hash output.

## 1. Classification rules

Use exactly one result classification per case:

- `PASS`: all product and evidence assertions passed.
- `PRODUCT_FAIL`: the approved VM preconditions were true, but the product returned an unexpected result, exit code, state transition, identity, generation, or cleanup result.
- `ENVIRONMENT_BLOCKED`: an approved VM, driver, AIM, permission, filesystem, or free-letter precondition was false. Do not run the product case.
- `HARNESS_FAIL`: an observer, evidence path, capture, or operator recording error; no product conclusion may be drawn.
- `USER_ABORTED`: the manual power/process boundary was not performed exactly as specified.

An environment block is not a product pass. A harness failure is not repaired by
loosening an assertion. Keep the original bundle and evidence when a case fails.

## 2. VM-1 — two-disk create and checkpoint

### Operator commands

First run the observer in `Preflight` mode and require `PASS`:

```powershell
.\tests\e2e\m4_vm_multi_disk.ps1 -Phase Preflight `
  -Executable .\out\build\msvc\src\Release\myrd.exe `
  -BundleA E:\m4\disk-a -BundleB E:\m4\disk-b `
  -DriveA R: -DriveB S: -EvidenceDirectory E:\m4\evidence\vm1-preflight
```

Manually run and capture each command's stdout, stderr, and exit code:

```powershell
myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
myrd.exe run --bundle E:\m4\disk-b --size 64M --drive-letter S: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
```

Write a distinct marker file on each volume, wait for an explicit successful
checkpoint generation in each output, and record SHA-256 hashes. Leave both
processes running for VM-3, or perform the observer after both commands:

```powershell
.\tests\e2e\m4_vm_multi_disk.ps1 -Phase PostRun `
  -Executable .\out\build\msvc\src\Release\myrd.exe `
  -BundleA E:\m4\disk-a -BundleB E:\m4\disk-b -DriveA R: -DriveB S: `
  -EvidenceDirectory E:\m4\evidence\vm1-post `
  -CommandLineA 'myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1' `
  -CommandStdoutA E:\m4\captures\a.stdout -CommandStderrA E:\m4\captures\a.stderr -CommandExitCodeA 0 `
  -CommandLineB 'myrd.exe run --bundle E:\m4\disk-b --size 64M --drive-letter S: --checkpoint-seconds 10 --checkpoint-dirty-mb 1' `
  -CommandStdoutB E:\m4\captures\b.stdout -CommandStderrB E:\m4\captures\b.stderr -CommandExitCodeB 0 `
  -SameBundleCollisionCommandLine 'myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R:' `
  -SameBundleCollisionStdout E:\m4\captures\same.stdout -SameBundleCollisionStderr E:\m4\captures\same.stderr -SameBundleCollisionExitCode 1 `
  -DriveCollisionCommandLine 'myrd.exe run --bundle E:\m4\disk-b --size 64M --drive-letter R:' `
  -DriveCollisionStdout E:\m4\captures\drive.stdout -DriveCollisionStderr E:\m4\captures\drive.stderr -DriveCollisionExitCode 1
```

### Assertions

- A and B exit `0` for creation and report a committed generation.
- `R:` and `S:` are NTFS volumes bound to different observed AIM identities.
- Bundle trees contain independent generation-bearing files and SHA-256 values.
- The same-bundle second instance exits non-zero and its output contains lock/busy evidence.
- The drive collision exits non-zero and its output identifies a drive/letter/volume conflict.
- No command uses the observer to launch or stop a product process.

## 3. VM-2 — single-disk abnormal recovery

1. With A running, write `A-checkpoint.txt` and wait for a recorded successful generation.
2. Write `A-after-checkpoint.txt` without waiting for another successful generation.
3. Manually terminate only the recorded A process PID. Record the PID, time, and exit observation.
4. Apply exactly one approved hypervisor Reset or Power Off→Power On boundary by hand. Do not combine boundaries or use an in-guest power command.
5. After login, run `Preflight` again and record the new boot time and clean device/letter baseline.
6. Re-run the original A command with the same bundle, size, and letter; capture output independently.

`A-checkpoint.txt` must match its checkpoint hash and generation. The later file
may be absent or present. Recovery must not format the existing bundle or create
a fresh generation merely because the later write was not committed.

## 4. VM-3 — dual-instance isolation and collision cases

Run A and B concurrently with distinct bundles and letters. While both are
running, repeat the same-bundle and drive-collision commands from VM-1 and pass
their captures to the observer. Verify:

- A's generation, bundle tree, AIM identity, volume identity, and cleanup ledger are independent of B's.
- Closing A normally does not remove B's `S:` volume or AIM device.
- A failure or non-zero collision attempt does not change B's files or generation.
- The final observer state identifies every residual process, drive, and AIM disk by number/unique ID/serial where available.

## 5. VM-4 — dual-disk abnormal recovery

1. Commit a distinct marker on A and B and record both successful generations and hashes.
2. Add one uncommitted marker to each disk.
3. Record both process PIDs, then apply one manual hypervisor power boundary.
4. After login, run the same A and B commands and capture each output separately.
5. Observe both recovered volumes and bundles with a new `PostRun` evidence directory.

Both committed markers must recover on their original letters and bundles. Each
uncommitted marker may be lost. No A identity may appear under B, and no B
identity may appear under A. After orderly cleanup, no owned process, letter, or
AIM disk may remain.

## 6. VM-5 — negative and integrity cases

Use disposable copies of the bundles. For each case, run only the approved
command and capture stdout, stderr, exit code, pre/post bundle tree and hashes:

| Case | Required result |
|---|---|
| Target letter already occupied | Non-zero; no foreign volume mutation or formatting |
| Non-empty directory without a valid manifest | Non-zero; no AIM/volume creation |
| Existing bundle geometry differs from request | Non-zero; no reformat |
| Same bundle writer lock held | Non-zero; existing instance remains healthy |
| Manifest minimally altered | Non-zero; fail closed |
| Journal tail truncated/altered | Non-zero or recovery of last complete generation only; never mixed data |
| Driver/AIM prerequisite unavailable | Deterministic precondition block before bundle mutation |
| Flush/checkpoint failure | Non-zero; only resources owned by this invocation are cleaned |

For every case, the untouched bundle copy and the other disk's observation must
remain available for comparison.

## 7. Driver recovery pre-experiment

This is a prerequisite experiment, not a product bypass. Record the service
configuration and query result before changing anything. On a disposable VM,
the operator may perform exactly one controlled transition:

```powershell
sc.exe config phdskmnt start= demand
sc.exe start phdskmnt
```

Record the command output, service state, AIM query result, target-letter state,
and whether a subsequent product recovery can discover the adapter. Restore the
approved service configuration before continuing. If starting the service does
not restore adapter/query availability, classify the environment as
`ENVIRONMENT_BLOCKED` and stop the remaining VM qualification; do not add an
unapproved device-install or restart workaround to the product.

## 8. Evidence checklist and final decision

Each VM case must include:

- preflight VM UUID, MAC, Windows/AIM/driver versions and administrator status;
- exact literal operator command line for every product attempt;
- separate stdout, stderr, and numeric exit code;
- observed committed generations from output and bundle files;
- recursive per-bundle relative path, byte length and SHA-256 list;
- A/R: and B/S: volume unique IDs, disk/partition numbers and filesystems;
- AIM disk number, unique ID/serial, size and operational status;
- before/after process and cleanup observations;
- classification and explanation for every non-pass case.

M4 is accepted only when VM-1 through VM-5 are `PASS`, the driver recovery
pre-experiment is either `PASS` or an explicitly documented approved support
profile, and the final state has no unowned `myrd` process, target letter, or AIM
device. Attach the evidence directory manifest and reviewer sign-off below.

**Final classification:** `PASS / PRODUCT_FAIL / ENVIRONMENT_BLOCKED / HARNESS_FAIL / USER_ABORTED`

**Reviewer:** `<name>`  **Date (UTC):** `<date>`  **Signature/commit:** `<value>`

## 9. Offline evidence verification

### 9.1 Assemble canonical case results

The observation runbooks intentionally never claim the canonical qualification
schema: they record preflight, captured commands, post-run state, and bundle
trees only. After the operator has finished a case, create a checkpoint JSON
and a cleanup JSON from those records, then assemble the verifier-facing result
without starting `myrd` or querying/changing a device:

```powershell
.\tools\Assemble-M4VmCaseResult.ps1 `
  -EvidenceRoot E:\m4\evidence\qualification `
  -CaseId VM-1 `
  -SourceCommit <40-hex-commit> `
  -ReleaseArtifact C:\m4\myrd-release.exe `
  -DebugArtifact C:\m4\myrd-debug.exe `
  -CheckpointEvidence E:\m4\captures\vm1-checkpoints.json `
  -Cleanup E:\m4\captures\vm1-cleanup.json `
  -Message 'Operator reviewed VM-1 preflight, command and post-run records.'
```

`CheckpointEvidence` must contain either an array or an object with a
`checkpoint_evidence` array. Each entry names a positive generation and files
whose `relative_path` is relative to the qualification evidence root; copy the
referenced marker/capture files into that root before assembly. `Cleanup` must
contain `zero_residuals`, `processes`, `drive_letters`, `aim_devices`, and
`owned_resources`; a PASS case requires the latter four arrays to be empty.
The assembler copies the two artifacts into `artifacts/`, computes their
SHA-256 values, validates every checkpoint file hash, and writes exactly
`<EvidenceRoot>/<CaseId>/result.json` with schema
`myrd.m4.vm.case-result.v1`. It is fail-closed on missing fields, traversal,
hash mismatch, or a non-empty cleanup list.

After all manual VM work is complete, assemble a new evidence root containing
`qualification.json`, `VM-1` through `VM-5` result directories, the driver
pre-experiment result, and the referenced artifact/capture files. The verifier
is read-only: it never starts `myrd`, queries a device, changes a service, or
controls VMware. It fails closed on a missing file, invalid schema, commit or
artifact hash mismatch, missing generation/file hash, non-empty residual list,
or a non-`PASS` classification.

```powershell
.\tools\Verify-M4SingleMultiDiskEvidence.ps1 `
  -EvidenceRoot E:\m4\evidence\qualification `
  -ExpectedSourceCommit <40-hex-commit> `
  -ExpectedReleaseSha256 <64-hex-release-hash> `
  -ExpectedDebugSha256 <64-hex-debug-hash>
```

Only the literal `M4 VM EVIDENCE PASS` output and exit code `0` qualify the
evidence set. Any other result is a failure requiring review; it must not be
reclassified by editing assertions or omitting evidence.
