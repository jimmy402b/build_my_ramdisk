# M4 Negative VM Scope Design

## Status and scope

This design repairs only the real-VM `Negative` evidence route in
`tests/e2e/Invoke-M4RunLifecycle.ps1`, its independent Final verifier and the
local tests that preserve the volume-ambiguity defense. It does not change
normal create/recover/compact behavior or the approved VM identity.

Formal10 proved the product recovery-discovery repair through CreateKill,
RecoverShutdown and OfflineCompact. Negative then failed before starting a
product process because the operator fixture manifest was absent. Review of
the manifest contract showed that its `ambiguous_recovery` case required an
artificial one-disk/two-volume topology. That topology is a useful defensive
code boundary, but it is not a normal product lifecycle and would require
risky VM repartitioning solely for acceptance evidence.

The approved scope therefore separates realistic VM checks from synthetic
structural boundaries.

## Real-VM negative cases

The real VM runs these eight serial cases:

1. `unknown_nonempty` leaves an unknown non-empty directory untouched;
2. `size_mismatch` rejects recovery with the wrong requested size;
3. `occupied_letter` does not steal an existing drive letter;
4. `gen0` does not reprovision an incomplete generation-zero bundle;
5. `writer_lock` rejects a second writer;
6. `missing_driver` fails before disk mutation;
7. `busy_volume` keeps a busy volume/device until the holder closes;
8. `compact_lock` rejects offline compact while `myrd run` owns the bundle.

Commands, arguments and refusal patterns remain frozen in the runner. Every
fixture path stays strictly below `<bundle>.r7-negative`, except that the
occupied-letter oracle reads the already-existing approved baseline volume.
Each case writes immutable process, refusal, inventory and nonmutation
evidence. Final requires exactly these eight files plus the aggregate Negative
phase file.

`ambiguous_recovery` is removed from the VM manifest, runner case list and
Final VM evidence list. No formal VM run creates a second partition, formats a
candidate volume or modifies the fixed 8 GiB RAW Disk2 merely to synthesize
this condition.

## Local volume-ambiguity defense

One virtual disk can contain more than one partition/volume even though the
normal product creates exactly one GPT/NTFS volume. The product must continue
to accept exactly one matching volume and reject zero or multiple matching
volumes.

The existing discovery code remains fail-closed. A small pure classification
seam, used by the production result check, classifies match counts as:

- zero: missing/refused;
- one: exact/accepted;
- two or more: ambiguous/refused.

Same-translation-unit unit tests cover 0, 1, 2 and a larger count. A composition
source contract confirms production discovery uses that seam after complete
enumeration. This retains the defense without a product test hook or a
destructive VM fixture.

## Data-only fixture manifest

The runner continues to consume
`<StateFile>.negative-fixtures.json` with schema
`myrd.m4.negative-fixtures.v1`. Its `active_cases` may contain only the eight
approved names. Case entries contain only constrained paths, one-letter drive
letters, approved sizes and the existing busy-volume holder filename. They
cannot provide commands, executable options, refusal regexes, source commit,
artifact identity or evidence paths.

Unknown, duplicate, null or malformed cases fail before a product process
starts. Existing per-case evidence is loaded only when schema, case, source,
artifact, bundle root, refusal contract and both oracle sides are valid.

## Resumable missing-driver execution

`missing_driver` requires stopped/absent `phdskmnt`, while the other
driver-backed cases require it running. The existing per-case resume mechanism
is retained and the operator uses two controlled passes:

1. Stop only the approved `phdskmnt` service, prove AIM/R are absent, and run a
   manifest whose `active_cases` contains only `missing_driver`. The case
   evidence is immutable. The exact final `Negative remaining cases: ...`
   exit 77 is an expected partial boundary, not aggregate PASS.
2. Restore the same service, re-prove the clean AIM/R/disk baseline, and run a
   manifest listing the other seven cases. The runner validates the existing
   `missing_driver` evidence, executes the remainder serially, writes
   `m4-lifecycle-Negative.v1.json`, and exits 0.

Any other exit 77, evidence mismatch, process timeout, inventory drift or
service restoration failure is a real RED. Final accepts only the completed
aggregate and all eight immutable case files.

## Fixture preparation and safety

Preparation occurs only after OfflineCompact returns to the clean baseline.
Valid case bundles are copies of the same formal run's compacted bundle. The
generation-zero fixture comes from a separately proved incomplete fixture and
is copied read-only into the derived negative root. The occupied-letter case
uses an existing approved baseline letter and records its GUID before and
after the refusal.

Preparation never partitions or formats a physical disk. System disks and the
fixed 8 GiB RAW Disk2 are never modified. Every temporary helper and manifest
path is exact and current-owned. Driver stop/start operations are bounded,
verify the exact `phdskmnt` service and must return the same clean baseline.

Every formal root is one-use. A real phase RED stops downstream execution and
that root is never reused.

## TDD and Final verification

Local RED tests first prove that the old runner/Final still require nine cases
and that production discovery lacks an explicit tested 0/1/many match-count
contract. Minimal GREEN changes only:

- the runner's frozen Negative list, contract validator and synthetic
  negative selftest;
- Final's frozen case list, per-case checks and mutation selftest;
- the volume match-count classification seam and same-TU unit test;
- the composition source contract and R7 ledger.

Required local verification is:

- PowerShell 5.1 parsing for runner and verifier;
- focused negative-contract, Final, Windows-volume and composition tests;
- sequential Debug and Release `m4-v2` and `m4-v3` labels;
- product line counter, diff check and exact allowlist audit;
- independent SPEC and quality review.

After commit, a fresh formal11 root must pass four no-ack probes, CreateKill,
one password-authorized hard reset, RecoverShutdown, OfflineCompact, the two
controlled Negative passes and Final. R7 remains DOING until those immutable
same-commit files pass independent evidence review.
