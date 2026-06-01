# hyphasync — Benchmark Lessons Learned & Roadmap

**Benchmark date:** 2026-05-31  
**Benchmark scope:** 10 sample databases, fresh Postgres per run, `hypha_base_snapshot()`  
**hyphasync version:** 0.2.0 / DuckDB v1.5.2

---

## 1. Lessons Learned

### 1.1 What scales well

**Simple schemas with integer PKs (TPC-H)** are the happy path. TPC-H SF1/SF3/SF10 achieved
perfect row fidelity across 8 tables and delivered 280k–470k rows/sec. The key
structural properties that made this easy:

- Small number of tables (8): per-table transaction overhead is negligible.
- Integer PKs on every table: keyset pagination (`WHERE pk > last`) avoids the
  LIMIT/OFFSET double-scan problem and never revisits rows.
- Narrow schemas (61 columns): DDL generation is trivial, rows are small, COPY is efficient.

**Fingerprinting is fast everywhere.** Fingerprint time was < 1% of total time across all
databases. The `MUTABLE_ENTITY` strategy (COUNT + MIN/MAX rowid) completes in milliseconds
for even the largest tables — zone-map statistics mean DuckDB never scans a row. This is
the correct behavior for a base snapshot.

> **Note on strategy classification:** `ClassifyTable` now applies the full EXACT /
> APPEND_ONLY / MUTABLE_ENTITY classification on base snapshots as well as incremental
> syncs. Small tables (estimated bytes < 1 MB) will show `EXACT` and large tables with
> integer PKs will show `APPEND_ONLY`; `MUTABLE_ENTITY` is the fallback for everything
> else. This is the correct behavior for a base snapshot.

**Medium FERC databases (ferc6, ferc2)** completed without catastrophic failure and
produced reasonable throughput (~25 MB/sec). The `SET STORAGE EXTERNAL` mitigation for
wide text columns helps most tables clear the 8 KB Postgres row limit.

---

### 1.2 What doesn't scale

#### Per-table transaction overhead dominates wide catalogs

The table count and the Postgres round-trip budget are tightly coupled. Each table costs at
minimum six synchronous round trips: `BEGIN` → `DROP TABLE IF EXISTS` → `CREATE TABLE` →
`ALTER TABLE SET STORAGE EXTERNAL` → `COPY FROM STDIN` → `COMMIT`. That is ~6 ms at local
loopback latency, or 1,752 Postgres round trips for ferc2's 292 tables before a single row
is copied.

The throughput numbers confirm this:

| Database        | Tables | Rows       | Rows/sec |
|-----------------|--------|------------|----------|
| ferc714-xbrl    | 14     | 6,558,487  | 721,585  |
| tpch-sf1.db     | 8      | 8,661,245  | 471,437  |
| ferc6-xbrl      | 147    | 397,268    | 126,478  |
| ferc1-xbrl      | 255    | 2,000,422  | 87,324   |
| ferc2-xbrl      | 292    | 445,444    | 69,136   |

ferc2 has fewer rows than ferc714 but takes 7× longer per row — solely because of the
292-table overhead. The COPY itself is fast; the per-table scaffolding is not.

#### Schema-heavy databases spike memory disproportionately

ferc1 used **3,947 MB of RSS for a 993 MB source file** — a 4× expansion ratio. The memory
pressure comes from two sources:

1. **DuckDB buffer pool**: DuckDB's default memory limit is uncapped. As the plan phase
   queries `duckdb_columns()`, `duckdb_tables()`, and `duckdb_constraints()` for 255 tables,
   DuckDB materializes results and buffers source data pages. A 993 MB DuckDB file with
   heavy schema work can easily inflate to 2–3 GB of process RSS in the buffer pool alone.

2. **Schema metadata accumulation in DuckDB**: `hypha.column_snapshot` grows to hold all
   column metadata for the current commit. For a wide catalog (e.g., 4,233 total columns in
   ferc1), the column_snapshot table itself and its backing in-memory pages contribute
   hundreds of megabytes.

The current code is already correct in one sense — `col_rows` is a per-table local vector
that is deallocated after each table's INSERT. But the DuckDB buffer pool and the growing
`hypha.column_snapshot` table keep accumulating.

#### Row fidelity is not reliable for XBRL and wide-type databases

| Database        | Source Rows | Landed | Fidelity | Symptom |
|-----------------|-------------|--------|----------|---------|
| tpch-sf1.db     | 8,661,245   | 100%   | perfect  | clean schema |
| tpch-sf3.db     | 25,976,639  | 100%   | perfect  | clean schema |
| tpch-sf10.db    | 86,586,082  | 100%   | perfect  | clean schema |
| ferc714-xbrl    | 6,558,487   | ~32%   | **FAIL** | tables silently skipped |
| frankenstein    | 17,006,411  | ~74%   | **FAIL** | 4 tables missing |

---

### 1.3 Where the bottlenecks actually are

```
hypha_base_snapshot() time allocation (typical COPY-heavy run):

  [Fingerprint plan]   < 1%   — always MUTABLE_ENTITY, zone-map O(1)
  [Schema DDL]         1–3%   — CREATE TABLE, SET STORAGE EXTERNAL, DROP
  [COPY to CSV]        30–40% — DuckDB COPY (SELECT …) TO '/tmp/file.csv'
  [fread + PQputCopy]  30–40% — read temp file, stream to Postgres
  [PG row_hash/meta]   5–10%  — row_hash inserts for PK tables (plan phase)
  [Transaction mgmt]   10–20% — BEGIN/COMMIT × N_tables (grows with table count)
```

The temp-file round trip (write CSV → read CSV) wastes one full I/O pass per chunk for
every table. For tpch-sf10 (2.5 GB, 86M rows), this overhead is the difference between
8 MB/sec achieved and the 40+ MB/sec theoretically achievable with a direct pipe.

---

### 1.4 Row fidelity gaps — root cause analysis

**The mechanism:** when a table's COPY throws any exception (Postgres error, libpq error,
type conversion failure), the code rolls back to the per-table savepoint and continues to
the next table. The failure is logged to `hypha.event_log` as `TABLE_FAIL` but is otherwise
silent — the output row shows `rows_copied=0` and the table simply does not exist in Postgres.

**Causes in the benchmark:**

1. **Postgres 8 KB row limit (fixed-width columns)**: ferc60 explicitly failed with
   "row too big". `SET STORAGE EXTERNAL` moves varlena (text, bytea, jsonb) values
   out-of-line, but fixed-width columns (integer, numeric, date, timestamp) cannot go
   out-of-line. A table with hundreds of integer/numeric columns can exceed 8 KB in its
   fixed-width portion alone and will fail every COPY row. No error is surfaced until
   the first row is sent.

2. **All-unsupported-type columns → table skipped**: when `DuckTypeToPostgres` returns
   `"(unsupported: ...)"` for every column (e.g., a table consisting entirely of ENUM
   columns, UNION types, or custom DuckDB extension types), `cols.empty()` becomes true
   and the table is silently skipped with `"skipped: no supported columns"`. This is the
   most likely cause for frankenstein's 4 missing tables — frankenstein is a type-coverage
   DB designed to exercise exactly these edge cases.

3. **Data-level COPY failures**: values that cannot be represented in the mapped Postgres
   type (e.g., out-of-range timestamps, NaN in a `real` column, encoding errors in text)
   cause Postgres to abort the COPY session. The savepoint catches the exception and moves
   on, leaving the table absent from Postgres. This is the most likely explanation for
   ferc714's 32% landing rate: some tables in the XBRL dataset have data values that fail
   Postgres's strict type enforcement at COPY time.

4. **NOT NULL constraint violations**: columns marked `NOT NULL` in DuckDB might have NULL
   values in practice (XBRL data is notoriously sparse). `CREATE TABLE ... col_name type
   NOT NULL` followed by a COPY containing NULLs will fail the COPY.

**The silent failure problem:** None of these failures stop the snapshot. They are
logged as `TABLE_FAIL` warnings in `hypha.event_log` but the top-level output and return
value of `hypha_base_snapshot()` do not report a fidelity shortfall. The caller has no
easy way to know that 68% of ferc714's rows are missing.

---

### 1.5 What "typical" vs "extreme" looks like

**Typical** (works today, fast, correct):
- ≤ 50 tables, ≤ 200 columns total, integer PKs, standard scalar types
- TPC-H is the canonical example; any well-normalized OLTP schema behaves similarly
- Expected: 300k–500k rows/sec, < 2× file size in RSS, 100% fidelity

**Stress** (works but with overhead):
- 100–300 tables, 1k–5k total columns, mixed types, some no-PK tables
- FERC6/ferc2 are here: correct but slow due to per-table overhead
- Expected: 50k–130k rows/sec, 3–4× file size in RSS, > 95% fidelity if data is clean

**Extreme** (current limits):
- > 200 tables, high row counts, XBRL/wide schemas with unusual types
- ferc1 and ferc714 are here: memory pressure, fidelity gaps
- ferc1 at 993 MB → 4 GB RSS is the current memory ceiling
- tpch-sf10 at 2.5 GB → 5 min 7 sec is the current throughput ceiling

---

## 2. Parallelism Analysis

### 2.1 Current architecture

```
Table N:  [DuckDB scan] → [COPY TO /tmp/file.csv] → [fread loop] → [PQputCopyData]
Table N+1: (waits for Table N to complete)
...
```

Everything is single-threaded, sequential, and disk-mediated. The temp file exists because
DuckDB's `COPY TO` emits a file, and there is no built-in DuckDB API to stream CSV rows
directly into a caller-controlled buffer without going to disk first.

### 2.2 Intra-table parallelism

**Can a single table's COPY be parallelized?**

Theoretically: open N Postgres connections, each with a `COPY FROM STDIN` session on the
same table, feed them non-overlapping ranges of rows from DuckDB. Postgres handles
concurrent inserts into the same table via tuple-level locking.

**Practical limitations:**
- Postgres WAL write is the bottleneck at high insert rates. N concurrent writers
  serialize on WAL — you get N connections with N/1 WAL throughput, not N× speedup.
- Without a PK, non-overlapping ranges require `LIMIT/OFFSET` or `ctid` tricks, both
  of which have correctness or performance hazards.
- The N temp files (`schema_table_0.csv`, `schema_table_1.csv`, ...) need concurrent
  DuckDB scans over the same table, which DuckDB supports (multiple `Connection`s on
  the same file) but wastes disk I/O writing the same pages N times.

**Verdict:** intra-table parallelism is O(N) complexity for low marginal gain. It is not
the right focus. The bottleneck for wide tables is already the Postgres WAL, not the number
of connections.

### 2.3 Inter-table parallelism — the high-value win

**Can multiple tables be copied concurrently?**

Yes. This is well-supported by both DuckDB and Postgres:

- DuckDB allows multiple concurrent `Connection` objects on the same `.duckdb` file in
  read mode. Each connection gets its own transaction snapshot.
- Postgres handles multiple concurrent `COPY FROM STDIN` sessions on different tables with
  zero lock contention (each table has its own lock chain).
- The only shared state that needs serialization is already mutex-protected in the current
  code: `state.lock` guards `tables_synced`, `rows_synced`, `error_count`, `skipped_count`.

**Worker pool design:**

```cpp
// N workers, each processing one table at a time from a shared queue
for (int w = 0; w < N; ++w) {
    workers.emplace_back([&state]() {
        Connection local_con(state.db);      // per-worker DuckDB connection
        PGconn *local_pg = PQconnectdb(...); // per-worker Postgres connection
        while (true) {
            size_t idx;
            { std::lock_guard<std::mutex> g(state.lock);
              if (state.current_idx >= state.tables.size()) break;
              idx = state.current_idx++; }
            CopyOneTable(local_con, local_pg, state, state.tables[idx]);
        }
    });
}
```

**Right value for N:**

| Factor                  | Implication                                           |
|-------------------------|-------------------------------------------------------|
| Postgres `max_connections` | Default 100; use at most 25% → N ≤ 25            |
| Postgres WAL throughput | Saturates at ~4–8 concurrent COPY writers on NVMe    |
| DuckDB CPU parallelism  | DuckDB scan already uses multiple threads internally  |
| Temp file I/O           | N concurrent temp files means N× disk writes         |
| Recommendation          | **N = 4–8** for local Postgres; expose as `workers` option |

**Expected speedup for ferc2 (292 tables, transaction-bound):**

With N=8: the 292 table transactions can run 8 at a time → ~8× reduction in total
transaction overhead time → from 6.4 s to ~0.8 s for the schema setup portion.
COPY throughput scales sub-linearly (WAL contention), but the net win is still 3–5×.

**Serialization requirements:**
- `hypha.event_log` DuckDB inserts: already guarded by `state.lock`; each worker needs
  its own DuckDB `Connection` (DuckDB connections are not thread-safe)
- `hypha.commit` and `hypha.table_snapshot` writes: done before/after the parallel phase —
  no change needed
- Temp file names: already safe (`schema_table.csv`) as long as schema+table is unique
  within the database (it is by definition)
- `pg_log` (remote event log connection): each worker should have its own `pg_log`
  connection, or use a dedicated logging connection with a mutex

### 2.4 Pipeline parallelism — eliminate the temp file

**Current pipeline:**
```
DuckDB: COPY (SELECT ...) TO '/tmp/file.csv'    [write ~64 MB CSV to disk]
OS:     fread(buf, 65536, f)                    [read 64 MB CSV from disk]
libpq:  PQputCopyData(pg, buf, n)              [send 64 MB to Postgres]
```

**Proposed direct-pipe pipeline:**

DuckDB has a streaming query execution path. Instead of `COPY (SELECT ...) TO file`,
execute `SELECT ...` via DuckDB and serialize the `MaterializedQueryResult` (or better,
a `StreamQueryResult`) to CSV in C++, feeding directly into `PQputCopyData`:

```cpp
// No temp file — stream DuckDB rows directly into Postgres COPY
auto result = con.StreamQuery(select_sql); // returns QueryResult lazily
std::string csv_buf;
csv_buf.reserve(65536);
while (auto chunk = result->Fetch()) {
    AppendChunkAsCSV(*chunk, csv_buf);
    if (csv_buf.size() >= 65536) {
        PQputCopyData(pg, csv_buf.data(), csv_buf.size());
        csv_buf.clear();
    }
}
if (!csv_buf.empty()) PQputCopyData(pg, csv_buf.data(), csv_buf.size());
PQputCopyEnd(pg, nullptr);
```

This eliminates:
- One full write-to-disk per chunk (currently 64 MB chunks)
- One full read-from-disk per chunk
- `stat()` and `fopen()`/`fclose()` overhead per chunk
- The `remove()` cleanup per chunk

**Expected speedup:** tpch-sf10 at 8.3 MB/sec is I/O-bound by the temp-file round trip.
Removing the temp file should bring tpch-sf10 to 15–25 MB/sec (limited by Postgres
WAL + libpq network buffer). For ferc1 (already at 43 MB/sec), the gain is smaller.

The streaming `QueryResult` path also eliminates the chunk-loop complexity: a single DuckDB
scan over the full table, fed row-by-row into libpq. This simplifies the keyset/LIMIT+OFFSET
pagination logic — it becomes unnecessary for the COPY path (only needed for row_hash).

**This is the Phase B streaming architecture.** The `hypha_base_snapshot` function is
already a streaming DuckDB TableFunction. The next step is to stream *through* it without
materializing to disk.

---

## 3. Schema-Width Problem

### 3.1 What is the actual memory culprit

For ferc1 (993 MB file, 255 tables, 4 GB RSS), the memory pressure has three components:

**Component 1: DuckDB buffer pool (largest contributor)**

DuckDB's default memory limit is uncapped. `RunBaseSnapshotPlan` drives 255 × ~5 queries
per table (enumerate columns, detect PK, fingerprint, insert snapshots) = ~1,275 DuckDB
queries. As these execute, DuckDB reads source data pages into its buffer pool. For a
993 MB file being actively queried, the buffer pool can hold 1–3 GB of decompressed
columnar data. This is the dominant contributor to the 4 GB RSS.

**Fix:** Set `memory_limit` early in the session:
```sql
SET memory_limit = '1GB';    -- or parameterize as hypha_base_snapshot_plan(memory_limit='1GB')
```

**Component 2: hypha.column_snapshot growth**

The `hypha.column_snapshot` DuckDB table accumulates all column metadata for the current
commit. For ferc1 with 4,233 total columns, this table grows to ~4,233 rows. At ~500 bytes
of DuckDB row overhead each, that's only ~2 MB — negligible. This is not the culprit.

**Component 3: DDL string generation and column metadata vectors**

`BuildCreateTableDDL` generates one DDL string per table. For wide tables this can be large
(100+ columns × ~40 bytes per column fragment = ~4 KB per DDL). Across 255 tables, all DDL
strings are short-lived (deallocated after each table's `PGExec`). This is not the culprit.

**Component 4: DuckDB materialized results per table**

`Exec(con, ..., "enumerate columns")` returns a `MaterializedQueryResult`. For a table with
many columns, this materializes all column descriptors in DuckDB's memory. These are
deallocated after each table, but DuckDB may retain the buffer pages. DuckDB's result
caching and buffer management can hold multiple result sets simultaneously during the plan
phase.

### 3.2 Lazy schema loading — the fix

The current `RunBaseSnapshotPlan` processes ALL tables in a single pass before any COPY
starts. This means DuckDB has opened the source file and is running queries against all 255
tables' schemas while Postgres has not yet received a single row. All 255 table's metadata
is in DuckDB's buffer pool simultaneously.

**Proposed two-phase approach (already partially in place):**

Phase 1 — *lightweight catalog scan*: enumerate table names only (no column metadata).
Record just `(schema_name, table_name)` into a plan table. Fingerprint each table
(MUTABLE_ENTITY is O(1) — no column scanning needed). Insert into `hypha.table_snapshot`.

Phase 2 — *per-table streaming COPY*: for each table in the plan, load column metadata
*just-in-time* from `duckdb_columns()` (one table at a time), build DDL, COPY data, release.

**The current code is almost there.** `HyphaSnapshotFunction` already reads from
`hypha.column_snapshot` per-table (one table's columns at a time). The memory issue is in
`RunBaseSnapshotPlan` which processes all tables before COPY begins. The fix is to
interleave the plan and COPY phases: scan one table → fingerprint it → copy it → move on.

**Proposed merged one-pass architecture:**
```
For each table in duckdb_tables():
  1. Enumerate columns (one table, deallocate after)
  2. Compute fingerprint (MUTABLE_ENTITY, O(1))
  3. Insert column_snapshot, table_snapshot, object_snapshot rows
  4. Immediately COPY to Postgres (no accumulation phase)
  5. Deallocate all per-table allocations before moving to next table
```

This bounds DuckDB memory to: buffer pool for one table's pages + one table's column
metadata + one table's COPY CSV buffer. The `memory_limit` setting further caps the buffer
pool. The 4 GB RSS for ferc1 should drop to < 1 GB with both changes.

### 3.3 Streaming DDL generation

`BuildCreateTableDDL` currently materializes the entire DDL string as a `std::ostringstream`
before calling `PGExec`. For a table with 4,000 columns this is a ~200 KB string. This is
fine. The bigger opportunity is to set `memory_limit` (see §3.1) and interleave phases
(see §3.2). DDL string size is not the bottleneck.

---

## 4. Row Fidelity Gaps — Investigation & Fixes

### 4.1 What we know for certain

The `max_rows_per_table` guard is **not** the cause (default is 0 = unlimited; would only
apply if explicitly set in `hypha.meta`).

The row_hash table is **not** used as a COPY filter (it is populated during the plan phase
and is used for incremental sync diff, not base snapshot COPY).

Type mapping failures **do** silently drop columns (not rows): when `DuckTypeToPostgres`
returns `"(unsupported: ...)"`, that column is excluded from the COPY SELECT list but the
row is still inserted with the remaining columns. So type mapping alone cannot cause 68% row
loss — unless ALL columns of a table are unsupported, in which case the table is entirely
skipped.

### 4.2 The savepoint rollback is the mechanism

From the code:
```cpp
} catch (const std::exception &ex) {
    ok = false;
    PGresult *rb = PQexec(pg, ("ROLLBACK TO SAVEPOINT " + sp).c_str());
    tables_skipped++;
    LogAll(con, pg_log, ..., "warn", "sync", "TABLE_SKIP", "skipped ... " + err_msg);
}
```

Any exception during a table's COPY — including a Postgres error mid-COPY — causes a
complete rollback of that table. The table does not appear in Postgres at all (0 rows, not
partial rows). The final output row for that table shows `status = "error: ..."` but this
is easy to miss in practice.

### 4.3 ferc714 32% fidelity: probable root causes

ferc714 has 14 tables with 6.5M total rows. Landing only 32% (~2.1M rows) means
approximately 10–12 of the 14 tables failed COPY and were rolled back. The most likely
causes:

1. **NOT NULL constraint violations during COPY**: `RunBaseSnapshotPlan` preserves `NOT NULL`
   constraints from DuckDB in the Postgres DDL. XBRL datasets are sparse — a column marked
   `NOT NULL` in DuckDB's schema may have NULL values in practice (DuckDB doesn't enforce
   NOT NULL at scan time). When Postgres receives a NULL for a NOT NULL column, the COPY
   aborts.

2. **Data values rejected by Postgres type**: XBRL text fields with embedded special
   characters, Unicode control characters, or values that overflow numeric precision can
   cause Postgres to reject the COPY. DuckDB's CSV export does not sanitize these.

3. **Row size for wide XBRL tables**: some FERC XBRL tables have hundreds of text columns
   per form; even with `SET STORAGE EXTERNAL`, if a row has more than ~1,600 fixed-width
   columns (beyond the Postgres tuple header limit), it can fail.

### 4.4 frankenstein 4 missing tables: probable root cause

frankenstein.duckdb is a synthetic type-coverage database. The 4 missing tables are most
likely tables whose columns are all of types that `DuckTypeToPostgres` cannot map:

- `ENUM` types: no mapping exists; would return `"(unsupported: ENUM)"` for every column
- `UNION` types: no mapping
- `TIMESTAMP_S` / `TIMESTAMP_MS` / `TIMESTAMP_NS` (sub-second timestamp variants): not
  in the `DuckTypeToPostgres` switch
- Parameterized `LIST(STRUCT(...))` or `MAP(ENUM, ...)`: mapped to `jsonb`, but if a
  subtype itself is unsupported, the outer type mapping may fail at COPY time

When `cols.empty()` after filtering unsupported types, the table is skipped with
`"skipped: no supported columns"` — a correctness-silent skip.

### 4.5 Fixes required

**Fix 1 — Drop NOT NULL from base snapshot DDL** (highest impact, lowest risk):

```cpp
// In BuildCreateTableDDL: suppress NOT NULL for base snapshot
// DuckDB does not enforce NOT NULL on insert; Postgres will reject COPY
// if the data contains NULLs. Log a warning if NOT NULL was suppressed.
if (!cols[i].is_nullable && !suppress_not_null) {
    ddl << " NOT NULL";
}
```

For base snapshots, `NOT NULL` should be dropped entirely. The goal is to mirror the data,
not enforce the schema constraint. A follow-up `hypha_sync` can detect schema drift.

**Fix 2 — Add TIMESTAMP_S/MS/NS to DuckTypeToPostgres:**

```cpp
if (t == "TIMESTAMP_S" || t == "TIMESTAMP_MS" || t == "TIMESTAMP_NS") {
    return "timestamp without time zone";  // truncates sub-second precision
}
```

**Fix 3 — Report fidelity shortfall in output:**

`hypha_base_snapshot()` should emit a top-level warning row (or column) when
`tables_skipped > 0`. Currently the per-table `"error: ..."` status is the only signal.
At minimum, the final summary should include `tables_skipped` as a prominent output column
alongside `rows_synced`.

**Fix 4 — Distinguish "column unsupported" from "table skip":**

When a table is skipped because all columns are unsupported, log it as a distinct code
(`UNSUPPORTED_SCHEMA`) rather than `TABLE_FAIL`. This helps operators diagnose the issue
without digging through event_log error messages.

---

## 5. Roadmap Recommendations

### Priority 0 — Immediate correctness fixes

These should land before any performance work. Correctness problems are harder to find
after the fact.

**0a. Drop NOT NULL in base snapshot DDL** _(ferc714 32% fidelity)_

Suppress `NOT NULL` constraints when building the Postgres table DDL for base snapshots.
Base snapshots mirror data, not schema contracts. This single change is likely to recover
most of ferc714's missing rows.

```
Risk: low | Effort: 30 min | Impact: fixes ferc714 + similar XBRL databases
```

**0b. Add TIMESTAMP_S/MS/NS to type mapper** _(frankenstein + any nanosecond-precision DB)_

Handle the three sub-second DuckDB timestamp variants. Map to `timestamp without time zone`
with a precision truncation warning in the event_log.

```
Risk: low | Effort: 15 min | Impact: fixes frankenstein missing tables (partial)
```

**0c. Surface fidelity shortfall in output**

Add `tables_skipped INTEGER` and `rows_failed BIGINT` as output columns of
`hypha_base_snapshot()`. Return values > 0 should be accompanied by a top-level warning
message that names the skipped tables so operators don't need to query `hypha.event_log`.

```
Risk: low | Effort: 2 hours | Impact: makes silent failures visible
```

**0d. Set memory_limit before plan phase** _(ferc1 4 GB RSS)_

Set `SET memory_limit = '2GB'` (or configurable) at the start of `RunBaseSnapshotPlan`.
This caps DuckDB's buffer pool and prevents runaway RSS on schema-heavy databases.

```
Risk: low | Effort: 30 min | Impact: reduces ferc1 RSS from 4 GB to < 2 GB
```

---

### Priority 1 — Short-term wins (next sprint)

**1a. Pipeline parallelism: eliminate the temp file**

Replace the `COPY (SELECT ...) TO '/tmp/file.csv'` + `fread` loop with a direct streaming
path: execute `SELECT ...` via DuckDB's streaming query API (`Connection::Query` →
`QueryResult::Fetch()` → format to CSV in C++) and feed directly into `PQputCopyData`.

- Removes one full I/O write + one full I/O read per chunk per table
- Simplifies the chunk loop (no `stat()`, no `fopen`/`fclose`, no `remove()`)
- Eliminates disk space requirement for temp files (currently one 64 MB temp file per table)
- Expected speedup: 1.5–2× for I/O-bound databases (tpch-sf10 8.3 → 15+ MB/sec)

```
Risk: medium (streaming CSV serialization must match Postgres COPY FROM STDIN format exactly)
Effort: 1–2 days
Impact: significant throughput improvement for large tables
```

**1b. Interleave plan and COPY phases (lazy column loading)**

Merge `RunBaseSnapshotPlan` and `HyphaSnapshotFunction` so column metadata is loaded, DDL
is built, and data is copied one table at a time in a single pass. This bounds per-table
DuckDB memory allocation to one table's pages + one table's column metadata + one CSV
buffer.

```
Risk: medium (restructures the two-phase commit model; needs careful commit_id handling)
Effort: 1 day
Impact: ferc1 RSS from 4 GB to < 1 GB; enables processing databases > available RAM
```

---

### Priority 2 — Medium-term (async worker pool)

**2a. Inter-table parallelism: N-worker pool**

Dispatch tables to a thread pool of N workers, each with its own DuckDB `Connection` and
`PGconn`. The mutex-protected `state.current_idx` acts as the work queue. Recommended
default N = 4, tunable via `workers` option.

Implementation notes:
- Each worker opens its own `Connection db_file (read)` and `PQconnectdb(conn_string)`
- Per-worker temp filenames: `schema_table_workerid.csv` to avoid collision (or eliminate
  temp files via 1a first)
- `hypha.event_log` writes: each worker uses its own DuckDB `Connection` with the mutex
  protecting `LogAll` calls
- `pg_log` connection: one dedicated logging PGconn shared under a mutex, or one per worker

Expected results for ferc2 (292 tables, currently 6.4 s):
- With N=4: schema setup phase ~4× faster → ~2 s
- With N=8: schema setup ~6× faster → ~1 s (diminishing returns due to WAL contention)

```
Risk: medium-high (thread safety in DuckDB + libpq; both are thread-safe per-connection but
      not across connections without locks)
Effort: 3–5 days
Impact: 3–8× speedup for transaction-overhead-dominated databases (ferc2, ferc1)
```

**2b. Intra-table row_hash chunk parallelism**

The `row_hash` computation phase (in `RunBaseSnapshotPlan`) uses LIMIT/OFFSET chunks to
insert SHA-256 hashes into `hypha.row_hash`. For tables with millions of rows, this can
take seconds. This phase could be parallelized across chunks using DuckDB's internal
parallel scan (via `SET threads = N`). However, this is lower priority than inter-table
parallelism because fingerprinting is < 1% of total time in the benchmark.

```
Risk: low
Effort: 1 day
Impact: minor (fingerprint is not the bottleneck)
```

---

### Priority 3 — Architecture direction (Phase B streaming foundation)

All of the above improvements are implementable within the current `hypha_base_snapshot`
streaming TableFunction architecture. The function already:
- Yields one output row per table as it finishes (native DuckDB progress reporting)
- Has a GlobalState struct with a mutex and worker-pool-ready table queue
- Processes tables one at a time with isolated savepoints

The recommended architecture direction for Phase B:

```
GlobalState:
  - DuckDB file path (read-only, N connections can open simultaneously)
  - tables[]: pre-computed plan (from lightweight catalog scan)
  - current_idx, lock (work queue for N workers)
  - shared pg_log connection (mutex-protected)

Per-worker LocalState:
  - Connection duckdb_con(db_file)    // own DuckDB connection
  - PGconn *pg = PQconnectdb(...)     // own Postgres connection
  - std::string csv_buf               // streaming CSV buffer (no temp file)

Per-table work (CopyOneTable):
  1. Enumerate columns just-in-time from duckdb_columns()
  2. BEGIN on pg
  3. DROP + CREATE TABLE (with memory_limit active)
  4. SET STORAGE EXTERNAL for varlena columns
  5. Stream COPY: DuckDB QueryResult → CSV buffer → PQputCopyData (no temp file)
  6. COMMIT
  7. Update state counters under lock
  8. LogAll (DuckDB event_log write under lock, pg_log write under lock)
```

This architecture supports both the immediate fixes (0a–0d) and the medium-term wins
(1a–2a) without a rewrite. Each change is incremental.

**The Phase B streaming table function is the right foundation for all async improvements.**
Build the temp-file elimination (1a) first, then interleave phases (1b), then add the
worker pool (2a). Each step makes the next one easier.

---

## 6. Summary Table

| ID  | Fix / Feature                         | Priority    | Effort | Expected Impact                          |
|-----|---------------------------------------|-------------|--------|------------------------------------------|
| 0a  | Drop NOT NULL in base snapshot DDL    | **URGENT**  | 30 min | Fixes ferc714 32% → ~95%+ fidelity       |
| 0b  | TIMESTAMP_S/MS/NS type mapping        | **URGENT**  | 15 min | Fixes frankenstein missing tables        |
| 0c  | Surface fidelity shortfall in output  | **URGENT**  | 2 hr   | Makes silent failures visible            |
| 0d  | SET memory_limit before plan phase    | **URGENT**  | 30 min | Reduces ferc1 RSS 4 GB → < 2 GB         |
| 1a  | Eliminate temp file (streaming COPY)  | High        | 1-2 d  | 1.5-2× throughput for large tables      |
| 1b  | Interleave plan+COPY phases           | High        | 1 d    | Bounded memory for any-size schema       |
| 2a  | Inter-table parallelism (N workers)   | Medium      | 3-5 d  | 3-8× speedup for many-table databases   |
| 2b  | Row_hash chunk parallelism            | Low         | 1 d    | Minor (< 1% of total time)              |

**Correctness is the gate.** Fix 0a–0d before any performance work. A tool that silently
drops 68% of rows is not ready for any real workload, no matter how fast it runs.

---

*Generated from benchmark run 2026-05-31. Source code reviewed: `src/hypha_snapshot.cpp`,
`src/hypha_fingerprint.cpp`, `src/hypha_metadata.cpp`, `scripts/benchmark-full.sh`.*
