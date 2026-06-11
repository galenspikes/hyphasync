# hyphasync roadmap

> **⚠️ ARCHIVED — historical.** Superseded by the live
> [../PROJECT_STATUS_AND_RECOMMENDATIONS.md](../PROJECT_STATUS_AND_RECOMMENDATIONS.md) (canonical
> status + roadmap) and [../TODO.md](../TODO.md). Kept for history; not maintained.

**Status: experimental.** The sync pipeline is implemented end-to-end; quality validation on real workloads comes before new features or release packaging.

**Last reviewed:** 2026-06-07  
**Version:** `0.2.0` · **DuckDB pin:** `v1.5.2` · **Fingerprint:** `v3`

---

## Canonical planning doc

**→ [PROJECT_STATUS_AND_RECOMMENDATIONS.md](../PROJECT_STATUS_AND_RECOMMENDATIONS.md)** — current status, gaps, prioritized must-haves / nice-to-haves, milestones, and concrete next steps. Read this first.

Historical detail:

- [STATUS.md](STATUS.md) — point-in-time snapshot (2026-05-31)
- [NEXT.md](NEXT.md) — agent task backlog
- [ROADMAP-benchmark.md](ROADMAP-benchmark.md) — May 2026 benchmark postmortem

---

## Current focus (one line)

Prove correctness on real sample databases → close scale/operability gaps → ship release engineering.

---

## Phases (summary)

| Phase | Goal | Exit criteria |
|-------|------|---------------|
| **A — Prove it** | Re-baseline fidelity, NOT NULL parity, memory/chunk fixes | sample-DB harness green on frankenstein + tpch-sf10 + ferc714 |
| **B — Scale it** | Worker pool, interleaved plan+copy, runtime config | 3× faster on many-table DBs; bounded RSS |
| **C — Ship it** | Signed binaries, stable client story | Distribution pipeline on tag; 1.0.0 |

Full task breakdown: [PROJECT_STATUS_AND_RECOMMENDATIONS.md](../PROJECT_STATUS_AND_RECOMMENDATIONS.md).

---

## Non-goals

- CDC / WAL / streaming replication
- Postgres → DuckDB
- Multi-target or non-Postgres databases

---

## Clients

**Supported today:** `./build/release/duckdb` built from this repo (hyphasync linked in).

**Not supported:** `LOAD hyphasync` from CRAN `{duckdb}` or other mismatched DuckDB versions. When R integration matters, plan is a CLI wrapper — see [r.md](../../r.md).

---

## Testdata & stress infrastructure

| Asset | Script | Role |
|-------|--------|------|
| Public samples | `scripts/download-testdata.sh` | TPC-H, FERC/PUDL |
| `frankenstein.duckdb` | `scripts/build-frankenstein.sh` | Multi-schema type coverage |
| `cheminformatics.duckdb` | `scripts/build-cheminformatics.sh` | Domain-scale tox/chem |
| `hts-pipeline.duckdb` | `scripts/build-hts.sh` | HTS well/DR firehose |
| Results | `testdata/results/*.jsonl` | Benchmark history |

`testdata/` is git-ignored; create it locally or symlink it to your own external storage.

---

## Build pins

| Dependency | Pin |
|------------|-----|
| DuckDB | `v1.5.2` (submodule + CI) |
| extension-ci-tools | `@v1.5-variegata` |
| libpq | system / Homebrew / vcpkg overlay |
| Postgres (tests) | 16; port 54329 (native preferred, Docker fallback) |

Bump procedure: [UPDATING.md](../../UPDATING.md). User guide: [upgrading-duckdb.md](../../upgrading-duckdb.md).

---

## References

- [PROJECT_STATUS_AND_RECOMMENDATIONS.md](../PROJECT_STATUS_AND_RECOMMENDATIONS.md) — **start here**
- [functions.md](../../functions.md) — SQL surface
- [fingerprinting.md](../../fingerprinting.md) — v3 spec
- [upgrading-duckdb.md](../../upgrading-duckdb.md) — user upgrade guide
