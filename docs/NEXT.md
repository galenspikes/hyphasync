# hyphasync — prioritized task list for next agent

**Date:** 2026-06-07  
**Context:** Read [PROJECT_STATUS_AND_RECOMMENDATIONS.md](PROJECT_STATUS_AND_RECOMMENDATIONS.md) first. Read [STATUS.md](STATUS.md) for function-level detail. Read [AGENTS.md](../AGENTS.md) for build/test commands.

Tasks are ordered by priority: correctness and operability before performance and new features.

---

## Phase A — Quality bar (active)

See [PROJECT_STATUS_AND_RECOMMENDATIONS.md § Must-haves](PROJECT_STATUS_AND_RECOMMENDATIONS.md) (M1–M8). Top items:

1. **M1** — Re-run `./scripts/test-sample-dbs.sh` on frankenstein + tpch-sf1; record results in `testdata/results/`
2. **M3** — Suppress NOT NULL on sync DDL (`hypha_snapshot_sync.cpp`)
3. **M4** — Set `memory_limit` before plan phase
4. **M6** — Fidelity summary on stderr when any table fails

---

## Phase B — Scale (after Phase A)

See [PROJECT_STATUS_AND_RECOMMENDATIONS.md § Should-haves](PROJECT_STATUS_AND_RECOMMENDATIONS.md) (S1–S7). Top items:

1. **S1** — Inter-table worker pool (N=4 default)
2. **S2** — Interleave plan + COPY per table
3. **S5** — Dynamic chunk sizing per [dynamic-chunk-sizing.md](dynamic-chunk-sizing.md)

---

## Completed (2026-05-31)

### ~~Task 4 — Streaming COPY~~ ✓ DONE

`CopyChunkViaPipe()` in `src/hypha_snapshot_common.cpp` — pipe + background thread, no temp CSV.

### ~~Task 5 — `hypha_drop()`~~ ✓ DONE

`src/hypha_snapshot.cpp`

### ~~Task 6 — `hypha_status()`~~ ✓ DONE

`src/hypha_snapshot.cpp`

### ~~Task 7 — Fix N+1 column INSERTs~~ ✓ DONE

Batched column_snapshot INSERTs (`COL_BATCH_SIZE = 500`) in `hypha_snapshot_plan.cpp`

### ~~Task 8 — Integration test cleanup~~ ✓ DONE

All 94 integration tests passing
