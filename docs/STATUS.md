# hyphasync — status & roadmap

The single living status document for hyphasync. User-facing usage lives in the
[README](../README.md) and [docs/functions.md](functions.md); this file is the
engineering truth: what works, what is broken, and the path to a public release.

**Last reviewed:** 2026-06-05
**Version:** `0.2.0` · **DuckDB build pin:** `v1.5.2` · **Fingerprint:** `v3` · **Metadata schema:** `3`
**Status:** experimental — the sync pipeline is complete end-to-end, but quality on
large/real workloads is not yet proven and the extension is not yet released.

---

## The wedge

DuckDB's built-in `postgres` extension copies a table to Postgres in one shot. hyphasync
exists for the **repeated** case: re-publishing a DuckDB database to Postgres on a schedule,
copying only what changed since the last sync (SHA-256 snapshot diff, no CDC/WAL). Every
roadmap decision below should make the *incremental diff* cheaper or safer; anything that
doesn't is probably out of scope.

---

## What works today (verified against the live build)

Full vertical slice, experimental but complete:

- Workflow: `hypha_init` → `hypha_base_snapshot_plan` → `hypha_base_snapshot` →
  `hypha_sync_plan` → `hypha_sync`, plus `hypha_status`, `hypha_verify`, `hypha_drop`,
  `hypha_doctor`, `hypha_target_status`, `hypha_help`.
- Fingerprinting v3 — EXACT / APPEND_ONLY / MUTABLE_ENTITY strategy classifier; row-level
  diff for single and composite PKs; append-only fast path and TRUNCATE+COPY fallback for
  keyless tables.
- Schema evolution (ADD/DROP COLUMN without DROP+CREATE), nested types (LIST/STRUCT/MAP →
  `jsonb`), full scalar type mapping.
- Streaming COPY via in-process `pipe()` + reader thread — no temp files.
- Remote `hypha.sync_log` / `hypha.object_state` on Postgres; local `hypha.*` metadata
  schema; `hypha.event_log` observability with real-time mirroring to Postgres.
- `tables_failed` / `rows_failed` tracking with WARNING output.

See the [README](../README.md) and [docs/functions.md](functions.md) for the full SQL
surface, type-mapping table, and per-function semantics.

### Test status

- `make test` — unit tests, DuckDB-only (no Postgres). Last green at 94 assertions.
- `./test/integration/run.sh` — 94 integration tests against Postgres 16 on port 54329
  (native preferred, Docker Compose fallback). Last green 2026-05-31.
- CI (`.github/workflows/ci.yml`) builds + runs both on every push, Linux x86-64 and macOS.

---

## Known issues & critical gaps

Ordered by how much they threaten a public release. The top two are the focus of the
correctness milestone below.

### 1. Silent table-skip on COPY failure (correctness — top priority)

When a table's COPY throws (Postgres error, type conversion failure, oversized row), the
code rolls back that table's savepoint, logs `TABLE_FAIL` to `hypha.event_log`, and
continues. The table then simply **does not exist** on Postgres, and the only signal is a
log line. The benchmark run measured real fidelity loss from this: ferc714-xbrl landed
~32% of rows and frankenstein ~74%, with tables silently missing. See
[docs/benchmarks.md](benchmarks.md) §1.4.

A sync tool that can silently leave the target incomplete is not releasable. The fix
(post-COPY target verification + loud, non-silent failure) is the correctness milestone.

### 2. MUTABLE_ENTITY in-place-update blind spot (correctness)

The O(1) `MUTABLE_ENTITY` strategy hashes `COUNT(*) + MIN/MAX(rowid)`. An in-place update
that leaves those identical is not detected, so `hypha_sync()` skips the table and Postgres
goes silently stale. Mitigations exist but are **opt-in** (`exact_verify=true`, or periodic
`hypha_verify()`), so the unsafe behavior is the default. The correctness milestone flips
this to safe-by-default and makes the strategy mix visible on every run.

### 3. Per-table transaction overhead dominates wide catalogs (performance)

Each table costs ~6 synchronous Postgres round trips (BEGIN/DROP/CREATE/ALTER/COPY/COMMIT).
Databases with hundreds of tables (ferc1: 255, ferc2: 292) spend most of their time on
scaffolding, not COPY. Addressed by the performance milestone (inter-table parallelism).

### 4. OFFSET pagination for non-integer PKs (performance)

Tables without a single-column integer PK paginate with `LIMIT/OFFSET`, which is O(n) in
offset depth. Keyset pagination already exists for integer PKs; extending it to composite
keys is part of the performance milestone.

### 5. Wide fixed-width tables can exceed Postgres's 8 KB row limit

`SET STORAGE EXTERNAL` only helps varlena columns. Tables with hundreds of integer/numeric
columns can fail COPY with "row is too big". hyphasync pre-warns (`TABLE_TOO_WIDE`) and logs
the failure, but cannot fix it — the table must be split or columns widened to text/jsonb.

### 6. Memory expansion on schema-heavy databases

ferc1 used ~3.9 GB RSS for a 993 MB source (DuckDB buffer pool + growing
`hypha.column_snapshot`). DuckDB's memory limit is uncapped by default. Not yet addressed.

### 7. Plaintext credentials in the DuckDB file

Connection strings (including passwords) are stored in `hypha.target` inside the DuckDB
file. No env-var / `.pgpass` indirection yet. Worth closing before public release.

### 8. Output goes to stderr; functions return `""`

The rich summaries print to stderr while the scalar functions return empty strings, so
anything scripting hyphasync (cron, Airflow) cannot consume the result programmatically.
Addressed by the selective-sync / structured-output milestone.

---

## Roadmap to public release

Sequenced so each milestone unblocks the next. Nothing ships publicly before the
correctness milestone is green.

### M1 — Positioning & scope ✅ (this pass)

Wedge-forward README, single living status doc (this file), Windows dropped (Linux + macOS
only; `_WIN32` source paths and Windows build archs removed).

### M2 — Correctness (the release gate)

- **Post-COPY target verification:** assert Postgres `COUNT(*)` matches source per table;
  re-fingerprint small tables on the PG side; emit a non-silent `TARGET_DRIFT` event and
  fail loudly. Closes gap #1.
- **Safe-by-default fingerprinting:** make `exact_verify` the default; require opt-in to the
  O(1) fallible path; surface the strategy mix on every `hypha_sync()`. Closes gap #2.
- **Differential test harness:** generate random tables (types, NULLs, unicode, edge
  numerics, single/composite/no PK), sync, compare every row against Postgres, thousands of
  seeds in CI.

### M3 — Selective sync & structured output

- Include/exclude table globs and column exclusion.
- Machine-readable return from `hypha_sync()` / `hypha_sync_plan()` (JSON or a queryable
  `hypha.last_sync`). Closes gap #8.

### M4 — Performance (gated on M2 baselines)

- Inter-table parallel COPY (worker pool; thread-safe `event_log`). Addresses gap #3.
- Keyset pagination for composite/non-integer PKs. Addresses gap #4.

### M5 — Release engineering

- Re-enable `MainDistributionPipeline.yml`, extension signing, registry submission.
- Semver + `CHANGELOG.md`, `CONTRIBUTING.md`, issue/PR templates, a 90-second quickstart.
- CI matrix matching the supported-platform claim.

Open questions still being answered with real data: how long a 1B-row table takes to
fingerprint + COPY; where libpq/Postgres limits bite (row size, identifier length, memory);
whether incremental sync is actually cheaper than re-push for typical change patterns.

---

## Non-goals

- No CDC, DuckDB WAL inspection, or logical transaction logs.
- No streaming replication.
- No Postgres → DuckDB sync (one direction only).
- No multi-target or non-Postgres databases.
- No Windows.

---

## Clients — conservative stance

**Supported path today:** the DuckDB binary built from this repo
(`./build/release/duckdb`), with hyphasync linked in. Shell scripts and SQL files call that
binary directly.

**R / Python / other bindings:** query DuckDB files freely with `{duckdb}` / `duckdb` — that
does not require hyphasync. Running *sync* from inside CRAN `{duckdb}` is not a supported
workflow yet: extension binaries are tied to a specific DuckDB engine version, and CRAN
updates on its own schedule. The planned R path is a conservative wrapper that shells out to
the known-good CLI (`system2` / `processx`) rather than `LOAD`-ing a loose `.duckdb_extension`
into whatever DuckDB version `{duckdb}` shipped. See [docs/r.md](r.md).

---

## Build pins & maintainer notes

| Dependency | Pin |
|------------|-----|
| DuckDB | `v1.5.2` (submodule + CI) |
| extension-ci-tools | `@v1.5-variegata` |
| libpq | system / Homebrew (vcpkg port for distribution builds) |
| Postgres (tests) | 16; native port 54329 (preferred), Docker Compose fallback |

We bump DuckDB when we need a fix or when preparing a release — not on every upstream patch.
DuckDB 2.0 (Fall 2026) is on the radar; no action until the correctness milestone is done.
Submodule bump procedure: [docs/UPDATING.md](UPDATING.md). User-facing upgrade story:
[docs/upgrading-duckdb.md](upgrading-duckdb.md).

### Bump checklist

- Submodule + extension-ci-tools + CI workflow version.
- `make release`, `make test`, `./test/integration/run.sh`.
- Sample-DB harness on large testdata.
- Update pins in this file and [upgrading-duckdb.md](upgrading-duckdb.md).

---

## Testdata & stress infrastructure

| Asset | Script | Role |
|-------|--------|------|
| Public samples | `scripts/download-testdata.sh` | TPC-H, FERC/PUDL |
| `frankenstein.duckdb` | `scripts/build-frankenstein.sh` | Multi-schema type coverage |
| `cheminformatics.duckdb` | `scripts/build-cheminformatics.sh` | Domain-scale tox/chem |
| `hts-pipeline.duckdb` | `scripts/build-hts.sh` | HTS well/DR firehose |
| Results | `testdata/results/*.jsonl` | Benchmark history |

`testdata/` is git-ignored; create it locally or symlink it to your own storage. The
repeatable stress run is `./scripts/test-sample-dbs.sh`. Empirical findings from the last
full run live in [docs/benchmarks.md](benchmarks.md).

---

## References

- [README](../README.md) — user-facing overview and quick start
- [docs/functions.md](functions.md) — full SQL surface
- [docs/fingerprinting.md](fingerprinting.md) — v3 spec
- [docs/benchmarks.md](benchmarks.md) — empirical benchmark data and lessons
- [docs/upgrading-duckdb.md](upgrading-duckdb.md) — user upgrade guide
</content>
</invoke>
