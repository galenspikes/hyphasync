# hyphasync — prioritized task list for next agent

**Date:** 2026-05-31  
**Context:** Read `docs/STATUS.md` first for the current state. Read `AGENTS.md` for build/test commands.

Tasks are ordered by priority: correctness and operability before performance and new features.

---

## ~~Task 4 — Streaming COPY (eliminate temp file I/O)~~ ✓ DONE (2026-05-31)

Replaced all three COPY paths (base snapshot, sync full-table, delta row-level diff) with a
`pipe()` + `std::thread` approach: DuckDB serializes CSV directly to `/dev/fd/N` (the pipe
write-end) while a background thread drains the read-end with `PQputCopyData`. No `/tmp/*.csv`
files are created. Removed `sys/stat.h`, all `fopen`/`fread`/`fclose`/`stat` calls, and the
`tmp_dir` infrastructure. `make test` passes (88 assertions). See `src/hypha_snapshot.cpp`
(`CopyChunkViaPipe` helper ~line 40).

---

## ~~Task 5 — `hypha_drop()` function~~ ✓ DONE (2026-05-31)

Implemented `SELECT hypha_drop([drop_meta BOOLEAN])`. Reads the stored default target, connects
to Postgres, queries `information_schema.schemata` for non-system schemas, and drops each with
`DROP SCHEMA IF EXISTS … CASCADE`. `hypha_drop(true)` also drops the remote `hypha` bookkeeping
schema. Logs dropped schema names to stderr. Returns `"dropped N schemas from postgresql://…"`.
`make test` passes (92 assertions). See `src/hypha_snapshot.cpp` (`RunHyphaDrop`).

---

## ~~Task 6 — `hypha_status()` function~~ ✓ DONE (2026-05-31)

Implemented `SELECT hypha_status()`. Reads local DuckDB metadata (`hypha.commit`,
`hypha.object_snapshot`) — no Postgres connection required. Returns a one-line summary:
`last_sync=<ISO8601>  kind=<kind>  tables=<N>  commit_id=<id>  message=<msg>`.
Returns `"no sync history found — …"` when no sync has been run.
`make test` passes (94 assertions). See `src/hypha_snapshot.cpp` (`RunHyphaStatus`).

---

## ~~Task 7 — Fix N+1 column INSERTs~~ ✓ DONE (2026-05-31)

Per-column `INSERT INTO hypha.column_snapshot` statements batched into multi-value INSERTs
capped at 500 rows per statement. A table with 4,233 columns now emits 9 INSERT statements
instead of 4,233. `make test` passes (94 assertions). See `src/hypha_snapshot.cpp`
(`COL_BATCH_SIZE = 500` loop ~line 550).

---

## ~~Task 8 — Integration test cleanup~~ ✓ DONE (2026-05-31)

Started at 2/94 failing; both fixed:

1. **"init: wrong password throws"** — Local dev Postgres uses trust authentication, so any
   password is accepted for the `hypha` user. Changed `PG_BAD_PASS` (wrong password for existing
   user) to `PG_BAD_CRED` (non-existent role), which is rejected even under trust auth. Updated
   the test description accordingly. Comment in `run.sh` documents the trust-auth reason.

2. **"large table: object_snapshot entry present"** — Test queried
   `hypha.object_snapshot WHERE table_name=…` but the column is named `object_name`. Fixed
   the column reference in `test/integration/run.sh`.

No code changes were needed — both failures were stale test assumptions.
`./test/integration/run.sh` now reports **ALL 94 TESTS PASSED**. No Docker required (native
Postgres 16 on port 54329).
