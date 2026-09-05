# M0 V6 Twenty-cycle Evidence

Date: 2026-08-02 (Asia/Shanghai)

## Build and dependency evidence

- Release build, all 10 unit tests and the SHM integration test passed; the V2
  integration was also repeated 50 consecutive times.
- Release import audit found no forbidden AIM user-mode dependency.
- Release executable SHA-256:
  `CBC84333F1E64504EDC156A5FEA69C1CF90BC8BCE5C8C232EB642C113BD1AB85`.
- `phdskmnt.sys` SHA-256:
  `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9`.

## Real-driver result

- Harness summary:
  `reports/m0-v6/summary-20260802-005337-3234647.json`
- Independent aggregate:
  `reports/m0-v6/summary-20260802-005337-3234647-aggregate.json`
- Post-restore independent query:
  `reports/m0-v6/summary-20260802-005337-3234647-final-query.json`
- Result: 20 attempted, 20 passed.

The independent aggregate re-parsed the exact report list from the summary and
confirmed:

- 20 distinct run ids;
- 20 distinct provider sessions and object paths;
- one unchanged Release executable hash and one unchanged driver hash;
- every report passed strict counters, identity, digest, exact-removal and
  cleanup assertions;
- the summary's before/after queries and the new post-service-restore query all
  observed AIM/PhysicalDrive sets equal to the initial baseline;
- the before/after query reports and all 20 lifecycle reports retained their
  recorded immutable SHA-256 values through service restoration and aggregate
  validation;
- reopening every session mapping, Request event, Response event and Server
  mutex failed as expected;
- `phdskmnt` remained Running and `deviosvc` state/StartMode were restored.

This accepted run includes the named-object race fix. A prior post-merge run
stopped safely at 8/9 when the ninth lifecycle observed a still-open object;
the object disappeared shortly afterward. The lifecycle now polls the exact
condition for up to 15 seconds instead of weakening or removing the per-run
assertion. The failed run remains under `reports/` as diagnostic history.

Verdict: V6 PASS. This is the fresh highest-tier Task29 acceptance run.
