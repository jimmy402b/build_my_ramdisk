# M4 VMware execution report

Date: 2026-09-05  
Commit: `dac11ca21ac9bbee6aa22e5f72f23f46cafe27ee`  
Branch: `feature/m4-single-multi-disk`  
Release SHA-256: `26058C46460A2C132D9D5EA974F8717AADA173F2422349EB2E44D435EE8205DF`  
Debug SHA-256: `BD6C5FC00110268B2175994A220CF90C1511BEAC36BC444126915D922622BE14`

This report records the real VMware observations performed during this
qualification run. The final Release retest used the committed source above
and completed VM-1 through VM-5 runtime scenarios. It is deliberately not a substitute for the canonical
`qualification.json` package: the original command captures and per-case
checkpoint hash files were not all preserved in one evidence root, so no
missing evidence is synthesized here.

## Environment

- VM: VMware `VMware20,1`, UUID `DDEC4D56-37B7-6E63-5AF8-03044F0290C1`
- Windows build: `10.0.26200`
- Guest user: `v-user`
- `phdskmnt.sys`: installed and running; service start mode `DEMAND_START`
- AIM query after cleanup: `No virtual disks.`
- Final target letters: neither `R:` nor `S:` present
- Final product processes: none

## Observed cases

| Case | Result | Evidence |
|---|---|---|
| VM-1 dual create/isolation | PASS (runtime observation) | A reached `R: generation 1`; B reached `S: generation 1` when started sequentially. Distinct NTFS Volume IDs and AIM devices were observed. Same-bundle collision returned `writer_lock.acquire`; fresh-bundle R: collision returned `requested drive letter is occupied`. |
| VM-2 single abnormal recovery | PASS (runtime observation) | After terminating the recorded process and a manual VMware power cycle, recovery returned `Ready: R: generation 31`; committed and post-checkpoint markers were present. Cleanup ended with no AIM devices. |
| VM-3 cross-instance isolation | PASS (runtime observation) | After A was terminated, B remained on `S:` with its own AIM device and process. Normal Ctrl+C of A removed `R:` while `S:` remained. |
| VM-4 dual abnormal recovery | PASS (runtime observation) | After a manual VMware power cycle, A/B recovery returned `R: generation 70` and `S: generation 71`; both committed markers were present. Both AIM devices were removed during cleanup. |
| VM-5 negative/integrity cases | PASS (runtime observation) | Occupied letter, invalid non-empty bundle, geometry mismatch, and tampered manifest all returned non-zero before creating a usable foreign volume. |

The final-release retest (2026-09-05) repeated VM-1, VM-2, VM-3, VM-4 and VM-5
with `myrd-final-release.exe`. VM-2 recovered at generation 61 and VM-4
recovered A/B at generations 37/33. The final residual check reported no
matching process, no target letters, and `No virtual disks.`.

## Local verification

- M4 focused Release CTest: **9/9 passed**.
- M4 focused Debug CTest: **9/9 passed** in the preceding verification run.
- Evidence assembler tests: **PASS**.
- Evidence verifier tests: **PASS**.
- VM multi-disk harness regression test: **PASS**.
- `git diff --check`: no whitespace errors.

The complete historical CTest suite is not a release gate for this branch; it
contains unrelated legacy M2/M3 administrator/shared-memory tests that fail in
this host environment.

## Release gate status

The persistent qualification package is at
`C:\m4-final-evidence-20260905\qualification`. It contains
`qualification.json`, VM-1 through VM-5 canonical results, the driver
pre-experiment result, artifacts and referenced bundle hashes. The offline
verifier returned the literal `M4 VM EVIDENCE PASS` with exit code 0.

The feature branch is therefore formally qualified for review. It has not been
merged to `main`; merge/release packaging remains an integration decision.
