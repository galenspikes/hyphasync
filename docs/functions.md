# hyphasync SQL function reference

Most hyphasync functions return `VARCHAR` and emit multi-line `key=value` reports parseable with `string_split_regex`. `hypha_base_snapshot()` is a **table function** that returns one row per table. Side effects and observability are recorded in `hypha.event_log`.

---

## `hypha_hello()`

```sql
SELECT hypha_hello();
-- hyphasync extension loaded
```

**Purpose:** Confirms the extension is loaded. Returns a fixed string. No side effects, no metadata required.

**Use case:** Smoke-test that `LOAD hyphasync` succeeded before calling anything else.

---

## `hypha_doctor()`

```sql
SELECT hypha_doctor();
```

**Purpose:** Returns a diagnostic snapshot of the running extension, the local DuckDB identity, and the capability inventory. Useful as a first call after loading the extension and before filing bug reports.

**Report fields:**

| Field | Example | Notes |
|-------|---------|-------|
| `hyphasync_version` | `0.2.0` | Release tag |
| `duckdb_version` | `v1.5.2` | DuckDB library version |
| `local_database` | `mydb` | `current_database()` — sanity-check you're attached to the right file |
| `local_database_path` | `/data/mydb.duckdb` | Absolute path; `:memory:` for in-memory databases |
| `metadata_initialized` | `true` / `false` | Whether `hypha_init()` has been called |
| `metadata_schema_version` | `2` | Internal schema layout version |
| `fingerprint_algo` | `v3` | Active hashing algorithm |
| `capability_*` | `available` | One line per function confirming it is wired up |
| `nested_types` | `LIST/STRUCT/MAP → jsonb (requires json extension)` | Summary of compound type support |
| `schema_evolution` | `ADD/DROP COLUMN without DROP+CREATE when possible` | Schema change behaviour |
| `remote_metadata` | `hypha.sync_log and hypha.object_state written after each push` | Bookkeeping on target |
| `row_level_diff` | `targeted DELETE+INSERT for single and composite PKs` | Row sync strategy |
| `note` | usage hint | Quick-start reminder |

**No side effects.** Safe to call at any time, even before `hypha_init()`.

---

## `hypha_init(conn VARCHAR [, max_rows BIGINT [, fast_mode BOOLEAN [, exact_verify BOOLEAN]]])`

```sql
-- 1-arg: safe defaults (no row limit, fast_mode=false, exact_verify=false)
SELECT hypha_init('postgresql://user:pass@host:5432/dbname');

-- 2-arg: skip tables with more than N rows
SELECT hypha_init('postgresql://...', 500000);

-- 3-arg: enable fast_mode (synchronous_commit=off) for faster pushes
SELECT hypha_init('postgresql://...', 0, true);

-- 4-arg: enable exact_verify (full per-row EXACT fingerprinting for every table)
SELECT hypha_init('postgresql://...', 0, false, true);
```

**Purpose:** Registers a Postgres target and initializes the local `hypha.*` metadata schema. Always verifies the connection is reachable before writing anything — a failed init leaves the DuckDB file completely unchanged.

**Arguments:**

| Argument | Type | Required | Notes |
|----------|------|----------|-------|
| `conn` | `VARCHAR` | Yes | libpq connection string or URL. NULL and empty string throw. |
| `max_rows` | `BIGINT` | No | If > 0, tables with more rows than this limit are skipped during snapshot. Default 0 (unlimited). |
| `fast_mode` | `BOOLEAN` | No | If true, sets `synchronous_commit=off` on every Postgres connection. Faster but risks data loss on crash. Safe for read-replica use. Default false. |
| `exact_verify` | `BOOLEAN` | No | If true, forces the full per-row `EXACT` fingerprint strategy for every table, closing the `MUTABLE_ENTITY` in-place-update blind spot at an O(n) scan cost per snapshot/sync. Default false. See `hypha_verify()` for an on-demand alternative. |

**What it does:**
1. Validates the connection string syntax
2. Opens a TCP connection to Postgres (5-second timeout) to confirm reachability
3. Creates the local `hypha` schema and all metadata tables (`hypha.target`, `hypha.commit`, etc.)
4. Upserts the target as `target_name='default'` — re-running is safe and updates the stored connection string
5. Seeds `hypha.meta` with the current version and fingerprint algorithm
6. Logs an `init / OK` event to `hypha.event_log`

**Return value:** Single-line summary string, e.g.:
```
hyphasync metadata initialized (target=default); connected to database 'mydb' as 'myuser' in 12ms
```

**Throws on:**
- NULL or empty connection string
- Postgres unreachable (connection refused, wrong port)
- Bad credentials (authentication failure)

**No Postgres writes.** Init only writes to the local DuckDB file.

**Use cases:**
- First-time setup before any sync operations
- Updating the stored Postgres URL (`hypha_init` with a new URL upserts the target)
- CI pre-check: confirm connection string is valid before running a pipeline

---

## `hypha_target_status(conn VARCHAR)`

```sql
SELECT hypha_target_status(NULL);          -- uses stored default target
SELECT hypha_target_status('postgresql://...'); -- explicit URL, not stored
```

**Purpose:** Read-only Postgres health probe. Measures round-trip latency, confirms server identity, and reports what hyphasync bookkeeping exists on the remote side. Never writes anything.

**Arguments:**

| Argument | Type | Required | Notes |
|----------|------|----------|-------|
| `conn` | `VARCHAR` | No | NULL uses the stored default target from `hypha.target`. Explicit URL overrides. |

**Report fields:**

| Field | Values | Notes |
|-------|--------|-------|
| `status` | `ok` / `degraded` | `ok` = all probes succeeded; `degraded` = connected but some sub-queries failed |
| `target_name` | `default` / `(explicit)` | Which target was probed |
| `latency_ms` | integer | TCP + one round-trip to `SELECT version()` |
| `postgres_version` | string | Full `version()` output from Postgres |
| `database` | string | `current_database()` on the remote |
| `user` | string | `current_user` on the remote |
| `remote_hypha_schema` | `true` / `false` / `unknown` | Whether the `hypha` schema exists on Postgres |
| `remote_hypha_table_count` | integer | How many tables are in the remote `hypha` schema |
| `remote_hypha_sync_log` | `true` / `false` | Whether `hypha.sync_log` exists (created by first push) |
| `remote_hypha_object_state` | `true` / `false` | Whether `hypha.object_state` exists |
| `note` | string | Confirmation that no remote writes were performed |

**Throws on:** unreachable host, bad credentials, NULL with no stored default target.

**Use cases:**
- Pre-flight check before a push or sync
- Confirm credentials after a Postgres password rotation
- After a push: check `remote_hypha_sync_log=true` to confirm bookkeeping was written

---

## `hypha_base_snapshot_plan()`

```sql
SELECT hypha_base_snapshot_plan();
```

**Purpose:** Walks the local DuckDB catalog and captures a full snapshot into local `hypha.*` metadata tables — without connecting to Postgres. Computes SHA-256 fingerprints for every table.

**What it does:**
1. Automatically installs and loads the `json` extension (needed for LIST/STRUCT/MAP fingerprinting)
2. Enumerates all non-internal, non-temporary tables in the current database (excluding the `hypha` schema itself)
3. For each table: captures columns into `hypha.column_snapshot`, computes `table_hash` + `definition_hash` + `object_fingerprint`, captures per-row hashes into `hypha.row_hash` (for PK tables)
4. Detects primary key columns (single and composite) for row-level diff
5. Records the entire operation as a `hypha.commit` with `kind='base_snapshot_plan'`, `status='completed'`

**Report fields:**

| Field | Example | Notes |
|-------|---------|-------|
| `commit_id` | UUID | Identifies this snapshot; referenced by push and sync |
| `status` | `completed` | Always `completed` unless an exception is thrown |
| `database` | `mydb` | DuckDB database name |
| `schemas_scanned` | `2` | Number of user schemas walked |
| `tables_captured` | `15` | Tables for which column metadata was recorded |
| `tables_hashed` | `15` | Tables that got complete fingerprints |
| `columns_captured` | `87` | Total column rows written to `hypha.column_snapshot` |
| `total_rows` | `140000` | Sum of row counts across all tables |
| `hashes_computed` | `true` / `partial` / `false` | Whether all tables were fingerprinted |
| `fingerprint_algo` | `v3` | Algorithm used |
| `note` | string | Confirms no remote writes |

**event_log codes:**
- `OK` — all tables fingerprinted
- `HASH_SKIP` — one or more tables could not be fingerprinted (unsupported type, missing json extension if offline); table is still captured with NULL hashes
- `JSON_EXT_MISSING` — json extension unavailable and nested columns are present (auto-install attempted but failed)

**No Postgres writes.** Safe to call multiple times — each call creates a new commit row without affecting prior commits.

**Use cases:**
- Inspect type mapping before committing to a push: `SELECT * FROM hypha.column_snapshot WHERE table_name = 'my_table'`
- Dry-run: check `tables_hashed` and `hashes_computed` before pushing
- Diff investigation: run plan twice, compare `commit_id` values

---

## `hypha_base_snapshot()`

```sql
-- Returns a table — one row per synced table.
SELECT * FROM hypha_base_snapshot();

-- Aggregate example: check for any errors.
SELECT status, COUNT(*) FROM hypha_base_snapshot() GROUP BY status;

-- Filter to failed tables only.
SELECT table_name, status FROM hypha_base_snapshot() WHERE status LIKE 'error:%';
```

**Purpose:** Runs a fresh snapshot plan, then pushes the entire local database to the stored Postgres target. Creates Postgres schemas and tables, streams all data via `COPY FROM STDIN`. **Re-running drops and re-copies every table** — this is a full re-push, not an incremental sync.

> **This is a table function** — always use `SELECT * FROM hypha_base_snapshot()`. Calling `SELECT hypha_base_snapshot()` (scalar syntax) throws a migration error.

**What it does:**
1. Calls `hypha_base_snapshot_plan()` internally to capture the current state
2. Connects to the stored default Postgres target
3. Pre-flight: checks for unmanaged tables in target schemas (`SCHEMA_OWNERSHIP_WARNING`)
4. Opens a single Postgres transaction covering all schemas
5. Per table (in a savepoint): `DROP TABLE IF EXISTS`, `CREATE TABLE`, `ALTER COLUMN SET STORAGE EXTERNAL` for varlena columns, `COPY` data from DuckDB → temp CSV → `COPY FROM STDIN`
6. After the main transaction commits: writes `hypha.sync_log` and `hypha.object_state` to Postgres (best-effort)
7. Marks the DuckDB commit as `status='applied'`

**Output schema (one row per table):**

| Column | Type | Notes |
|--------|------|-------|
| `table_name` | `VARCHAR` | `schema.table` label |
| `row_count` | `BIGINT` | Rows copied (0 on error or skip) |
| `fingerprint_strategy` | `VARCHAR` | Strategy used: `MUTABLE_ENTITY`, `APPEND_ONLY`, `EXACT`, etc. |
| `duration_ms` | `DOUBLE` | Time to process this table in milliseconds |
| `status` | `VARCHAR` | `'ok'`, `'error: <msg>'`, or `'skipped: <reason>'` |

**`status` values:**
- `'ok'` — table successfully pushed
- `'error: <msg>'` — table failed; remaining tables still attempt (per-table isolation); check `hypha.event_log`
- `'skipped: row_count=N > max_rows_per_table=M'` — table exceeded the row-count guard set via `hypha_init`
- `'skipped: no supported columns'` — all columns have unsupported types (e.g., all `(unsupported: ...)`)
- `'error: connection lost'` — Postgres connection dropped and could not be re-established

**Postgres schema naming:** `{sanitized_db_name}_{sanitized_duckdb_schema}`, e.g. `mydb_main`, `mydb_analytics`.

**event_log codes:**
- `OK` — push completed
- `TABLE_SKIP` — individual table failed (row-too-big, type error, etc.); rest of tables continue
- `SCHEMA_OWNERSHIP_WARNING` — target schema contains tables not previously managed by this hyphasync instance
- `REMOTE_META_FAIL` — bookkeeping write to `hypha.sync_log` / `hypha.object_state` failed (sync data was still applied)

**Throws on:** no initialized metadata, no stored connection string, Postgres unreachable.

**Use cases:**
- Initial population of a Postgres replica from a DuckDB file
- Hard reset: force Postgres into exact sync with the current DuckDB state
- After a major schema refactor that would be complex to evolve incrementally

---

## `hypha_sync_plan()`

```sql
SELECT hypha_sync_plan();
```

**Purpose:** Computes an incremental diff between the current local state and the last applied snapshot, without making any Postgres writes. Shows exactly what `hypha_sync()` would do.

**What it does:**
1. Calls `hypha_base_snapshot_plan()` internally to capture the current local state (creates a new `hypha.commit`)
2. Diffs the new snapshot against the last `status='applied'` commit using a four-level fingerprint hierarchy:
   - `object_fingerprint` equal → `LIKELY_UNCHANGED` (skip)
   - `definition_hash` differs → `SCHEMA_CHANGED`
   - `table_hash` / `row_count` differs → `ROWS_CHANGED`
   - Table absent in old but present in new → `NEW`
   - Table present in old but absent in new → `DROPPED`
3. Records a `hypha.commit` with `kind='sync_plan'`, `status='planned'`

**Report fields:**

| Field | Example | Notes |
|-------|---------|-------|
| `commit_id` | UUID | The new plan commit |
| `status` | `planned` | |
| `based_on_commit` | UUID | The last applied commit being diffed against |
| `fingerprint_algo` | `v3` | |
| `new_tables` | `2` | Tables to be created on Postgres |
| `dropped_tables` | `1` | Tables to be dropped from Postgres |
| `schema_changed_tables` | `3` | Tables with DDL changes |
| `rows_changed_tables` | `5` | Tables with data-only changes |
| `likely_unchanged_tables` | `47` | Tables that will be skipped |
| `tables_to_sync` | `11` | Total tables that will be touched |
| Per-table `action=` lines | | One line per non-unchanged table: `action=schema_changed schema=main table=orders detail=column_signature_changed` |

**Throws on:** no prior applied snapshot (call `hypha_base_snapshot()` first), fingerprint algorithm mismatch (run `hypha_base_snapshot()` to rebaseline).

**Use cases:**
- Preview changes before applying: check `schema_changed_tables`, `dropped_tables`
- Monitoring: run on a schedule, alert when `tables_to_sync > 0`
- Debugging: inspect per-table `detail=` for schema change reason

---

## `hypha_sync()`

```sql
SELECT hypha_sync();
```

**Purpose:** Applies an incremental sync to the stored Postgres target. Computes a diff (same as `hypha_sync_plan()`) and applies it: creates new tables, drops removed tables, updates changed tables, skips unchanged ones.

**What it does:**
1. Runs `hypha_base_snapshot_plan()` internally
2. Diffs against last applied snapshot (same hierarchy as `hypha_sync_plan()`)
3. Per table, chooses the cheapest safe operation:
   - `LIKELY_UNCHANGED` → skip (0 Postgres work)
   - `DROPPED` → `DROP TABLE IF EXISTS`
   - `NEW` → `CREATE TABLE` + `COPY`
   - `SCHEMA_CHANGED`:
     - Type change or column reordering → `DROP TABLE` + `CREATE TABLE` + `COPY`
     - Drop-only column change → `ALTER TABLE DROP COLUMN` (no COPY; preserves indexes)
     - Add-only or mixed add+drop → `ALTER TABLE ADD/DROP COLUMN` + `TRUNCATE` + `COPY`
   - `ROWS_CHANGED`:
     - PK table with row hashes → targeted `DELETE` + filtered `COPY INSERT` (only changed rows)
     - No PK, insert-only change → append-only fast path: filtered `COPY` of just the new rows, no `TRUNCATE` (logged as `KEYLESS_APPEND`)
     - No PK with any delete/update, or missing row hashes → `TRUNCATE` + full `COPY` (logged as `TRUNCATE_COPY`)
4. After commit: writes `hypha.sync_log` and `hypha.object_state` (best-effort)
5. Records a `hypha.commit` with `kind='sync'`, `status='applied'`

**Report fields:**

| Field | Example | Notes |
|-------|---------|-------|
| `commit_id` | UUID | The sync commit |
| `status` | `applied` | |
| `database` | `mydb` | |
| `tables_synced` | `11` | Tables that were actively changed on Postgres |
| `tables_dropped` | `1` | Tables dropped |
| `tables_skipped` | `47` | Tables with no changes (LIKELY_UNCHANGED or errors) |
| `tables_truncate_copy` | `2` | Tables that used TRUNCATE+COPY fallback (no PK or missing hashes) |
| `rows_synced` | `423` | Rows inserted/updated (row-level diff) or copied (TRUNCATE+COPY) |

**event_log codes:**
- `OK` — sync completed
- `TABLE_SKIP` — individual table failed; check `hypha.event_log` for details
- `KEYLESS_APPEND` — no-PK table changed by inserts only; new rows COPYed without TRUNCATE
- `TRUNCATE_COPY` — table had no PK (with deletes/updates) or missing row hashes; TRUNCATE+COPY was used
- `REMOTE_META_FAIL` — bookkeeping write failed (sync data still applied)

**Action values** (returned in the `action` column): `new`, `updated`, `appended` (keyless append-only fast path), `schema_changed`, `truncate_copy`, `dropped`.

**Throws on:** no prior applied snapshot, fingerprint algorithm mismatch.

**Use cases:**
- Regular incremental sync after DuckDB data changes
- Schema migration propagation (ADD/DROP COLUMN applied on Postgres without full DROP+CREATE)
- Pipeline: `SELECT hypha_sync_plan()` to preview, then `SELECT hypha_sync()` to apply

---

## `hypha_verify()`

```sql
SELECT hypha_verify();
```

**Purpose:** On-demand reconciliation tripwire for the `MUTABLE_ENTITY` in-place-update blind spot (see [fingerprinting.md §6.4](fingerprinting.md)). The fast O(1) fingerprint strategies can miss an in-place update that preserves row count and rowid range, leaving Postgres silently stale after a `hypha_sync()`. `hypha_verify()` recomputes the full per-row `EXACT` hash of every local table and reports any table that changed in a way the fast fingerprint would not catch.

**What it does:**
1. Enumerates the same user tables as `hypha_base_snapshot_plan()`.
2. For each table, computes the `EXACT` table_hash (full per-row SHA-256) now.
3. Compares it to the baseline stored in `hypha.verify_state` from the previous `hypha_verify()` run.
4. For a changed table, compares the current *fast* hash against the stored snapshot fast hash to classify the change.
5. Advances the baseline to the current exact hash.

**Per-table classification:**

| Result | Meaning |
|--------|---------|
| `armed` | First observation — baseline established, nothing to compare yet |
| unchanged | Exact hash matches the last verify; table is clean |
| `PENDING` | Changed since last verify, and the fast fingerprint also changed — `hypha_sync()` will catch it |
| `BLIND_SPOT_DRIFT` | Changed in-place but the fast fingerprint did **not** change — Postgres is stale; run `hypha_base_snapshot()` to reconcile |

**Return value:** One-line summary, e.g. `verify: 12 checked · 1 drift · 0 pending · 2 armed · 0 skipped`. Per-table detail prints to stderr.

**event_log codes:** `VERIFY_DRIFT` (warn), `VERIFY_PENDING` (info), `VERIFY_SKIP` (warn — unsupported column type), `VERIFY_SUMMARY`.

**No Postgres connection required.** The O(n) exact scan per table is the explicit cost of verifying. Each run measures "in-place change since the last verify," so run it as part of your post-sync routine to keep the tripwire armed.

**Relationship to `exact_verify` mode:** `hypha_verify()` lets the fast path stay fast and pays the exact cost only when you ask. If you would rather never miss a change in the first place, initialize with `exact_verify := true` (4th argument to `hypha_init`) so every snapshot and sync uses `EXACT` directly.

---

## Local metadata tables

All metadata lives in the `hypha` schema of the DuckDB file. Never write to these tables directly.

| Table | Written by | Purpose |
|-------|-----------|---------|
| `hypha.target` | `hypha_init` | Stored Postgres connection strings |
| `hypha.commit` | all plan/push/sync | Snapshot and sync history |
| `hypha.object_snapshot` | `hypha_base_snapshot_plan` | Per-table fingerprints and PK columns |
| `hypha.column_snapshot` | `hypha_base_snapshot_plan` | Per-column type mapping |
| `hypha.table_snapshot` | `hypha_base_snapshot_plan` | Per-table row counts and `table_hash` |
| `hypha.row_hash` | `hypha_base_snapshot_plan` | Per-row `pk_json` + `row_hash` (PK tables only) |
| `hypha.event_log` | all functions | Append-only observability; never throws |
| `hypha.verify_state` | `hypha_verify` | Per-table exact-hash baseline for the blind-spot tripwire |
| `hypha.meta` | `hypha_init` | Config/version: `metadata_schema_version`, `fingerprint_algo`, `hyphasync_version`, `fast_mode`, `exact_verify` |

---

## Remote metadata tables (written to Postgres)

Created automatically on first push inside a `hypha` schema on the Postgres target.

| Table | Written by | Purpose |
|-------|-----------|---------|
| `hypha.sync_log` | `hypha_base_snapshot`, `hypha_sync` | One row per push/sync: source database, commit IDs, table/row counts, timestamp |
| `hypha.object_state` | `hypha_base_snapshot`, `hypha_sync` | Current fingerprint state per synced table; upserted on each push |

---

## Typical workflows

### Initial setup

```sql
LOAD hyphasync;
SELECT hypha_init('postgresql://user:pass@host:5432/dbname');
SELECT hypha_doctor();           -- confirm metadata_initialized=true
SELECT hypha_target_status(NULL); -- confirm status=ok
```

### Push all tables for the first time

```sql
SELECT hypha_base_snapshot_plan(); -- inspect type mapping, check tables_hashed
SELECT * FROM hypha.column_snapshot WHERE table_name = 'my_table'; -- verify mapping
SELECT * FROM hypha_base_snapshot();       -- push
```

### Regular incremental sync

```sql
-- optional: preview first
SELECT hypha_sync_plan();
-- apply
SELECT hypha_sync();
```

### Investigate a sync

```sql
-- what changed in the last sync?
SELECT * FROM hypha.commit ORDER BY created_at DESC LIMIT 5;
-- any skipped tables?
SELECT * FROM hypha.event_log WHERE code IN ('TABLE_SKIP','TRUNCATE_COPY','HASH_SKIP') ORDER BY event_time DESC;
-- remote bookkeeping
SELECT * FROM hypha.object_state WHERE pg_schema = 'mydb_main' ORDER BY last_synced_at DESC;
```

### Force a full re-push (after major schema changes)

```sql
SELECT * FROM hypha_base_snapshot(); -- drops and recreates everything
```

---

## `hypha_help([name VARCHAR])`

```sql
-- List all functions
SELECT hypha_help(NULL);

-- Describe a specific function
SELECT hypha_help('hypha_init');
```

**Purpose:** Returns a plain-text function reference directly from SQL. Useful when documentation is not at hand.

**Arguments:**

| Argument | Type | Required | Notes |
|----------|------|----------|-------|
| `name` | `VARCHAR` | No | Function name to look up. NULL or omitted returns all descriptions. |

**Return value:** One line per matching function in the format:
```
function_signature  →  return_type  Description
```

**No side effects.** Safe to call at any time.
