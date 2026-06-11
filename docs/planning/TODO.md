# hyphasync — TODO

A living checklist of actionable work. Item IDs (M#, S#, N#) and full rationale live in
[PROJECT_STATUS_AND_RECOMMENDATIONS.md](PROJECT_STATUS_AND_RECOMMENDATIONS.md) — this file
is just the running state so nothing gets lost between sessions.

**Last updated:** 2026-06-11

> Maintained on the `develop` branch under `docs/planning/`. The public `main` branch does
> not carry this directory — see [planning/README.md](README.md).

---

## ✅ Done (branch `claude/repo-review-brainstorm-mC0km`)

- **Positioning** — README leads with the incremental-diff wedge + "vs DuckDB's `postgres`
  extension" comparison.
- **Windows dropped** — `_WIN32` source paths removed, `windows_*` archs excluded from the
  distribution pipeline, marked a non-goal. (`vcpkg_ports/libpq/windows/*` left in place —
  they patch libpq on all platforms.)
- **M3** — suppress NOT NULL on both `hypha_sync()` table-recreate paths (parity with base
  snapshot). Fixes silent COPY aborts on sparse/coerced data after schema drift.
- **M6** — `hypha_base_snapshot()` prints a fidelity summary on every run (landed-vs-lost,
  loud `WARNING` on partial pushes).
- **M8** — integration `section 16` locks the NOT NULL parity behavior.

Verified locally: `make test` (104 assertions) + integration suite (122 tests) green.

---

## Next up — Phase A (the release gate)

Ordered by value. Phase A is "prove fidelity on real workloads"; it is the bar before any
public release.

- [ ] **M1 — re-baseline the sample-DB harness** *(highest value, needs test data)*
  Run `./scripts/test-sample-dbs.sh` on frankenstein + tpch-sf1 + one FERC DB; commit/record
  results in `testdata/results/`. Confirms the M3/M6 fixes actually closed the benchmark
  fidelity gaps (ferc714 was ~32%, frankenstein missing 4 tables). Blocked only on downloading
  the git-ignored `testdata/`.
- [ ] **M4 — configurable `memory_limit` before the plan phase**
  `SET memory_limit` at the start of `RunBaseSnapshotPlan` (default ~2GB, overridable via a
  `hypha_init` option). Caps the ~4GB RSS seen on ferc1. Small, self-contained; RSS benefit
  only provable on a large DB.
- [ ] **M2 — sample-DB smoke in CI** (scheduled/nightly; TPC-H only for PR-sized runs).
- [ ] **M5 — dynamic chunk sizing** (design in [dynamic-chunk-sizing.md](dynamic-chunk-sizing.md)).
- [ ] **M7 — make the fingerprint benchmark CI step blocking** once a stable baseline exists.

---

## Backlog — Phase B (scale) / Phase C (ship)

Pull detail from the canonical doc when starting any of these.

- [ ] **S1** — inter-table parallel COPY (worker pool; thread-safe `event_log`).
- [ ] **S2** — interleave plan + COPY per table to bound RSS on wide catalogs.
- [ ] **S5** — keyset pagination for composite / non-integer PKs (kill the OFFSET O(n²) cliff).
- [ ] **S7** — runtime config API (`hypha_set`/`hypha_get`: copy_chunk_mb, memory_limit, workers).
- [ ] **Phase C** — tagged-release binary pipeline is in place (`release.yml`); remaining:
  R/processx CLI wrapper, version bump to 1.0.0, CHANGELOG/CONTRIBUTING. (Extension signing /
  community-extensions registry publish is a **non-goal** — the downloadable binary is enough.)

---

## Open correctness questions (not yet scheduled)

These came out of the repo review and are worth a decision before 1.0:

- [ ] **MUTABLE_ENTITY safe-by-default** — the O(1) strategy can silently miss in-place
  updates; mitigations (`exact_verify`, `hypha_verify()`) are opt-in. Consider flipping the
  default to safe, or at least surfacing the fast/fallible strategy mix on every `hypha_sync()`.
- [ ] **Post-COPY target verification** — assert Postgres `COUNT(*)` matches source per table
  (and re-fingerprint small tables on the PG side); emit `TARGET_DRIFT` on mismatch. Closes the
  "silent divergence after a dropped COPY" gap that M6 only *reports*, doesn't *prevent*.
- [ ] **Plaintext credentials** — connection strings (with passwords) are stored in
  `hypha.target` inside the DuckDB file. Add env-var / `.pgpass` indirection.
- [ ] **Structured output** — `hypha_sync()` summaries go to stderr while the function returns
  `""`; add a machine-readable return or queryable `hypha.last_sync` for automation.
</content>
