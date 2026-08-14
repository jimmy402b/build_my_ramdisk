# M4 Negative Fixture Evidence Design

## Status and scope

This design repairs only the real-VM `Negative` evidence route in
`tests/e2e/Invoke-M4RunLifecycle.ps1` and its independent Final verifier. It
does not change product code, normal create/recover/compact behavior, the
approved VM identity, or the frozen nine negative cases.

Formal10 proved the product recovery-discovery repair through CreateKill,
RecoverShutdown and OfflineCompact. Negative then failed before starting a
product process because the operator fixture manifest was absent. Inspection
also found that the existing `ambiguous_recovery` contract was not satisfiable
on the required clean baseline: it required two candidate drive letters to be
mounted before the product started, while the phase-wide inventory gate
required the same empty-AIM baseline captured by CreateKill.

## Chosen approach

The harness continues to consume a data-only
`myrd.m4.negative-fixtures.v1` manifest. Commands, arguments and refusal
patterns remain frozen in the runner. The repair changes ambiguity evidence
from a pre-launch live-mount assertion to an immutable fixture assertion:

1. The operator prepares an ambiguous recovery bundle below
   `<bundle>.r7-negative` whose persisted disk contains two volumes and known
   sentinel contents.
2. The manifest records exactly two candidate volume GUIDs, exactly two
   sentinel paths, the corresponding two sentinel SHA-256 values, and the
   expected bundle-tree digest.
3. Before launching the refusal command, the runner validates all manifest
   fields and requires the current bundle-tree digest to equal the declared
   digest.
4. The current product must exit 1 with the exact existing
   `find_recovered_volume: volume identity is ambiguous` refusal.
5. After exit, the runner recomputes the bundle-tree digest and requires it to
   equal both the pre-launch digest and the declared digest. Stable Windows
   inventory must remain equal to the CreateKill baseline.
6. Per-case immutable evidence records the declared candidate identities and
   sentinel hashes in both oracle sides, plus the measured before/after bundle
   digest. It never claims the candidate volumes were mounted before launch.

The exact ambiguity refusal proves that the product observed multiple matching
volumes. The unchanged complete bundle-tree digest proves that the rejected
attempt did not persist a partition, format, sentinel or metadata change.
Together these facts replace the contradictory pre-launch mount requirement
without adding a product test seam.

## Bundle-tree digest

The existing textual `TreeHash` output is retained for other cases. Ambiguity
uses a new deterministic SHA-256 digest over that exact canonical tree text.
The digest is uppercase 64-hex and is computed by a private PowerShell helper
using UTF-8 without BOM. The manifest and Final verifier accept only that
syntax. An absent or changed file, relative path, length or file SHA changes
the digest.

## Manifest validation

The `ambiguous_recovery` entry retains its existing fields and adds:

- `sentinel_sha256`: exactly two uppercase or lowercase 64-hex strings in the
  same order as `sentinels` and `candidate_volume_guids`;
- `bundle_tree_sha256`: one 64-hex digest of the prepared fixture tree.

The validator continues to require two distinct candidate letters, GUIDs and
sentinel paths. Paths must remain under the derived negative root or under the
paired candidate drive as already specified. Null, missing, duplicate,
malformed or wrong-count values fail before a product process starts.

No manifest field can supply a command, executable option, refusal regex,
source commit, artifact identity or evidence path.

## Resumable missing-driver execution

The nine negative cases remain ordered and immutable. The existing per-case
evidence resume mechanism is retained because `missing_driver` requires a
stopped/absent `phdskmnt` service while the other driver-backed cases require
it running.

The operator runs Negative in two controlled passes:

1. With `phdskmnt` stopped, a manifest whose `active_cases` contains only
   `missing_driver` writes that immutable case evidence. The exact final
   `Negative remaining cases: ...` exit 77 is an expected partial boundary,
   not aggregate PASS.
2. After restoring the same approved driver service and re-proving the clean
   AIM/R/disk baseline, the manifest lists the other eight cases. The runner
   validates the existing `missing_driver` evidence, executes the remaining
   cases serially, writes aggregate `m4-lifecycle-Negative.v1.json`, and exits
   0.

Any other exit 77, evidence mismatch, process timeout, inventory drift or
service restoration failure is a real RED. Final accepts only the completed
aggregate plus all nine immutable per-case files.

## Final verification

`tools/Invoke-M4Verification.ps1` strengthens the ambiguity oracle. It must
require:

- exact ambiguity refusal and exit 1;
- equal before/after candidate GUID arrays;
- equal before/after sentinel SHA-256 arrays, with exactly two valid hashes;
- measured before/after bundle-tree digests equal to one another and to the
  declared manifest digest recorded in evidence;
- `no_format_indicators=true`;
- unchanged stable inventory.

The synthetic Final selftest gains valid ambiguity digest data and mutations
for a changed post-run digest and malformed sentinel hash.

## Fixture preparation and safety

Fixture preparation is an operator action performed only after the main
OfflineCompact phase returns to a clean AIM/R baseline. Every fixture path is
strictly below the fresh formal root's derived negative directory. Existing
valid bundles are copied from the same formal run. The ambiguity bundle is
prepared only through an exact current-owned AIM device whose DeviceId, size,
sector geometry and PhysicalDrive mapping are proved before any partition
operation; system disks and the fixed 8 GiB RAW Disk2 are never modified.

Preparation records the two candidate GUIDs, sentinel SHA-256 values and
bundle-tree digest before clean shutdown returns AIM/R to baseline. The
generation-zero fixture is copied from a separately proved incomplete fixture
and is never opened for mutation by the preparation route.

Every formal root is one-use. A real phase RED stops downstream execution and
that root is never reused.

## TDD and acceptance

Local RED tests must first prove that the old validator accepts an ambiguity
entry without the new hashes, the old oracle depends on live pre-launch drive
letters, and Final accepts a mutated bundle digest. Minimal GREEN then changes
only the runner, Final verifier, their focused tests/CMake source contracts and
the R7 ledger.

Required local verification is:

- PowerShell 5.1 parsing for runner and verifier;
- focused negative-contract, native, Final and composition tests;
- sequential Debug and Release `m4-v2` and `m4-v3` labels;
- product line counter, diff check and exact allowlist audit;
- independent SPEC and quality review.

After commit, a fresh formal11 root must pass four no-ack probes, CreateKill,
one password-authorized hard reset, RecoverShutdown, OfflineCompact, the two
controlled Negative passes and Final. R7 remains DOING until those immutable
same-commit files pass independent evidence review.
