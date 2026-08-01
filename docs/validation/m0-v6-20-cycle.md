# M0 V6 Twenty-cycle Evidence

Date: 2026-08-01 (Asia/Shanghai)

## Build and dependency evidence

- Release build, all 10 unit tests and the SHM integration test passed; the V2
  integration was also repeated 50 consecutive times.
- Release import audit found no forbidden AIM user-mode dependency.
- Release executable SHA-256:
  `0CC6B83AD7D099C328C683E5736DEB75CF4F224D2FF5C0B1E8CCC4D68AABDB4D`.
- `phdskmnt.sys` SHA-256:
  `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9`.

## Real-driver result

- Harness summary:
  `reports/m0-v6/summary-20260801-230009-6480435.json`
- Independent aggregate:
  `reports/m0-v6/summary-20260801-230009-6480435-aggregate.json`
- Post-restore independent query:
  `reports/m0-v6/summary-20260801-230009-6480435-final-query.json`
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
- reopening every session mapping, Request event, Response event and Server
  mutex failed as expected;
- `phdskmnt` remained Running and `deviosvc` state/StartMode were restored.

Verdict: V6 PASS. This is the fresh highest-tier Task29 acceptance run.
