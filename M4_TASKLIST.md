# M4 Contracted Product TASKLIST

This is the execution control file for the final MyRamDisk milestone. It is not
an independent requirement source. Every task must follow, in order:

1. `docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md`
2. `docs/superpowers/plans/2026-08-13-m4-contracted-product.md`
3. this file
4. the accepted M0-M3 contracts

Implementation base and workspace:

```text
4f9f912de56160062a007483fc56d5a02d5ff613
C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m4-contracted-product
feature/m4-contracted-product
```

The historical `feature/m4-task23-restart` branch is not an implementation
dependency and must not be merged or cherry-picked wholesale.

---

## A. Execution contract

### A1. Subagent-driven implementation

The project owner has frozen M4 to subagent-driven execution. For every R0-R8
implementation task, assign a fresh `gpt-5.6-terra` agent at medium reasoning
as implementer. After it implements the task, assign independent fresh
Terra/medium reviewers first for SPEC compliance and then for code quality.
The implementer fixes every review finding and the work is re-reviewed.

The main Sol/xhigh controller performs structural analysis with CodeGraph and
gives the implementer the needed context, maintains sequence and the one-DOING
limit, reads the diff, independently reruns verification, updates this ledger,
and creates each exact task commit. Never run multiple implementation agents
concurrently in the shared worktree. If implementation fails, the controller
first determines whether the task or plan is defective, context is missing, the
task is too large, or the work is difficult. If the plan is wrong, pause and
request or record a decision; only take over implementation when the task is
sound but Terra cannot complete it.

### A2. Before a task

- [ ] Confirm the fixed worktree, branch, HEAD and status.
- [ ] Protect every pre-existing modification and untracked path.
- [ ] Read the full SPEC sections referenced by the task.
- [ ] Read the matching PLAN task and this task card.
- [ ] Require every dependency to be `DONE` with accessible evidence.
- [ ] Ensure the table has at most one `DOING` task.
- [ ] Change the task from `PENDING` to `DOING` and fill the start record.
- [ ] Assign the fresh implementer and reserve independent fresh SPEC-compliance and code-quality reviewers.
- [ ] Have the implementer add and run the focused RED before product implementation.

### A3. Before a task is DONE

- [ ] Run the PLAN's focused Debug verification.
- [ ] Run the task's cumulative M3/M4 labels.
- [ ] Run the required Release check.
- [ ] Run `tools/Measure-M4ProductLines.ps1`.
- [ ] Run product-boundary/prohibited-string and test-seam scans.
- [ ] Run `git diff --check` and `git status --short`.
- [ ] Map the actual files to the allowed-file list.
- [ ] Obtain independent SPEC-compliance and code-quality reviews; have the implementer fix findings and obtain re-review.
- [ ] Have the main controller read the diff and independently rerun the required verification.
- [ ] Record the implementer's RED and implementation, the reviewers' findings and re-review, the main-controller verification, and remaining exclusions.
- [ ] Update status to `DONE` only with fresh evidence.
- [ ] Have the main controller stage exact paths and create the fixed task commit.

### A4. Status values

- `PENDING`: not started.
- `DOING`: active; at most one task.
- `BLOCKED`: a concrete external/environmental blocker prevents progress.
- `BLOCKED_DECISION`: R1 found a product decision that only the owner can make.
- `DONE`: implementation, tests, record and exact commit are complete.

An ordinary test failure is not `BLOCKED`; keep the task `DOING` and debug it.

### A5. Stop conditions

Stop the current task without beginning the next if a change would:

- add Service, Host, Worker, named-pipe IPC, profile or operation-store code;
- add M4 durable phase, transition, admission, evidence or revision state;
- add an interface/manager/strategy for a single concrete call;
- copy M3 checkpoint or compaction durability algorithms;
- format a recovery-path disk;
- dismount after volume-lock failure;
- clean up an orphan AIM device without reboot-bound identity;
- exceed 2500 product lines;
- weaken an accepted M0-M3 contract;
- require an M5 for product usability.

Preserve the RED, document the exact conflict and request a scoped decision.

---

## B. Build, test and evidence conventions

### B1. Standard local commands

Run inside an MSVC x64 developer environment:

```powershell
cmake --preset msvc
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

If CMake selects MinGW or `cl` reports a missing standard header, load:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
```

Do not classify that environment failure as a code RED.

### B2. M4 labels

```powershell
ctest --preset release -L m4-v0 --output-on-failure
ctest --preset release -L m4-v1 --output-on-failure
ctest --preset release -L m4-v2 --output-on-failure
ctest --preset release -L m4-v3 --output-on-failure
```

`m4-v4` is the explicit real VM route; it is not silently skipped inside local
CTest.

### B3. Evidence record format

Each task record must use:

```text
Task / status:
Executor / UTC timestamp:
Start commit / branch / end commit:
Allowed files / actual files:
Dependencies and evidence:
RED command / observed failure / exit code:
Implementation summary:
Focused Debug verification / exit code:
Cumulative and Release verification / exit code:
Line count / architecture and seam scans:
VM evidence paths and SHA-256, if applicable:
SPEC/PLAN/TASKLIST drift audit:
Open risks / explicitly excluded follow-up:
Commit subject / commit hash:
```

No old-branch result, reviewer assertion or earlier-turn output counts as fresh
task evidence.

---

## C. Overall status

| ID | Task | Status | Depends on | Primary gate | Fixed commit subject |
|---|---|---|---|---|---|
| R0 | Clean product boundary | DONE | M3 GO | `m4-v0`, product ancestry and line counter | `build(m4): establish contracted product boundary` |
| R1 | Online compaction feasibility | DONE | R0 | NO-GO evidence, owner amendment, explicit defer | `docs(m4): resolve compaction scope` |
| R2 | Exact product CLI | DONE | R1 | run/compact CLI matrix, `m4-v0` | `feat(m4): add minimal product commands` |
| R3 | Minimal M3 product runtime | DONE | R2 | create/recover/offline compact, stats, typed journal capacity, `m3-v2`, `m4-v1` | `feat(m4): expose minimal m3 product runtime` |
| R4 | Exact Windows volume | DONE | R3 | anti-misformat, no-format recovery, `m4-v2` | `feat(m4): provision and recover exact windows volume` |
| R5 | Persistence maintenance loop | DONE | R4 | checkpoint, 80% warning, full-journal stop-retry policy, `m4-v3` | `feat(m4): schedule persistence maintenance` |
| R6 | Safe Ctrl+C shutdown | DONE | R5 | lock/flush/dismount order, busy refusal, `m4-v3` | `feat(m4): shut down ram disk safely` |
| R7 | Real AIM/NTFS lifecycle | PENDING | R6 | create-kill-reboot-recover-shutdown VM PASS | `test(m4): prove real run lifecycle` |
| R8 | Final regression and GO | PENDING | R7 | Debug/Release full, M0-M4 labels, OFF scan, <=2500 | `docs(m4): close contracted product milestone` |

R1's NO-GO experiment is closed by the owner-approved v1 amendment. R2 may
start only after the exact decision-document commit exists.

---

## D. Task cards

## R0 — Clean product boundary

**SPEC:** Sections 1, 4, 6, 13.
**PLAN:** Task R0.
**Allowed files:**

```text
src/run/main.cpp
src/CMakeLists.txt
tests/CMakeLists.txt
tests/unit/m4_product_boundary_tests.ps1
tools/Measure-M4ProductLines.ps1
M4_TASKLIST.md
```

**Acceptance:**

- [x] Branch descends from M3 GO and does not descend from Task23.
- [x] `myrd.exe` exists and exposes only the temporary contracted help shell.
- [x] Product binary has no old-M4 component tokens.
- [x] Line-count tool fails closed above a supplied maximum and reports the
  real 2500-line gate.
- [x] `unit.m4.product_boundary` is GREEN in Debug and Release.
- [x] Exact commit exists.

**Execution record:**

```text
Task / status: R0 / DONE
Executor / UTC timestamp: fresh gpt-5.6-terra medium implementer / 2026-08-12T20:07:15Z
Start commit / branch / end commit: b66b812a42b25581335787975abc2b8cef7a408a / feature/m4-contracted-product / this R0 commit at Git HEAD
Allowed files / actual files: R0 frozen allowlist / src/run/main.cpp; src/CMakeLists.txt; tests/CMakeLists.txt; tests/unit/m4_product_boundary_tests.ps1; tools/Measure-M4ProductLines.ps1; M4_TASKLIST.md
Dependencies and evidence: M3 GO 4f9f912 is ancestor (exit 0); Task23 5acc289 is not ancestor (exit 1); start worktree clean before R0 ledger update
RED command / observed failure / exit code: `cmd /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc && cmake --build --preset debug --target myrd_m4_product_boundary_tests --parallel 1'`; CMake generator expression `$<TARGET_FILE:myrd>` failed because target `myrd` did not exist; exit 1
Implementation summary: added the temporary `myrd --help` usage shell, focused PowerShell product-boundary assertion (bounded process, exact stdout/empty stderr, timeout cleanup, ASCII/UTF-16LE forbidden-token scan with self-audit, stderr-pollution and default/max-zero line-count self-checks), CMake custom target/CTest registration, and fail-closed M4 product-line counter; review repairs added language-aware comment handling, UTF-8/NUL/path guards, exact Git hunk counting without base-text reconstruction, cross-line C++ string/raw-string and PowerShell block/here-string handling, process cleanup hardening, and isolated regression fixtures including a five-file binary-base/no-hunk fail-closed proof
Focused Debug verification / exit code: MSVC Debug `myrd_m4_product_boundary_tests` target and `ctest --preset debug -R '^unit\.m4\.product_boundary$' --output-on-failure`; both exit 0
Cumulative and Release verification / exit code: MSVC Release `myrd_m4_product_boundary_tests` target and `ctest --preset release -R '^unit\.m4\.product_boundary$' --output-on-failure`; both exit 0; no full M3/VM route run per R0 exclusion
Line count / architecture and seam scans: main-controller fresh counter reported `src/run/main.cpp=12`, `TOTAL=12 MAXIMUM=2500` exit 0; `-Maximum 0` exit 1; invalid Base exit 1; Debug/Release product binary scan for old-M4 and representative test-seam tokens had no match; exact six-file allowlist and `git diff --check` passed
VM evidence paths and SHA-256, if applicable: not applicable to R0
SPEC/PLAN/TASKLIST drift audit: execution contract aligned by b66b812 and R0 sample include corrected; all SPEC findings and review-driven regression gaps were repaired and re-reviewed; final independent SPEC review APPROVED and final independent code-quality review APPROVED with no Critical/Important/Minor; main controller read the full diff and reran the required verification at 2026-08-13T04:12:21Z; no R1 or old-M4 content entered R0
Open risks / explicitly excluded follow-up: R0 is only a boundary/help shell; no M3 VM rerun and no R1 work; `vswhere.exe` emits a non-fatal vcvars warning after successful commands
Commit subject / commit hash: build(m4): establish contracted product boundary / this R0 commit at Git HEAD
```

## R1 — Online compaction feasibility

**SPEC:** Sections 7, 9.5 and 10.2.
**PLAN:** Task R1.
**Historical spike allowlist (the experiment was removed):**

```text
src/m3/m3_provider.h
src/m3/m3_provider.cpp
src/m3/m3_windows_acceptance.h
src/m3/m3_windows_acceptance.cpp
tests/integration/m3_online_compaction_spike_tests.cpp
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [x] Historical RED proves live provider lacks online compaction.
- [x] NO-GO proves a bounded method would require algorithm copy or an
  independently scoped shared M3 finalization refactor.
- [x] No algorithm copy, fake revision, durable M4 record or new abstraction is
  retained; final experimental production delta is zero.
- [x] If any bound fails, experiment is removed, evidence is recorded, task is
  `BLOCKED_DECISION`, and no later task starts.
- [x] Owner selected removal of online/automatic compaction from v1.
- [x] SPEC/PLAN/TASKLIST define supported offline `myrd compact`, 80% journal
  warning, typed full-journal handling and actionable recovery.
- [x] `docs/deferred/online-compaction.md` records the R1 root cause and
  independent reconsideration conditions outside v1.
- [x] Exact decision-document commit exists.

**Execution record:**

```text
Task / status: R1 / DONE
Executor / UTC timestamp: spike by fresh gpt-5.6-terra medium implementer; owner amendment and final document review by main Sol/xhigh / 2026-08-13
Start commit / branch / end commit: 89b441b06d8fcc926094cf7cc28805845a28cb9d / feature/m4-contracted-product / this decision-document commit at Git HEAD
Allowed files / actual files: R1 decision documents / M4_TASKLIST.md; docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md; docs/superpowers/plans/2026-08-13-m4-contracted-product.md; docs/deferred/online-compaction.md; all experimental product, test and CMake edits removed
Dependencies and evidence: R0 DONE at 89b441b with Debug/Release m4-v0 boundary evidence; M3 GO remains ancestor and Task23 remains outside ancestry
Historical observed RED (experiment reverted): `cmd /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset debug --target myrd_m3_online_compaction_spike_tests --parallel 1'`; after adding the missing test-only VHDX header, MSVC C2039 reported `M3Provider::compact` does not exist (the sole effective RED); exit 1. The target and test were removed with the experiment, so this command cannot be run against the final tree.
Implementation summary: attempted only the permitted provider bridge: concrete outcome/method, provider-local artifact names/identities and `CompactionCoordinator::begin/build_artifacts/prepare_delta`. The bridge reached a prepared transaction but cannot use the provider's existing `resume_pending_checkpoint`, because that method starts after manifest publication. Correct online behavior would require reproducing manifest mutation, publication disposition handling, commit resolution and reconciliation before that helper, or moving that common transaction-finalization algorithm from the lifecycle runner/provider into an M3 shared refactor. The former is a second lifecycle implementation and the latter exceeds this spike's allowed scope.
Focused Debug verification / exit code: RED command above exit 1 as required; GO verification not run because the bound failed before a valid implementation existed
Cumulative and Release verification / exit code: not applicable to the NO-GO decision; no online implementation retained
Line count / architecture and seam scans: experiment measured 204 net lines across the scoped source+spike-test paths before finalization; exact source-only split was not retained. Regardless, independent structural review confirmed completing publish/resolve/reconcile would violate the no-copy/shared-refactor boundary. The line count was pressure evidence; the blocker is the required algorithm copy or disallowed shared refactor. Experimental delta was then exactly removed. Final R1 production delta is 0.
VM evidence paths and SHA-256, if applicable: not applicable to R1
SPEC/PLAN/TASKLIST drift audit: R1's specified decision condition occurred. SPEC BLOCKER APPROVED. Quality review findings in the blocker ledger were fixed and re-review passed. On 2026-08-13 the owner approved removing online/automatic compaction from v1, adding the same-executable offline `myrd compact --bundle <path>` path, requiring the R5 80% warning and typed full-journal stop-retry behavior, and recording the online work explicitly outside v1. The amended documents were self-reviewed for command, task, test, line-budget and README consistency before this decision commit. No coordinator interface was changed and no algorithm copy was retained.
Open risks / explicitly excluded follow-up: online compaction remains outside v1 and may be reconsidered only through the independent M3 design conditions in docs/deferred/online-compaction.md. R3 must prove the offline command is ordinary-user usable and does not expose the engineering CLI. No revisions, records, abstractions or production spike remain.
Commit subject / commit hash: docs(m4): resolve compaction scope / this decision-document commit at Git HEAD
```

## R2 — Exact product CLI

**SPEC:** Section 3.
**PLAN:** Task R2.
**Allowed files:**

```text
src/run/cli.h
src/run/cli.cpp
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m4_cli_tests.cpp
tests/unit/m4_product_boundary_tests.ps1
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [x] Only `myrd --help`, `myrd run --help`, the exact run form,
  `myrd compact --help` and `myrd compact --bundle <path>` parse.
- [x] Defaults and every numeric/path/drive-letter bound are frozen by tests.
- [x] Duplicate, unknown, incomplete and device-path inputs exit 2 before
  filesystem, AIM or volume access.
- [x] Output is human-only and stderr/stdout purity is tested.
- [x] Debug/Release CLI and `m4-v0` pass.
- [x] Exact commit exists.

**Execution record:**

```text
Task / status: R2 / DONE
Executor / UTC timestamp: Terra/medium implementer; independent Terra/medium SPEC and quality reviewers; main Sol/xhigh controller / 2026-08-13T08:17:54Z
Start commit / branch / end commit: 4cb5a76905ec6c22d923029258316a6af2daaaae / feature/m4-contracted-product / this R2 commit at Git HEAD
Allowed files / actual files: frozen R2 allowlist / M4_TASKLIST.md; src/CMakeLists.txt; src/run/cli.h; src/run/cli.cpp; src/run/main.cpp; tests/CMakeLists.txt; tests/unit/m4_cli_tests.cpp; tests/unit/m4_product_boundary_tests.ps1
Dependencies and evidence: R0 DONE at 89b441b; R1 DONE at 4cb5a76; fresh Debug and Release unit.m4.product_boundary each 1/1 PASS before R2
RED command / observed failure / exit code: initial `cmake --build --preset debug --target myrd_m4_cli_tests --parallel 1` reported unknown target `myrd_m4_cli_tests`, exit 1. Expansion RED: `ctest --preset debug -R ^unit\.m4\.cli$ --output-on-failure` failed because parser errors had empty messages, exit 1. ADS coverage also initially failed before its parser repair. Final R2 scope RED: the same CLI target failed the new ordinary `C:\globalroot-not-device` path case because lexical substring rejection treated it as a device path, exit 1; rejection was narrowed to actual namespace prefixes. Quality RED: same CLI test command failed new `\GLOBAL??\root` / `\DosDevices\root` cases before those native namespace prefixes were rejected, exit 1. The lexical runtime-API scan is a coverage strengthening with self-audited positive/negative synthetic source snippets. Final scanner RED: Debug product-boundary target failed because a raw string self-audit containing `DeviceIoControl(` was falsely detected, exit 1; bounded C++ raw-literal stripping and malformed/unterminated fail-closed checks were added. Final minor RED: form-feed and vertical-tab raw delimiter self-audits did not fail closed, exit 1; delimiter validation now uses PS5.1-compatible `[char]::IsControl` with strict-mode-safe array counting. Main-controller-found contract RED: changing the process assertion to global-usage-only for no arguments made the Debug boundary target fail because `unknown command` prefixed stderr, exit 1; `main` now emits only global usage for an empty argument list while other parser errors remain actionable. Additional accepted-form/boundary cases that passed on first run are coverage repairs, not new RED evidence.
Implementation summary: added the filesystem-free exact CLI parser using bounded ASCII narrowing plus `std::from_chars`, frozen final option/result types, actionable parser messages, exact process routing/output coverage and runtime-API static boundary assertion; parser tests assert actual K/M/G/T scaling, defaults and accepted numeric boundary values. The first value-assertion test run failed only because its new helper accidentally duplicated `--size`; after correcting the test helper, values passed without production change (coverage repair). Valid run and compact forms print their temporary unavailable result.
Focused Debug verification / exit code: main-controller fresh MSVC Debug build of myrd_m4_cli_tests, myrd_m4_product_boundary_tests and myrd; focused CTest unit.m4.cli plus unit.m4.product_boundary 2/2 PASS; full m4-v0 2/2 PASS; combined command exit 0
Cumulative and Release verification / exit code: main-controller fresh MSVC Release build of the same targets; focused CTest 2/2 PASS; full m4-v0 2/2 PASS; combined Debug/Release command exit 0
Line count / architecture and seam scans: product line counter exit 0, cli.cpp=71 cli.h=27 main.cpp=17 TOTAL=115/2500; exact eight-file allowlist, diff check, unfinished/prohibited src/run scans, Release product-binary old-M4/test-seam scan and manual no-args/valid-compact/invalid-run process contracts all PASS
VM evidence paths and SHA-256, if applicable: not applicable to R2
SPEC/PLAN/TASKLIST drift audit: independent SPEC review initially found required from_chars/final/actionable-message/full-matrix/process-output and namespace/false-rejection gaps; implementer repaired and re-review APPROVED. Independent quality review found native GLOBAL??/DosDevices path gaps and a shallow source-API scanner; repairs added namespace coverage and a self-audited comment/string/raw-string-aware scanner. Raw delimiter control-character finding was fixed and final quality re-review APPROVED with zero Critical/Important/Minor. Main controller then found the no-argument exact-output drift, obtained a real boundary RED, required the narrow fix, and both reviewers re-approved before the main fresh verification above.
Open risks / explicitly excluded follow-up: R2 implements parsing/rendering only; create/recover/offline compaction execution remains R3
Commit subject / commit hash: feat(m4): add minimal product commands / this R2 commit at Git HEAD
```

## R3 — Minimal M3 product runtime

**SPEC:** Sections 8 and 9.1-9.5.
**PLAN:** Task R3.
**Allowed files:**

```text
src/m3/m3_provider.h
src/m3/m3_provider.cpp
src/m3/m3_windows_acceptance.h
src/m3/m3_windows_acceptance.cpp
src/m3/m3_aim_server_main.cpp
src/m3/m3_lifecycle_runner.h
src/m3/m3_lifecycle_runner.cpp
src/persistence/persistence_types.h
src/persistence/journal_writer.h
src/persistence/journal_writer.cpp
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m3_journal_writer_tests.cpp
tests/unit/m3_persistence_types_tests.cpp
tests/unit/m4_product_boundary_tests.ps1
tests/integration/m4_product_composition_tests.cpp
tests/CMakeLists.txt
tools/Measure-M4ProductLines.ps1
M4_TASKLIST.md
```

**Acceptance:**

- [x] Shared production persistence/store values preserve accepted M3 limits.
- [x] The product RAM-policy matrix freezes 64 MiB, default 2 GiB and 1 TiB:
  hard is the largest storage-block-aligned value no greater than the requested
  `ram-limit`, soft is half that aligned hard value rounded down to a block,
  low watermark is one quarter rounded down to a block, and every case
  preserves accepted M2 `soft < hard`. It also freezes 65537 KiB -> hard
  64 MiB / soft 32 MiB / low 16 MiB and validates every resulting option set
  with `validate_dynamic_block_store_options()`.
- [x] Generation-zero create uses existing creator and independently reopens.
- [x] Internal new-image generation zero bypasses only the existing-bundle
  product gate; an existing generation-zero bundle is rejected.
- [x] Recover requires exact geometry.
- [x] Maintenance stats use checked dirty-byte math and current journal truth.
- [x] Provider and runtime stats return `Result`; fact/arithmetic failure is
  never truncated or treated as a successful zero value.
- [x] The M4 product provider alone enforces the 2048 MiB cumulative committed
  journal ceiling and reports typed `JournalCapacityExhausted` without changing
  existing M3 per-operation resource semantics.
- [x] `JournalCapacityExhausted` has frozen value 20; equality succeeds,
  crossing does not advance selected manifest/generation/committed length, and
  an unset ceiling preserves accepted M3 behavior.
- [x] Provider checkpoint outcome preserves the typed capacity error unchanged.
- [x] Offline compact fixes `disk.rdm` and product limits, rejects generation
  zero/writer conflict, advances one committed generation, reopens identical
  data and starts no AIM runtime.
- [x] Offline compact freezes actionable success/in-use output and never exposes
  `myrd_m3_e2e`, `RawTest` or engineering store/persistence options.
- [x] M3 `m3-v2`, provider, recovery and Windows acceptance pass.
- [x] Exact commit exists.

**Execution record:**

```text
Task / status: R3 / DONE
Executor / UTC timestamp: assigned gpt-5.6-terra medium implementer; independent Terra/medium SPEC and quality reviewers; main Sol/xhigh controller / 2026-08-13T12:38:53Z
Start commit / branch / end commit: 65559b4f26c619a423a58199541aecc40db79848 / feature/m4-contracted-product / 3dc1e2eb95a02048fa5499a07984414096fefb52
Allowed files / actual files: frozen R3 allowlist / M4_TASKLIST.md; src/CMakeLists.txt; src/m3/m3_aim_server_main.cpp; src/m3/m3_lifecycle_runner.h; src/m3/m3_lifecycle_runner.cpp; src/m3/m3_provider.h; src/m3/m3_provider.cpp; src/m3/m3_windows_acceptance.h; src/m3/m3_windows_acceptance.cpp; src/persistence/journal_writer.h; src/persistence/journal_writer.cpp; src/persistence/persistence_types.h; src/run/main.cpp; tests/CMakeLists.txt; tests/integration/m4_product_composition_tests.cpp; tests/unit/m3_journal_writer_tests.cpp; tests/unit/m3_persistence_types_tests.cpp; tests/unit/m4_product_boundary_tests.ps1; tools/Measure-M4ProductLines.ps1
Dependencies and evidence: R2 DONE at 65559b4; owner-approved RAM-policy amendment committed as 8135fa160ea7760582f9d2136bd0d09eb4c695c7 before final R3 implementation; branch remains feature/m4-contracted-product and M3 GO remains its accepted base
RED command / observed failure / exit code: initial MSVC Debug `cmake --build --preset debug --target myrd_m4_product_composition_tests --parallel 1` failed because the product composition target/default and maintenance-stat APIs did not exist, exit 1. Independent slice REDs then proved missing frozen code-20/wire support, optional JournalWriter equality/crossing behavior, live provider maintenance facts, provider typed capacity preservation/nonadvance and the offline product wrapper. The amended RAM matrix failed because the former store constructor copied the requested 2 GiB into soft instead of deriving hard/soft/low; shared-limit RED also caught journal 512 MiB and BAT 64 MiB drift from the accepted engineering values. Process REDs proved absent-directory create did not retain generation zero, existing generation zero lacked actionable output, locked compact printed the literal `<the same bundle path>`, unknown nonempty directories lacked the M3 operation/message, runtime start cleanup was ignored, post-start runtime facts were unchecked and newly created generation zero was incorrectly rejected by an unconditional post-start generation gate. Static boundary RED proved the offline wrapper compared its final result to stale pre-lock discovery. Each RED exited 1 for the intended missing behavior before its minimal repair; test-setup mistakes (fixture namespace typo, strict-mode scalar Count and incomplete line-counter fixture files) were recorded as test repairs, not feature REDs.
Implementation summary: extracted exact accepted M3 production persistence/store values and retained engineering defaults; implemented the owner-approved aligned hard/half/quarter RAM policy; added checked Result maintenance stats and the concrete runtime forwarder; froze PersistenceErrorCode::JournalCapacityExhausted=20 with string/wire mappings; added an optional absolute committed-end ceiling used only by product main and preserved provider error context plus selected generation/committed-length nonadvance. Added one offline product-compaction wrapper that discovers geometry, rejects generation zero, supplies fixed disk.rdm/product limits, delegates to the existing lifecycle algorithm and requires exit 0, cleanup and Committed without a stale pre-lock generation comparison. Product main now classifies absent/empty/recover/unknown bundles fail-closed, creates random-identity generation zero through the existing creator, requires exact recover geometry, routes compact with exact actual-path output, validates started runtime size and Result maintenance facts, rejects post-start generation zero only in recover mode, cleans up start/fact failures, and temporarily stops at the R4 unavailable boundary. Ordinary run errors include the safe bundle path plus the concrete operation/message; existing generation zero has the dedicated verify-path/manual-remove/rerun message and remains unmodified. Product composition tests cover the RAM matrix, current dirty/journal facts, typed ceiling behavior, generation-one offline compact/reopen bytes, held-writer process output and nonmutation; the boundary test covers create artifacts, existing-generation-zero nonmutation, unknown-directory nonmutation, temporary R4 routing, line-counter fixtures and prohibited product tokens.
Focused Debug verification / exit code: main-controller fresh exact focused CTest set `unit.m3.persistence_types`, `unit.m3.journal_writer`, `integration.m3.recovery`, `integration.m3.image_create`, `integration.m3.compaction`, `integration.m3.provider`, `integration.m3.windows_acceptance`, `integration.m4.product_composition` passed 8/8, exit 0; fresh Debug product-boundary target/test passed, exit 0
Cumulative and Release verification / exit code: main-controller fresh Debug `ctest --preset debug -L m3-v2 --output-on-failure` passed 32/32 and `-L m4-v1` passed 1/1, exit 0; fresh Release exact focused set passed 8/8 and Release product boundary passed, exit 0
Line count / architecture and seam scans: main-controller fresh counter reported TOTAL=452 MAXIMUM=2500, exit 0; exact pre-ledger implementation allowlist was 18/18 and this record adds only M4_TASKLIST.md; `git diff --check` passed; Release product-binary prohibited-token scan and diff prohibited-concept/placeholder scan passed; no online/automatic compaction, new interface, copied publication/reconciliation algorithm, engineering command or test seam entered the product
VM evidence paths and SHA-256, if applicable: not applicable to R3; real AIM/NTFS lifecycle and destructive/reboot evidence remain R7
SPEC/PLAN/TASKLIST drift audit: owner RAM-policy amendment is commit 8135fa160ea7760582f9d2136bd0d09eb4c695c7. Independent SPEC review findings for actual-path writer-conflict output and actionable existing-generation-zero recovery were repaired with process-level nonmutation evidence; final SPEC review APPROVED. Independent quality review findings for stale pre-lock compaction comparison, post-start runtime fact validation and safe run diagnostics were repaired; the final create-path generation-zero condition was narrowed to recover mode; final quality review APPROVED. Main controller read the diff and independently reran the fresh evidence above. Exact R3 implementation commit 3dc1e2eb95a02048fa5499a07984414096fefb52 satisfies A4, so R3 is DONE.
Open risks / explicitly excluded follow-up: Windows elevation, exact-volume discovery/provisioning/anti-misformat and first filesystem checkpoint are R4; maintenance scheduling/journal warning policy is R5; Ctrl+C locked shutdown is R6; real VM kill/reboot/recover is R7; online/automatic compaction remains explicitly deferred outside v1 and was not reintroduced
Commit subject / commit hash: feat(m4): expose minimal m3 product runtime / 3dc1e2eb95a02048fa5499a07984414096fefb52
```

## R4 — Exact Windows volume

**SPEC:** Sections 5, 9 and 11.2.
**PLAN:** Task R4.
**Allowed files:**

```text
src/run/windows_volume.h
src/run/windows_volume.cpp
src/run/provision_volume.ps1
src/run/run_resources.rc
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m4_windows_volume_tests.cpp
tests/unit/test-m4-provision-script.ps1
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [x] All seven pre-provision anti-misformat checks fail closed.
- [x] Provision helper is fixed, embedded, bounded and accepts only disk number
  and drive letter.
- [x] Create produces exactly one GPT basic-data partition and NTFS volume on
  the exact runtime PhysicalDrive.
- [x] First volume flush precedes committed generation-one checkpoint.
- [x] Recover locates exact volume by disk extents and never calls provisioning.
- [x] Occupied drive letters and ambiguous/mixed volumes are rejected.
- [x] `m4-v2`, M3 Windows acceptance and product composition pass.
- [x] Exact commit exists.

**Execution record:**

```text
Task / status: R4 / DONE
Executor / UTC timestamp: delegated implementation agent; independent SPEC and quality reviewers; main controller / 2026-08-13
Start commit / branch: 62414f2286b84dfcc02c5e3aaec90ba2cf87ccb7 / feature/m4-contracted-product
Initial RED: correctly loaded VS2022 BuildTools Debug build of myrd_m4_windows_volume_tests failed because src/run/windows_volume.h was missing, exit 1.
TDD and safety record: subsequent REDs exposed missing native device-number proof, finite-query rejection, create/recover call graph, pre-start drive-letter preflight, one-TU predicate naming, script AST ordering, temporary-file error precedence, and token duplication access. GREEN adds fail-closed runtime/device/address/geometry/sector/layout/extents/mount validation; fixed embedded GPT/NTFS script AST allowlist; bounded helper resource extraction, restricted child handle inheritance, bounded output draining/timeout behavior and cleanup; exact volume device opens/flush; recovery-only discovery; create flush then generation-one checkpoint; elevation/admin preflight; and ownership-safe mount removal.
Independent approvals: final SPEC review and quality review APPROVED. The R4 temporary cleanup boundary was approved narrowly: mount removal is ordered before stop; R6 owns busy-volume keep-running/shutdown behavior.
Main-controller verification: fresh Debug exact 4/4, Debug m4-v2 3/3, Debug m3-v2 32/32; fresh Release exact 4/4 and Release m4-v2 3/3; both configuration product-boundary tests PASS. Line count 841/2500. Exact 10/10 allowlist, stage-0, prohibited/resource/callgraph scans and git diff --check PASS.
R7 follow-up: real destructive AIM/NTFS provisioning, forced-kill/reboot and VM evidence remain R7.
Exact implementation commit: d7d33c008e3fba2d11d585e9ebb0879b5326bd33 / feat(m4): provision and recover exact windows volume
```

## R5 — Persistence maintenance loop

**SPEC:** Section 10.
**PLAN:** Task R5.
**Allowed files:**

```text
src/run/run_loop.h
src/run/run_loop.cpp
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m4_run_policy_tests.cpp
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [x] One-second loop has no scheduler interface, queue or thread pool.
- [x] Time and dirty threshold equality trigger checkpoint exactly.
- [x] Journal usage below 1717986919 bytes is silent; at/above it prints one
  process-lifetime warning with the exact stopped-bundle compact command.
- [x] Every operation revalidates and flushes the exact volume first.
- [x] `NotCommitted` retries once and suppresses 60 seconds after second failure.
- [x] Maintenance-stat failure requests controlled failure shutdown, preserves
  an otherwise permitted final checkpoint and exits 1 after cleanup.
- [x] Typed `JournalCapacityExhausted` is not retried, disables later and final
  checkpoints, keeps the disk mounted until Ctrl+C, prints the exact recovery
  path and ultimately exits 1 after safe cleanup.
- [x] Reconciliation converges through existing M3 ownership.
- [x] Indeterminate commit requests controlled failure shutdown and suppresses
  final checkpoint.
- [x] `m4-v3` and affected M3 checkpoint/compaction tests pass.
- [x] Exact commit exists.

**Execution record:**

```text
Task / status: R5 / DONE
Executor / UTC timestamp: fresh gpt-5.6-terra medium implementer / 2026-08-13
Start commit / branch / end commit: f23b586ec77ae644f01aa939664b6f6af1ca16b7 / feature/m4-contracted-product / 946fd8ffecd20ac73f8c426913ea0a2e7b0fc3d3
Allowed files / actual files: R5 frozen allowlist / M4_TASKLIST.md; src/CMakeLists.txt; src/run/main.cpp; src/run/run_loop.h; src/run/run_loop.cpp; tests/CMakeLists.txt; tests/unit/m4_run_policy_tests.cpp
Dependencies and evidence: R4 is DONE at start HEAD f23b586ec77ae644f01aa939664b6f6af1ca16b7; worktree was clean before this ledger update.
RED command / observed failure / exit code: initial `cmake --build --preset debug --target myrd_m4_run_policy_tests --parallel 1` reported `unknown target 'myrd_m4_run_policy_tests'`, exit 1; after target/test creation it failed CMake generation because `src/run/run_loop.cpp` did not exist, exit 1. SPEC REDs then proved missing second reconciliation flush, missing ordinary second-failure warning, reconciliation failure treated as suppression-only, and missing committed-generation output. Quality REDs proved typed-context-only capacity misclassification, post-sleep work despite shutdown, and absent Windows argv command round-trip. Main-review RED proved status used a time captured before the checkpoint result rather than the result time.
Implementation summary: concrete one-second loop with private same-source behavior helpers, exact due/outcome policy, flush before every checkpoint/reconciliation attempt, one retry and exact 60-second suppression, typed capacity handling, Windows argv-safe compact recovery command, post-sleep shutdown check, ready line and committed-generation output. No scheduler/interface/queue/thread pool, persistent M4 state, R6 handler, lock/dismount or final-checkpoint implementation.
Focused Debug verification / exit code: main controller fresh Debug built product/run-policy/boundary and ran focused 5/5 plus `m4-v3` 1/1, exit 0. After the final result-time repair, focused 5/5 and `m4-v3` 1/1 were rerun GREEN.
Cumulative and Release verification / exit code: Debug `m3-v2` 32/32 passed in the same review round before the final result-time repair; do not treat it as rerun after that repair. Main controller fresh Release built product/boundary and ran focused 5/5 plus `m4-v3` 1/1, exit 0.
Line count / architecture and seam scans: fresh line counter reports TOTAL=1059 MAXIMUM=2500, exit 0; exact allowlist is 7/7 staged paths, stage is otherwise empty; prohibited architecture scan and `git diff --check` exit 0.
VM evidence paths and SHA-256, if applicable: not applicable; R7 is excluded.
SPEC/PLAN/TASKLIST drift audit: final independent SPEC and quality reviews APPROVED. R5 is DONE; R6/R7 remain later work.
Open risks / explicitly excluded follow-up: R6 console handler, locked final checkpoint and dismount cleanup remain deliberately unimplemented; R7 real VM lifecycle remains deferred. The current atomic is only the R5 local shutdown entry.
Commit subject / commit hash: feat(m4): schedule persistence maintenance / 946fd8ffecd20ac73f8c426913ea0a2e7b0fc3d3
```

## R6 — Safe Ctrl+C shutdown

**SPEC:** Section 11.
**PLAN:** Task R6.
**Allowed files:**

```text
src/run/windows_volume.h
src/run/windows_volume.cpp
src/run/run_loop.h
src/run/run_loop.cpp
src/run/main.cpp
tests/unit/m4_windows_volume_tests.cpp
tests/unit/m4_run_policy_tests.cpp
tests/integration/m4_product_composition_tests.cpp
M4_TASKLIST.md
```

**Acceptance:**

- [x] Console handler only stores an atomic flag.
- [x] Clean order is lock, extent check, flush, final checkpoint, dismount,
  mount removal and runtime stop.
- [x] Dismount is unreachable without a held volume lock.
- [x] Busy lock performs no cleanup mutation, prints Defender/Search guidance,
  clears the request and resumes running.
- [x] Forced termination is documented as requiring Windows reboot.
- [x] Only complete confirmed cleanup exits 0.
- [x] Debug/Release `m4-v3` pass and exact commit exists.

**Execution record:**

```text
Task / status: R6 / DONE
Start commit / branch: 72fc264c1af0e6fab5c8a73eb8893b49dac7ea2e / feature/m4-contracted-product
Initial RED: Debug locked-volume contract compilation failed for missing R6 symbols. SPEC REDs then proved each final retry needs its own flush, lock/verify failures need zero cleanup mutation and request clear, reconciliation needs one flushed retry, and indeterminate/capacity R5 state skips final checkpoint while retaining cleanup. Quality REDs proved a `Committed` outcome carrying an error cannot be success, typed capacity cannot retry, handler cleanup must not overwrite the registration error, and resumed maintenance must retain post-wait shutdown gating.
Implementation and review: exact locked-volume ownership verifies extents on the locked handle; shutdown preserves the frozen action order, logs safe cleanup failures, and only confirmed success prints the exact goodbye. Console callback performs only relaxed atomic store. Final SPEC and quality reviews APPROVED.
Fresh verification: Debug build, boundary, focused 6/6, m4-v3 1/1 and m3-v2 32/32 passed; Release build, boundary, focused 6/6 and m4-v3 1/1 passed. Allowlist 9/9, stage-0 prohibited scan and diffcheck passed; product lines 1217/2500.
Open risk / follow-up: R7 retains real destructive AIM/NTFS VM lifecycle evidence; R6 is complete.
Implementation commit: 03df02face954302f7d3618e6eacb3516eb735dc / feat(m4): shut down ram disk safely
```

## R7 — Real AIM/NTFS lifecycle

**SPEC:** Section 12.2.
**PLAN:** Task R7.
**Allowed files:**

```text
tests/e2e/Invoke-M4RunLifecycle.ps1
tools/Invoke-M4Verification.ps1
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] Host-built Release artifacts and hashes are recorded.
- [ ] Create phase provisions exact GPT/NTFS and commits file A.
- [ ] Exact PID is force-killed and Windows is rebooted.
- [ ] Recover phase never invokes provisioning and verifies A.
- [ ] Ctrl+C commits D, safely dismounts and returns inventory to baseline.
- [ ] Third run verifies D.
- [ ] Offline compact after clean shutdown advances one generation, preserves
  all hashes and changes no AIM/PhysicalDrive inventory.
- [ ] Negative matrix proves no overwrite, no letter theft, no gen-zero retry,
  no ambiguous recovery format, writer lock, driver preflight and busy refusal.
- [ ] Evidence contains no password, private key or secret.
- [ ] Final route says PASS and exact commit exists.

**Execution record:** Not started.

## R8 — Final regression and GO

**SPEC:** All sections.
**PLAN:** Task R8.
**Allowed files:**

```text
README.md
M4_TASKLIST.md
docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md
docs/superpowers/plans/2026-08-13-m4-contracted-product.md
```

SPEC/PLAN edits are allowed only for factual drift discovered during final
audit, not for adding features.

**Acceptance:**

- [ ] README contains exact command, defaults, recovery no-format guarantee,
  80%/full-journal behavior, exact offline compact recovery, busy-volume
  guidance, clean/full/forced-kill offline-compaction sequences, forced-kill
  reboot limitation, online-compaction defer link and exit codes.
- [ ] Fresh Debug full CTest passes.
- [ ] Fresh Release full CTest passes.
- [ ] Formal M0-M3 labels and all local M4 labels pass.
- [ ] R7 same-commit VM final evidence passes.
- [ ] `BUILD_TESTING=OFF` product builds and contains no test seam.
- [ ] Architecture and unfinished-marker scans are clean.
- [ ] Product line count is at most 2500.
- [ ] No requirement is deferred to M5; no M5 is planned.
- [ ] Drift audit maps every SPEC requirement and changed product file.
- [ ] Exact final commit exists and M4 status is GO.

**Execution record:** Not started.

---

## E. Final GO checklist

- [ ] R0-R8 are DONE, including the exact R1 owner-decision document commit.
- [ ] One product executable and one resident MyRamDisk process are shipped.
- [ ] Create, periodic persistence, forced-kill/reboot recovery and Ctrl+C
  shutdown pass on real AIM/NTFS.
- [ ] Recovery never formats.
- [ ] Busy volume never dismounts.
- [ ] Old M4 management architecture is absent from product source and binary.
- [ ] Product implementation is at most 2500 lines.
- [ ] Full local regression and same-commit VM evidence are fresh.
- [ ] README states every v1 limitation.
- [ ] Online compaction appears only as the explicit external defer and no v1
  placeholder API, option or scheduler hook exists.
- [ ] `src/run/run_loop.*` contains no compact/compaction path; offline compact
  is routed only by product `main`.
- [ ] M4 is declared the final milestone; no M5 tasklist or plan exists.
