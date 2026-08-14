# MyRamDisk v1.0 Release Qualification Design

> Status: APPROVED FOR PLANNING
> Date: 2026-08-15
> Implementation base: `952bcca624e7d7cd97ca304d19033edd3ece221e`
> Authoritative predecessors:
> `docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md`,
> `docs/superpowers/specs/2026-08-15-m4-negative-fixture-design.md`,
> `docs/superpowers/plans/2026-08-13-m4-contracted-product.md`, and
> `M4_TASKLIST.md`

## 1. Decision

MyRamDisk already has the intended M4 foreground product and accepted local
safety architecture. v1.0 work is release qualification, not a new product
milestone and not an invitation to add features.

The release target is a stable CLI product inside one declared support
profile. It is not universal Windows, storage-hardware or driver
certification. The remaining work is bounded to one primary formal VM run,
at most one same-scope repair run, the existing R8 local gates, documentation
and a small integrity-verifiable release archive.

This design freezes the acceptance surface so that strict testing has a
deterministic end.

## 2. v1.0 support profile

v1.0 supports:

- Windows 11 x64;
- an elevated interactive console;
- a compatible installed Arsenal Image Mounter `phdskmnt` driver;
- the driver/API identity proved by the formal VM baseline;
- bundle storage on a local NTFS path;
- one foreground `myrd.exe` process controlling one RAM disk;
- `myrd run` for create, recovery, checkpointing and clean shutdown;
- `myrd compact --bundle <path>` while the bundle is offline;
- recovery after an exact product-process kill followed by a hard VM reset;
- human-readable CLI output and the documented exit-code contract.

v1.0 does not include or promise:

- a GUI, installer, Windows service or auto-start manager;
- bundled or automatically installed third-party drivers;
- multiple disks per process or centralized multi-instance management;
- online or automatic compaction;
- automatic recovery from every host or hypervisor failure mode;
- certification across other Windows versions, physical-storage stacks or AIM
  releases;
- Authenticode signing or an MSI installer.

These exclusions are release boundaries, not unfinished v1.0 requirements.

## 3. Frozen acceptance budget

Release qualification has the following hard limits:

1. Formal12 is the primary and normally the only full VM qualification run.
2. Formal12 may perform at most two actual hard-reset events.
3. The real-VM Negative phase contains exactly the eight cases in Section 5.
4. No real-VM test repartitions, formats or deletes PD0, PD1 or the fixed RAW
   PD2. No test uninstalls or deletes `phdskmnt`.
5. Query helpers, direct `powershell.exe -File` capture wrappers, VM identity,
   root layout, arguments and evidence schemas are frozen before Formal12.
6. At most one pure setup correction is allowed across Formal12. It must be
   proved to occur before any product or driver command, must use a new fresh
   root and must repeat the complete clean baseline. A second setup error is a
   stop condition.
7. A real product, driver or evidence-contract defect may receive one minimal
   TDD repair followed by one full same-scope Formal13 run.
8. Formal13 may not add cases, resets, topology changes or product features.
9. If Formal13 fails, M4 is BLOCKED for architecture review. The v1.0 effort
   does not continue by adding Formal14 or ad-hoc scenarios.

Environment or authentication failures do not justify weakening a gate. They
stop the formal run for an explicit operator decision.

## 4. Formal12 positive lifecycle

Formal12 uses a never-before-used root and the exact fresh Release artifact
built from the frozen commit.

### 4.1 Preflight and no-ack

Before creating the root, prove:

- the linked worktree is at the exact commit and clean;
- exactly the approved VM is running;
- guest identity, MAC and administrator context match the approved baseline;
- the unrelated Task23 service is Disabled and Stopped;
- product, old helper, query and host processes are absent;
- DirectAimAdapter reports no AIM device;
- R: and S: are absent;
- PD0 and PD1 have their frozen 64 GiB GPT identities and partition counts;
- PD2 has its frozen 8 GiB RAW identity and zero partitions.

Deploy only the exact Release executable, lifecycle runner and verifier. All
four unacknowledged phases must independently return exit 77 with one exact
BLOCKED record and no process, filesystem, driver or disk-state change.

### 4.2 CreateKill and hard reset 1

The acknowledged CreateKill phase must prove:

- Ready generation 1 on the exact R: GPT/NTFS volume;
- a committed successor checkpoint;
- workload-A integrity;
- exact product PID termination and handle-based exit evidence;
- the expected owned AIM/volume handoff;
- immutable phase evidence bound to source commit and artifact SHA-256.

Then perform hard reset 1. The reset is either one successful
`vmrun ... reset ... hard` operation using only operator-approved credentials,
or an operator's VMware **Reset** / **Power Off then Power On** action. It must
not be a guest graceful shutdown. Credential text is never written to the
repository, logs or evidence.

The gate requires observed transport loss, transport recovery, a strictly
new guest boot time, the same VM identity, unchanged CreateKill evidence,
clean PD0/PD1/PD2, no AIM device and no R:/S: before recovery begins.

### 4.3 RecoverShutdown and OfflineCompact

RecoverShutdown must recover the same bundle without provisioning, preserve
workload A, create and preserve workload D, commit the required successor and
final generations, pass the process-descendant audit and return to the clean
baseline.

OfflineCompact must commit exactly one successor generation, preserve A and D,
start the compacted bundle successfully, return cleanly and leave no device,
drive letter or product process. Its immutable evidence must remain bound to
the same source, artifact and root.

Any real failure stops downstream execution and permanently retires the root.

## 5. Frozen real-VM Negative scope

The real VM exercises exactly these user-relevant cases:

1. `unknown_nonempty`;
2. `size_mismatch`;
3. `occupied_letter`;
4. `gen0`;
5. `writer_lock`;
6. `missing_driver`;
7. `busy_volume`;
8. `compact_lock`.

Zero/one/multiple recovered-volume candidates remain a production fail-closed
rule with local C++ unit coverage. Formal12 does not repartition a VM disk to
manufacture a second candidate volume.

All negative fixtures are independent copies or constrained non-bundle paths
below the approved derived fixture root. Preparation records complete
path/length/SHA-256 inventories and proves that source bundles are unchanged.
No fixture preparation partitions or formats a physical disk.

## 6. Bounded missing-driver procedure

`phdskmnt` is a demand-start SCSI miniport that cannot be stopped online on the
approved VM. Formal12 therefore uses one additional reset instead of trying
alternate stop mechanisms.

Immediately before changing the driver start mode, prove:

- DirectAimAdapter reports no device;
- R: and S: are absent;
- product, helper and host processes are absent;
- PD0, PD1 and PD2 exactly match the frozen baseline;
- the service is the exact approved `phdskmnt`, is Running, and has original
  `DEMAND_START` configuration.

Then:

1. Set only the exact `phdskmnt` start mode to Disabled.
2. Read back and record Disabled while leaving the currently loaded driver
   otherwise untouched.
3. Perform hard reset 2 using the same hard-reset rules as Section 4.2.
4. Prove a new boot, the same VM, `phdskmnt` not loaded, AIM empty, R:/S:
   absent and PD0/PD1/PD2 unchanged.
5. Run only `missing_driver` and save its immutable partial evidence.
6. Restore exactly `DEMAND_START`.
7. Execute one bounded `sc start phdskmnt` and prove Running.
8. Re-prove AIM empty, R:/S: absent, no relevant process and unchanged
   PD0/PD1/PD2.
9. Execute the other seven cases and produce the aggregate Negative evidence.

If configuration, hard-reset proof, partial evidence, online driver start or
the restored clean baseline fails, stop immediately. There is no third reset,
no alternate driver loader, no driver uninstall and no registry workaround
without a separately approved future design.

## 7. Final evidence gate

The independent Final verifier must accept only:

- the exact CreateKill, RecoverShutdown and OfflineCompact evidence;
- exactly the eight ordered Negative case files and one aggregate file;
- matching schema, source commit, artifact SHA-256, root and boot transitions;
- expected generations and workload hashes;
- clean stable inventories and descendant audits;
- no missing, duplicate, unexpected or mutated evidence.

Final PASS closes Formal12. Evidence disagreements fail closed and do not
trigger exploratory VM changes.

## 8. Formal13 repair rule

Formal13 exists only if Formal12 discovers a defect in the frozen product,
runner, verifier or evidence contract.

The repair sequence is:

1. preserve the Formal12 root and exact RED evidence;
2. diagnose the demonstrated boundary without adding a scenario;
3. add an actual-seam failing local test;
4. implement the smallest correction;
5. run the focused test and complete local Debug/Release M4 labels;
6. obtain independent SPEC and quality approval;
7. commit the correction;
8. run one full Formal13 from a fresh root with the same two-reset/eight-case
   contract.

Formal13 is not available for feature requests or wider certification. A
second real failure leaves the milestone BLOCKED.

## 9. R8 release gates

Version metadata, user documentation, packaging definitions and every file
that can affect `myrd.exe`, the lifecycle runner or the Final verifier are
completed before creating the release-candidate product commit. Formal12 (or
Formal13) builds and qualifies that exact commit and artifact.

After formal qualification, R8 performs the already specified local closure
against the same candidate:

- fresh Debug and Release configure/build/full CTest;
- cumulative M2.1, M3 and M4 labels;
- a `BUILD_TESTING=OFF` product-only build;
- prohibited seam/component binary and source scans;
- recovery-provisioning, unfinished-marker, TODO and drift audits;
- product physical-line budget checks;
- PowerShell 5.1 parsing and safety/no-ack contract checks;
- final VM evidence verification against the committed artifact;
- an exact clean-worktree and allowed-file audit.

No R8 gate may be waived to meet a release date.

After Final PASS, one release-record commit may change only release notes,
task/design/plan status records and generated package integrity metadata. It
may not change product sources, build definitions, resources, the lifecycle
runner, verifier or tests. A drift audit proves those qualification inputs are
unchanged from the candidate. The tag points to the release-record commit;
`BUILDINFO.txt` records the qualified candidate commit and artifact SHA-256.

## 10. Documentation and release artifact

v1.0 documentation must describe:

- the support profile and AIM prerequisite;
- exact commands, required administrator context and defaults;
- create versus recovery behavior;
- recovery's no-format guarantee;
- checkpoints, journal warning/full behavior and offline compaction;
- Ctrl+C clean shutdown and forced-kill recovery;
- the required hard-reset boundary after a forced kill;
- exit codes and common refusal diagnostics;
- the exclusions and limitations in Section 2;
- reproducible source/build instructions for the frozen toolchain profile.

Release metadata is updated consistently to `1.0.0` in CMake and the Windows
resource metadata. No new CLI feature is required solely to print a version.

The release archive is named `myrd-v1.0.0-windows-x64.zip` and contains only:

- `myrd.exe`;
- `README.md`;
- `LICENSE`;
- `RELEASE_NOTES.md`;
- `BUILDINFO.txt` with the qualified commit, toolchain profile and artifact
  SHA-256;
- `SHA256SUMS` covering every other shipped file.

The package does not redistribute AIM or install system components. A package
smoke test verifies extraction, `myrd --help`, `myrd run --help` and
`myrd compact --help` on the approved profile without mutating a disk.

The release is tagged `v1.0.0` only after independent SPEC, quality and release
reviews return GO and the repository is clean. Binary signing and an installer
remain separate post-v1 work.

## 11. Definition of done

v1.0 is complete only when all of the following are true:

1. Formal12 passes, or one approved repair is followed by a fully passing
   Formal13.
2. Final accepts the complete immutable evidence set.
3. Every R8 local, product-only, safety, line, architecture and drift gate
   passes in Debug and Release as applicable.
4. The driver is restored to `DEMAND_START`, is healthy, and the VM returns to
   the exact clean baseline.
5. Documentation and release metadata match the tested product contract.
6. The release archive and SHA-256 manifest pass their package smoke tests.
7. Independent reviews contain no Critical or Important finding.
8. The exact release commit is clean and tagged `v1.0.0`.

Passing this definition means stable within the declared v1.0 support profile.
It does not claim broader platform certification.
