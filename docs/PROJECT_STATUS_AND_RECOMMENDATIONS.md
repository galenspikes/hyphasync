# hyphasync — project status & recommendations

**Last reviewed:** 2026-06-07  
**Version:** `0.2.0` · **DuckDB pin:** `v1.5.2` · **Fingerprint:** `v3` · **Metadata schema:** `3`

This document is the canonical planning reference for where hyphasync stands, what remains risky, and how to get from *experimental* to *production-ready*. It supersedes the stale sections of [ROADMAP.md](ROADMAP.md), [STATUS.md](STATUS.md), and [NEXT.md](NEXT.md) (those files are kept for history but should not be edited independently).

---

## Executive summary

hyphasync is a **working vertical slice**: DuckDB → Postgres sync via SHA-256 fingerprint diffs, with base snapshot, incremental sync, schema evolution, composite PK row diff, nested types → `jsonb`, and observability on both sides. The core pipeline is implemented across focused modules in `src/` and covered by SQL + integration tests that run in CI.

The project is **not yet validated at scale**. Benchmark runs against real-world XBRL and multi-hundred-table catalogs showed silent table drops and memory blow-ups; several correctness fixes have landed since those runs, but **the sample-DB harness has not been re-baselined in CI**. Until `./scripts/test-sample-dbs.sh` passes with documented fidelity on frankenstein + at least one large FERC DB, treat every release as experimental.

**The path to “perfect” is:** prove correctness on real workloads → close remaining fidelity/operability gaps → optimize throughput → ship release engineering (signed binaries, stable client story).

---

## What is done (verified in code & tests)

### Core sync pipeline

| Stage | Entry point | Module(s) |
|-------|-------------|-------------|
| Init + target verify | `hypha_init()` | `hypha_metadata.cpp`, `hypha_postgres.cpp`, `hyphasync_extension.cpp` |
| Read-only probe | `hypha_target_status()` | `hyphasync_extension.cpp` |
| Catalog walk + fingerprint | `hypha_base_snapshot_plan()` | `hypha_snapshot_plan.cpp`, `hypha_fingerprint.cpp` |
| First push | `hypha_base_snapshot()` | `hypha_snapshot_base_snapshot.cpp`, `hypha_snapshot_pg.cpp` |
| Diff preview | `hypha_sync_plan()` | `hypha_snapshot_sync.cpp`, `hypha_snapshot_diff.cpp` |
| Incremental apply | `hypha_sync()` | `hypha_snapshot_sync.cpp`, `hypha_snapshot_diff.cpp` |
| Teardown / status | `hypha_drop()`, `hypha_status()` | `hypha_snapshot.cpp` |

Shared infrastructure: `hypha_snapshot_common.cpp` (`CopyChunkViaPipe`, type mapping), `hypha_snapshot_internal.hpp`.

### Fingerprinting v3

Three per-table strategies (EXACT / APPEND_ONLY / MUTABLE_ENTITY) with algorithm-version enforcement across syncs. Spec: [fingerprinting.md](fingerprinting.md).

### Correctness fixes already shipped (since May benchmark)

| Fix | Where | Notes |
|-----|-------|-------|
| Streaming COPY (no temp files) | `CopyChunkViaPipe()` in `hypha_snapshot_common.cpp` | Used by base snapshot, sync, and delta COPY |
| NOT NULL suppressed on base snapshot DDL | `BuildCreateTableDDL(..., suppress_not_null=true)` in `hypha_snapshot_base_snapshot.cpp` | Sync path still emits NOT NULL — see gaps |
| TIMESTAMP_S/MS/NS mapping | `hypha_snapshot_common.cpp`, `hypha_fingerprint.cpp` | Logged as `TYPE_COERCE` |
| `tables_failed` / `rows_failed` tracking | `hypha_snapshot_base_snapshot.cpp`, remote `hypha.sync_log` | WARNING on stderr when > 0 |
| TABLE_TOO_WIDE pre-check | `hypha_snapshot_base_snapshot.cpp`, `hypha_snapshot_sync.cpp` | Fixed-width row size estimate before COPY |
| JSON extension bundled | `extension_config.cmake`, auto-load in `hyphasync_extension.cpp` | LIST/STRUCT/MAP fingerprinting + COPY |
| Keyless append fast path | `ApplyKeylessAppendDiff()` in `hypha_snapshot_diff.cpp` | No-PK tables with append-only row growth COPY deltas instead of TRUNCATE+COPY |
| Batched column_snapshot INSERTs | `hypha_snapshot_plan.cpp` | 500-row batches |
| Remote event_log mirroring | Dedicated autocommit `pg_log` connection | Survives main txn rollback |

### Test coverage

| Layer | Location | Scope |
|-------|----------|-------|
| SQL unit tests | `test/sql/hyphasync.test` | 94 assertions — doctor, init errors, fingerprint golden vectors, help, drop/status shims |
| Integration tests | `test/integration/run.sh` | 12 sections / 94 checks — init, type mapping, base snapshot, sync, schema evolution, composite PK, nested types, 150k-row table, max_rows guard, resilience |
| Smoke test | `scripts/smoke-test.sh` | Fast E2E: init → base snapshot → sync; runs on **macOS CI** |
| Fingerprint perf gate | `scripts/benchmark-fingerprint.sh` | 100k-row hash smoke; runs in CI with `continue-on-error: true` |
| Real-workload harness | `scripts/test-sample-dbs.sh` | **Not in CI** — needs local `testdata/` (git-ignored) + Postgres 54329 |

### CI (`.github/workflows/ci.yml`)

Two jobs on every push/PR:

- **ubuntu-latest:** checkout + submodules → ccache-backed `make release` → `make format-check` → `make test` → fingerprint benchmark (non-blocking) → `./test/integration/run.sh --no-build`
- **macos-latest:** build → `make test` → `./scripts/smoke-test.sh` → integration tests (Postgres 16 on port 54329)

Distribution builds (`MainDistributionPipeline.yml`) are **manual-only** via workflow_dispatch.

---

## Gaps and risks

### P0 — Correctness & validation (blockers)

1. **Sample-DB fidelity unverified post-fixes.** [ROADMAP-benchmark.md](ROADMAP-benchmark.md) documented ferc714 at ~32% row landing and frankenstein missing 4 tables. Fixes 0a–0c from that doc are largely implemented, but `testdata/results/` is empty in-repo and CI does not run `./scripts/test-sample-dbs.sh`. **Risk: silent TABLE_FAIL regressions on real schemas.**

2. **Sync DDL still enforces NOT NULL.** Base snapshot passes `suppress_not_null=true`; `hypha_snapshot_sync.cpp` calls `BuildCreateTableDDL(...)` without suppression on schema-changed tables. Sparse XBRL data can still abort COPY on sync after schema drift.

3. **MUTABLE_ENTITY blind spot (documented, unfixed).** In-place updates that preserve COUNT/MIN(rowid)/MAX(rowid) are invisible. Acceptable for many workloads; dangerous if users assume cryptographic completeness.

4. **No-PK tables still fall back to TRUNCATE+COPY.** The keyless append fast path (`ApplyKeylessAppendDiff`) handles append-only growth; updates/deletes on keyless tables still require full TRUNCATE+COPY.

5. **Unsupported types silently drop columns.** ENUM, custom extension types, etc. map to `(unsupported: …)` and are excluded. Tables where *all* columns are unsupported are skipped entirely (`TABLE_SKIP` / no supported columns).

### P1 — Scale & operability

6. **No `memory_limit` during plan phase.** Benchmark showed ferc1 (993 MB file) hitting ~4 GB RSS. DuckDB buffer pool is uncapped during the catalog walk in `hypha_snapshot_plan.cpp`. Design in ROADMAP-benchmark §3.1; not implemented.

7. **Fixed `COPY_CHUNK_ROWS = 100000`.** Wrong primitive for wide/blob-heavy schemas (can allocate ~1 GB per chunk). Full design in [dynamic-chunk-sizing.md](dynamic-chunk-sizing.md); not implemented.

8. **Sequential per-table processing.** All tables run one-at-a-time. Benchmark showed 292-table FERC DBs spending most time on BEGIN/DROP/CREATE/COPY/COMMIT round trips, not COPY throughput. No worker pool.

9. **Two-phase plan-then-copy memory pattern.** `RunBaseSnapshotPlan` walks the entire catalog before any COPY starts. Interleaving plan + copy per table would bound RSS for wide catalogs (design in ROADMAP-benchmark §3.2).

10. **OFFSET pagination for non-integer PKs.** Last chunks of large composite/non-integer PK tables degrade to O(n²) scan cost during row_hash and delta COPY.

11. **Output asymmetry.** `hypha_base_snapshot()` yields one row per table; `hypha_sync()` yields only changed tables. Scripts counting result rows need special handling.

### P2 — Release & ecosystem

12. **Unsigned extension.** Built `.duckdb_extension` is not registry-signed. External DuckDB requires `--unsigned` or `SET allow_unsigned_extensions=true`.

13. **No stable R/Python client.** [r.md](r.md) documents calling the repo-built CLI; CRAN `{duckdb}` LOAD is explicitly unsupported.

14. **Distribution pipeline disabled on push.** Cross-platform binaries require manual workflow dispatch.

15. **Benchmark CI is non-blocking.** Fingerprint perf gate can regress without failing the build.

16. **Windows MinGW libpq patch** (`vcpkg_ports/libpq/windows/mingw-pthread.patch`) exists but is lightly exercised compared to Linux/macOS CI.

---

## Prioritized recommendations

### Must-haves (before calling it production-ready)

| # | Item | Rationale | Touch points |
|---|------|-----------|--------------|
| M1 | **Re-run and baseline sample-DB harness** | Prove fidelity post-fixes; establish comparable JSONL history in `testdata/results/` | `scripts/test-sample-dbs.sh`, frankenstein + tpch-sf1 + one FERC DB |
| M2 | **Add sample-DB smoke to CI (nightly or weekly)** | Catch TABLE_FAIL regressions on real schemas | New workflow or scheduled job; download TPC-H only for PR-sized runs |
| M3 | **Suppress NOT NULL on sync DDL** | Parity with base snapshot; fixes XBRL COPY aborts on schema-changed tables | `hypha_snapshot_sync.cpp` → `BuildCreateTableDDL(..., true)` |
| M4 | **Set configurable `memory_limit` before plan** | Cap RSS on 200+ table catalogs | `hypha_snapshot_plan.cpp` or `hypha_init` option |
| M5 | **Implement dynamic chunk sizing** | Prevent OOM on wide/blob tables; reduce round-trips on narrow tables | `hypha_snapshot_common.cpp`, design in [dynamic-chunk-sizing.md](dynamic-chunk-sizing.md) |
| M6 | **Fidelity summary in CLI output** | Operators must not dig through `event_log` to learn 68% of rows are missing | stderr summary + optional aggregate row on `hypha_base_snapshot()` completion |
| M7 | **Make fingerprint benchmark blocking** | Remove `continue-on-error: true` once baseline is stable on CI runners | `.github/workflows/ci.yml` |
| M8 | **Integration test: sync after sparse NULL data** | Lock NOT NULL suppression behavior | `test/integration/run.sh` new section |

### Should-haves (quality bar for “1.0”)

| # | Item | Expected impact |
|---|------|-----------------|
| S1 | Inter-table worker pool (N=4 default) | 3–8× on many-table DBs (ferc2-class) |
| S2 | Interleave plan + COPY per table | ferc1-class RSS from ~4 GB → < 1 GB |
| S3 | Distinct event code for all-unsupported-schema tables | `UNSUPPORTED_SCHEMA` vs generic `TABLE_FAIL` |
| S4 | Per-table chunk timing in `event_log` | Enables tuning before adaptive chunk sizing |
| S5 | Keyset pagination for UUID/text PKs where possible | Faster large-table delta sync |
| S6 | Document + test `fast_mode` data-loss window prominently | Operators must understand `synchronous_commit=off` |
| S7 | `hypha_set` / `hypha_get` for copy_chunk_mb, memory_limit, workers | Runtime tuning without recompile |

### Nice-to-haves (after quality bar)

| # | Item | Notes |
|---|------|-------|
| N1 | Watermark-column optimization for APPEND_ONLY tables | Only if benchmarks show fingerprint is the bottleneck (currently < 1%) |
| N2 | ENUM / UNION type mapping | frankenstein completeness |
| N3 | Progress reporting (% tables, ETA) on long syncs | UX; TableFunction already streams per-table rows |
| N4 | R/processx CLI wrapper package | Conservative client story per ROADMAP clients section |
| N5 | DuckDB extension registry publish + signing | Requires distribution pipeline automation |
| N6 | Adaptive chunk sizing | Defer until per-chunk metrics exist (see dynamic-chunk-sizing.md verdict) |
| N7 | Object lineage / Postgres COMMENT ON | Metadata polish |
| N8 | Postgres read replica lag awareness | Detect stale targets before sync |

### Non-goals (unchanged)

- CDC / WAL / logical replication
- Postgres → DuckDB
- Multi-target or non-Postgres databases
- Streaming replication

---

## Suggested milestones

### Phase A — Prove it (current → 4 weeks)

**Exit criteria:** sample-DB harness green on frankenstein (100% tables), tpch-sf10 (100% rows), ferc714 (>95% rows); results committed or archived; CI integration test covers NOT NULL sparse data.

```
Week 1: M1 re-baseline + document failures in testdata/results/
Week 2: M3, M8 (NOT NULL parity + test)
Week 3: M4, M5 (memory_limit + dynamic chunks)
Week 4: M6, M7 (fidelity summary + blocking benchmark)
```

### Phase B — Scale it (4–8 weeks)

**Exit criteria:** tpch-sf10 base snapshot ≥ 15 MB/sec on CI runner; ferc2 (292 tables) ≥ 3× faster vs Phase A baseline; RSS on ferc1 ≤ 2 GB.

- S1 inter-table worker pool
- S2 interleaved plan+copy
- S4 per-table chunk timing
- S7 runtime config API

### Phase C — Ship it (8–12 weeks)

**Exit criteria:** signed binaries for linux_amd64 + osx_arm64 + windows_amd64; README “stable” banner; R wrapper alpha.

- Re-enable distribution pipeline on tag push
- N4 R CLI wrapper
- N5 registry publish (optional)
- Version bump to 1.0.0

---

## Concrete next steps (this week)

1. **Run the woodchipper locally:**
   ```sh
   ./scripts/download-testdata.sh
   ./scripts/setup-postgres-test.sh   # if needed
   ./scripts/test-sample-dbs.sh --db frankenstein.duckdb
   ./scripts/test-sample-dbs.sh --db tpch-sf1.duckdb
   ```
   Record results in `testdata/results/` and note any TABLE_FAIL / TABLE_SKIP codes.

2. **Fix sync NOT NULL (M3)** — one-line change in `hypha_snapshot_sync.cpp`; add integration test with nullable-in-practice column.

3. **Add `SET memory_limit` at start of `RunBaseSnapshotPlan` (M4)** — default `2GB`, overridable via future `hypha_init` parameter.

4. **Triage ROADMAP-benchmark fidelity gaps** against current event_log from step 1; close or re-open each root cause with evidence.

5. **Remove `continue-on-error` from benchmark CI step** once a stable timing baseline is recorded (target: < 60s on ubuntu-latest).

---

## Module map (for contributors)

```
hyphasync_extension.cpp   — scalar functions, extension load, json auto-load
hypha_metadata.cpp        — local hypha.* schema, commits, event_log
hypha_postgres.cpp        — libpq connection pool, probe, URL parsing
hypha_fingerprint.cpp     — SHA-256 row/table hashing, strategy SQL
hypha_snapshot_plan.cpp   — hypha_base_snapshot_plan()
hypha_snapshot_base_snapshot.cpp — hypha_base_snapshot() TableFunction
hypha_snapshot_sync.cpp   — hypha_sync_plan(), hypha_sync() TableFunction
hypha_snapshot_diff.cpp   — fingerprint diff, row-level DELETE+INSERT
hypha_snapshot_pg.cpp     — DDL, remote hypha metadata, COPY helpers
hypha_snapshot_common.cpp — CopyChunkViaPipe, DuckTypeToPostgres
hypha_snapshot.cpp        — registration, hypha_drop, hypha_status
```

---

## CI checklist (maintainers)

| Check | Command | Blocking? |
|-------|---------|-----------|
| Release build | `CC=gcc CXX=g++ make release` | Yes |
| Format | `make format-check` | Yes |
| SQL tests | `CC=gcc CXX=g++ make test` | Yes |
| Integration | `./test/integration/run.sh --no-build` | Yes |
| Fingerprint bench | `BINARY=./build/release/duckdb ./scripts/benchmark-fingerprint.sh 60` | No (should become Yes) |
| Smoke test | `./scripts/smoke-test.sh` | macOS CI only |
| Sample DBs | `./scripts/test-sample-dbs.sh` | No (should become scheduled Yes) |
| Full cross-build | Main Distribution Pipeline (manual) | Manual |

---

## Related docs

| Doc | Purpose |
|-----|---------|
| [functions.md](functions.md) | SQL surface reference |
| [fingerprinting.md](fingerprinting.md) | v3 hashing spec |
| [ROADMAP-benchmark.md](ROADMAP-benchmark.md) | May 2026 benchmark postmortem (historical; verify against current code) |
| [dynamic-chunk-sizing.md](dynamic-chunk-sizing.md) | Chunk sizing design (not implemented) |
| [upgrading-duckdb.md](upgrading-duckdb.md) | Submodule bump guide for users |
| [UPDATING.md](UPDATING.md) | Maintainer submodule procedure |
| [r.md](r.md) | R / CRAN guidance |

---

*Generated from codebase review 2026-06-07. Re-review after each Phase exit criteria milestone.*
