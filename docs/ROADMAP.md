# hyphasync roadmap

**Status: experimental.** The sync pipeline is implemented end-to-end, but we have not yet established acceptable quality on real workloads (large tables, wide schemas, long-running syncs, failure recovery). Release engineering and multi-client packaging come **after** that bar is met.

**Last reviewed:** 2026-05-31  
**Version:** `0.2.0` · **DuckDB build pin:** `v1.5.2` · **Fingerprint:** `v3`

---

## Current focus — prove it works

Before new features or chasing upstream DuckDB releases:

| Priority | What “done” looks like |
|----------|------------------------|
| **Correctness** | Integration tests green; sample-DB harness passes on frankenstein + at least one multi-GB DB; row counts and types match on Postgres |
| **Scale** | Baseline timings recorded in `testdata/results/` for TPC-H, cheminformatics, HTS-scale tables; failures documented not silent |
| **Failure modes** | Skipped tables, partial sync, connection loss, and schema edge cases produce clear `hypha.event_log` entries |
| **Operability** | `./scripts/test-sample-dbs.sh` is the repeatable “woodchipper”; results are comparable run-to-run |

Open questions we are still answering with real data:

- How long does a 1B-row table take to fingerprint + COPY?
- Where do we hit Postgres or libpq limits (row size, identifier length, memory)?
- Is incremental sync actually cheaper than re-push for our typical change patterns?

---

## Shipped (0.2.0)

Experimental but complete as a **vertical slice**:

- `hypha_init` → `hypha_base_snapshot_plan` → `hypha_base_snapshot` → `hypha_sync_plan` → `hypha_sync`
- Fingerprinting v3 (rowid-statistics table_hash; v2 column encoding retained for row-level diff), row-level diff (PK tables), schema evolution (ADD/DROP), nested types → jsonb
- Remote `hypha.sync_log` / `hypha.object_state` on Postgres
- Test harness: `make test`, `./test/integration/run.sh`, `./scripts/test-sample-dbs.sh`

---

## Next (after quality bar)

Work we want but **not before** the focus items above are satisfied:

| Item | Why it waits |
|------|--------------|
| Watermark-column optimization | Needs proof full-table hash is the bottleneck |
| Parallel table COPY | Needs baseline to measure improvement |
| Progress reporting on long syncs | UX; useless if sync is wrong |
| Fingerprint migration UX polish | Edge case until we have real rebaseline stories |
| Object lineage / comments | Nice metadata; not core sync |

### Non-goals

- CDC / WAL / streaming replication
- Postgres → DuckDB
- Multi-target or non-Postgres databases

---

## Clients — conservative stance

**Supported path today:** the DuckDB binary built from this repo (`./build/release/duckdb`), with hyphasync linked in. Shell scripts and SQL files call that binary directly.

**R, Python, and other bindings:** query DuckDB files freely with `{duckdb}` / `duckdb` — that does not require hyphasync. **Running sync from inside CRAN `{duckdb}` is not a supported workflow yet.** Extension binaries are tied to a specific DuckDB engine version; CRAN updates on its own schedule. We do not want R users managing version pairs.

When R integration matters, the plan is a **conservative wrapper** (call the known-good CLI from R via `system2` / `processx`) rather than `LOAD` of a loose `.duckdb_extension` into whatever DuckDB version `{duckdb}` shipped today. Until that wrapper exists and is tested, use the CLI.

See [docs/upgrading-duckdb.md](upgrading-duckdb.md) for what happens when *you* upgrade DuckDB — written for when that becomes relevant, not something to worry about now.

---

## Testdata & stress infrastructure

| Asset | Script | Role |
|-------|--------|------|
| Public samples | `scripts/download-testdata.sh` | TPC-H, FERC/PUDL |
| `frankenstein.duckdb` | `scripts/build-frankenstein.sh` | Multi-schema type coverage |
| `cheminformatics.duckdb` | `scripts/build-cheminformatics.sh` | Domain-scale tox/chem |
| `hts-pipeline.duckdb` | `scripts/build-hts.sh` | HTS well/DR firehose |
| Results | `testdata/results/*.jsonl` | Benchmark history |

`testdata/` may symlink to external storage (e.g. `/Volumes/alpha/hyphasync-testdata`).

---

## Maintainer notes

For **us**, not end users. Submodule bump procedure: [docs/UPDATING.md](UPDATING.md). User-facing upgrade story: [docs/upgrading-duckdb.md](upgrading-duckdb.md).

### Build pins (internal)

| Dependency | Pin |
|------------|-----|
| DuckDB | `v1.5.2` (submodule + CI) |
| extension-ci-tools | `@v1.5-variegata` |
| libpq | system / Homebrew |
| Postgres (tests) | 16; native port 54329 (preferred), Docker Compose fallback |

We bump DuckDB when we need a fix or when preparing a release — not on every upstream patch. DuckDB 2.0 (Fall 2026) is on the radar; no action until quality work is done.

### Bump checklist (when we decide to)

- Submodule + extension-ci-tools + CI workflow version
- `make release`, `make test`, `./test/integration/run.sh`
- Sample-DB harness on large testdata
- Update pins in this file and [upgrading-duckdb.md](upgrading-duckdb.md)

---

## References

- [docs/functions.md](functions.md) — SQL surface
- [docs/fingerprinting.md](fingerprinting.md) — v3 spec
- [docs/upgrading-duckdb.md](upgrading-duckdb.md) — user upgrade guide (when relevant)
