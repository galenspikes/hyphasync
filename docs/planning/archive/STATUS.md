# hyphasync — current project status

> **⚠️ ARCHIVED — historical snapshot (2026-05-31).** Superseded by
> [../PROJECT_STATUS_AND_RECOMMENDATIONS.md](../PROJECT_STATUS_AND_RECOMMENDATIONS.md) (status)
> and the public [../../functions.md](../../functions.md) (function reference). Kept for history;
> not maintained.

**Date:** 2026-05-31  
**Build pin:** DuckDB `v1.5.2` · Fingerprint algo `v3` · Metadata schema version `3`  
**Test result:** `All tests passed (94 assertions in 1 test case)` — `make test`

---

## What works today (verified against the live build)

### Core workflow

| Function | Behavior |
|----------|----------|
| `SELECT hypha_init(conn)` | Verifies Postgres connectivity via libpq, creates local `hypha` schema, logs `OK` to event_log, prints `[hyphasync] connected · host=… · db=… · user=… · Nms` to **stderr**. Returns empty string `""`. |
| `SELECT hypha_init(conn, max_rows)` | Same, plus stores a per-table row-count guard. Tables with estimated row_count > max_rows are skipped with a `TABLE_SKIP` event log entry. |
| `SELECT hypha_init(conn, max_rows, fast_mode)` | Same, plus when `fast_mode=true` sets `synchronous_commit=off` on every Postgres connection for faster COPY throughput. |
| `SELECT hypha_base_snapshot_plan()` | Walks the local DuckDB catalog, classifies each table (EXACT/APPEND_ONLY/MUTABLE_ENTITY), computes fingerprints, populates `hypha.{object,column,table}_snapshot`. Prints 4 summary lines to **stderr**: database name + table/column/row counts, strategy breakdown (EXACT/APPEND_ONLY/MUTABLE_ENTITY counts), fingerprint algo + short commit id, and a no-write reminder. Returns empty string `""`. Per-table progress also printed to stderr. |
| `SELECT * FROM hypha_base_snapshot()` | **Table function.** Streams one row per table as it is COPYed to Postgres. Columns: `(table_name, row_count, fingerprint_strategy, duration_ms, status)`. Per-table progress on stderr. |
| `SELECT hypha_sync_plan()` | Diffs current local fingerprints against last applied snapshot. No Postgres writes. Prints an elegant summary to **stderr**: a header line with database name, changed/unchanged table counts, and the short base commit id; a counts breakdown (`new`, `dropped`, `schema changed`, `rows changed`); one indented line per changed table with aligned action label and row info (`old → new rows` or `N rows (content changed)`); and a trailing `run hypha_sync() to apply` prompt. When nothing has changed: a single `all N tables unchanged · nothing to sync` line. Returns empty string `""`. |
| `SELECT * FROM hypha_sync()` | **Table function.** Applies the diff and yields one row per changed table: `(table_name, action, rows_synced, duration_ms, status)`. Action values: `new`, `updated`, `schema_changed`, `truncate_copy`, `dropped`. Unchanged tables are counted but not yielded (they appear in the summary log). |
| `SELECT hypha_drop()` | Reads the stored default target, connects to Postgres, lists all non-system schemas (everything outside `{pg_catalog, information_schema, public, pg_toast, pg_temp_1, pg_internal}` and not matching `pg_%`), and drops each with `DROP SCHEMA IF EXISTS … CASCADE`. Safe to call twice (idempotent). `hypha_drop(true)` also drops the remote `hypha` bookkeeping schema. Returns `"dropped N schemas from postgresql://…"`. |
| `SELECT hypha_status()` | Reads local DuckDB metadata (`hypha.commit`, `hypha.object_snapshot`). No Postgres connection required. Prints one line to **stderr**: `[hyphasync] status · last sync: YYYY-MM-DD HH:MM UTC · kind: <kind> · N tables · commit: <8-char-id>`. When no sync has been run: `[hyphasync] status · no sync history — run hypha_init() then hypha_base_snapshot() first`. Returns empty string `""` in both cases. |

**Calling `SELECT hypha_base_snapshot()` or `SELECT hypha_sync()` without `* FROM` throws a helpful error** redirecting to the table-function syntax. Both scalar shims and table functions coexist in the catalog.

---

### Fingerprinting (algorithm v3)

All hashing runs in DuckDB only. Three strategies, selected per-table at snapshot-plan time:

| Strategy | Trigger condition | Hash computation | Cost |
|----------|-------------------|------------------|------|
| **EXACT** | Estimated serialized bytes < 1 MB | `sha256(string_agg(sha256(each_row_encoding), chr(10) ORDER BY row_hash))` | O(n) — fast because table is small |
| **APPEND_ONLY** | Table has a monotonic integer PK or timestamp-like column (`id`, `*_id`, `created_at`, `*_at`, `*_ts`, etc.) | `sha256(COUNT(*)::text \|\| '\|' \|\| COALESCE(MAX(pk)::text, ''))` | O(1) |
| **MUTABLE_ENTITY** | Default (all other tables) | `sha256(COUNT(*)::text \|\| '\|' \|\| COALESCE(MIN(rowid)::text, '') \|\| '\|' \|\| COALESCE(MAX(rowid)::text, ''))` | O(1) via DuckDB zone-map statistics |

`MUTABLE_ENTITY` detects inserts, deletes, and most updates. Known blind spot: an in-place update that leaves `COUNT(*)`, `MIN(rowid)`, and `MAX(rowid)` identical will not be detected (rare in DuckDB's current storage engine).

---

### Type mapping

All standard scalar types are mapped. Notable entries:

- `TIMESTAMP_S` / `TIMESTAMP_MS` / `TIMESTAMP_NS` → `timestamp without time zone` (logged as `TYPE_COERCE` event)
- `LIST(T)` / `T[]` / `STRUCT(...)` / `MAP(...)` → `text` holding canonical JSON (requires `json` extension loaded in DuckDB)
- `JSON` → `text` (lossless: preserves NUL/U+0000, which `jsonb` cannot) — first-class: exact per-row fingerprinting (tag `J`) and `::JSON`-cast COPY; requires `json` extension
- `HUGEINT` → `numeric(39,0)`, `UBIGINT` → `numeric(20,0)`
- Unsupported types → column excluded; logged as event; rest of table syncs normally

---

### Schema and identity handling

- **SafeTruncateIdent:** Postgres identifier names are truncated to 63 bytes with a collision suffix (`_01`, `_02`, …). Name truncations are logged as `NAME_TRUNCATED` events.
- **NOT NULL suppression in base snapshot DDL:** The `CREATE TABLE` on Postgres does not emit `NOT NULL` constraints (avoids COPY rejection on rows that have NULLs due to type coercion or excluded columns).
- **Varlena TOAST (SET STORAGE EXTERNAL):** After `CREATE TABLE`, all varlena columns (`text`, `bytea`, `jsonb`, `varchar`, `char`, `bit varying`) receive `ALTER COLUMN … SET STORAGE EXTERNAL` to force out-of-line storage and sidestep Postgres's 8 KB per-heap-row limit for text-heavy tables.

---

### Observability and error tracking

- **`tables_failed` / `rows_failed`:** Tracked per sync/snapshot run. If any table fails, a `WARNING: N tables failed (M rows lost)` line is printed to stderr and recorded in `hypha.sync_log`.
- **`TABLE_FAIL` events:** Every per-table failure is logged to `hypha.event_log` with the Postgres error message.
- **`TABLE_SKIP` events:** Tables exceeding `max_rows_per_table` are logged and skipped cleanly.
- **NOTICE suppression:** `SET client_min_messages = warning` is applied to Postgres connections to suppress noisy NOTICE output.
- **Real-time event_log mirroring:** A dedicated autocommit Postgres connection (`pg_log`) writes events immediately, visible even if the main transaction rolls back later.

---

### Local metadata schema (`hypha.*` in DuckDB)

| Table | Contents |
|-------|----------|
| `hypha.target` | Stored Postgres connection string (default target) |
| `hypha.commit` | Snapshot/sync commit history with `fingerprint_algo` |
| `hypha.object_snapshot` | Per-table `definition_hash`, `content_hash`, `object_fingerprint`, `pg_table_name` |
| `hypha.column_snapshot` | Per-column DuckDB→Postgres type mapping |
| `hypha.table_snapshot` | Per-table row count and `table_hash` |
| `hypha.row_hash` | Per-row `(pk_json, row_hash)` pairs for row-level diff |
| `hypha.event_log` | All operations with level/code/message/details |
| `hypha.meta` | Config: `metadata_schema_version=3`, `fingerprint_algo=v3`, `hyphasync_version` |

---

## Known issues / rough edges

### 1. Postgres 8 KB row limit for wide fixed-width tables

Tables with hundreds of integer/numeric/date columns (e.g. XBRL/FERC-style wide tables) can fail COPY with `row is too big for index` or `ERROR: row is too big`. `SET STORAGE EXTERNAL` only helps for varlena (text/bytea/jsonb) columns — it has no effect on fixed-width columns like `integer` or `numeric`.

**Mitigated in this build:** Before attempting COPY, `hyphasync` estimates the fixed-width row size by summing byte widths of all non-varlena columns. If the estimate exceeds 7,000 bytes, a `TABLE_TOO_WIDE` event is logged at level `warn` to `hypha.event_log` with a description of the estimated row size and a recommendation to split the table or convert fixed-width columns to `text`/`numeric`/`jsonb`. If COPY subsequently fails with `row is too big`, an additional actionable line is printed to stderr:

```
[hyphasync] TABLE_TOO_WIDE: table 'schema.table' row is too wide for Postgres heap.
Consider splitting or converting fixed-width columns to text/jsonb.
```

The COPY attempt still proceeds after the warning (it may succeed if Postgres's actual row overhead stays under 8 KB). Known affected table: `analysis_of_charges...` in ferc60/bacta.

### 2. `hypha_sync()` / `hypha_base_snapshot()` output schema inconsistency

`hypha_sync()` yields a row for each **changed** table; unchanged tables are counted in the summary but not returned as rows. `hypha_base_snapshot()` returns one row per table regardless. This asymmetry can surprise scripts that count output rows. Documented, not yet fixed.

---

## Test suite status

```
make test
# → All tests passed (94 assertions in 1 test case)
```

Tests cover (DuckDB-only, no Postgres required):
- Extension loading and `hypha_hello()` smoke test
- `hypha_doctor()` output format and capability flags
- Error cases: NULL/empty/unreachable conn string for `hypha_init()`
- `hypha_target_status()` error cases (unreachable, malformed, no stored target)
- Empty-table fingerprinting (MUTABLE_ENTITY SQL on zero-row table)
- UUID PKs, reserved-keyword column names, all-NULL non-PK columns
- `max_rows_per_table` QuickRowEstimate SQL pattern
- Fingerprint golden vectors: NULL, INTEGER, VARCHAR, BOOLEAN, UUID, DOUBLE, DATE, TIMESTAMP
- Multi-field row hash (chr(31) separator)
- `hypha_help()` — all functions listed, specific function lookup, unknown name
- Scalar shim error messages for `hypha_base_snapshot()` and `hypha_sync()`
- `hypha_help()` column-name coverage: `table_name`/`rows_synced` in hypha_sync; `row_count`/`fingerprint_strategy` in hypha_base_snapshot
- `hypha_help()` return-type annotation: TABLE token and canonical SELECT usage snippet
- TIMESTAMP_S/MS/NS column types accepted by DuckDB catalog (CREATE/DROP smoke test)
- `hypha_init()` return type declared as VARCHAR in the function catalog
- `hypha_drop()` error cases: no stored target (all three overloads); `hypha_help('hypha_drop')` entry
- `hypha_status()` — returns `''` (empty string) and prints to stderr; no-history line when `hypha` schema absent; `hypha_help('hypha_status')` entry

**Integration test suite** (`./test/integration/run.sh`) — all 94 tests passing (2026-05-31):
- Happy path: `hypha_init()` → `hypha_base_snapshot()` → `hypha_sync()`
- Type mapping end-to-end: all Postgres types round-trip
- Schema evolution: ADD/DROP COLUMN, type change (DROP+CREATE)
- Composite PK row diff
- Nested types (LIST/STRUCT/MAP/JSON → text, lossless canonical JSON)
- Remote metadata (`hypha.sync_log`, `hypha.object_state`)
- `max_rows_per_table` skipping real tables
- Wide tables, large tables (150 k rows), fingerprint strategy selection
- Network resilience: 10 k-row table with mid-test data change

No Docker required — runs against native Postgres 16 on port 54329.

---

## What is not yet implemented

| Feature | Notes |
|---------|-------|
| **Streaming COPY** | **Implemented (2026-05-31).** All three COPY paths (base snapshot, sync full-table, delta row-level) now stream via an in-process `pipe()` + background thread. DuckDB writes CSV to the pipe write-end via `/dev/fd/N`; the reader thread forwards chunks directly to Postgres with `PQputCopyData`. No `/tmp/*.csv` files are written. `sys/stat.h` and all `fopen`/`fread`/`fclose` calls removed. |
| **Inter-table parallelism** | All tables are processed sequentially. An N-worker pool would give 3–8× speedup on many-table databases. |
