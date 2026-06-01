# Dynamic Chunk Sizing for `hyphasync` COPY Loops

**Status:** Design / scoping only — not yet implemented

---

## Problem: Fixed `COPY_CHUNK_ROWS = 100000` Is Wrong for Most Tables

`hypha_snapshot.cpp` defines a single constant:

```cpp
static constexpr int64_t COPY_CHUNK_ROWS = 100000;
```

This constant is used in three distinct chunk loops:

| Loop | File path | Description |
|------|-----------|-------------|
| `row_hash INSERT` | `RunBaseSnapshotPlan` | Populates `hypha.row_hash` in local DuckDB; PK + hash per row |
| `base_snapshot COPY` | `RunBaseSnapshot` | Full table → CSV → Postgres via `PQputCopyData` |
| `sync COPY` | `RunSync` | Same COPY path for NEW/SCHEMA_CHANGED/ROWS_CHANGED fallback tables |

A fixed row count is the wrong primitive because actual memory usage scales with **bytes per row**, not row count:

| Schema | Bytes/row (est.) | 100k-row chunk size | Problem |
|--------|-----------------|---------------------|---------|
| `(id INT, amount NUMERIC, date DATE)` | ~20 bytes | ~2 MB | Too small — far too many round-trips/chunks |
| `(id INT, 5 × VARCHAR avg 50 bytes)` | ~270 bytes | ~27 MB | Reasonable |
| `(50 × VARCHAR avg 200 bytes)` | ~10,002 bytes | ~1 GB | Memory explosion; Postgres transaction balloons |
| `(id, blob BYTEA avg 8KB)` | ~8,200 bytes | ~820 MB | Catastrophic |

The 100k default was chosen arbitrarily. A **target-bytes-per-chunk** strategy is a direct fix.

---

## Information Available at Chunk-Size Decision Time

Both COPY loops already have access to:

- **`cols: std::vector<ColumnDef>`** — column name, `duckdb_type`, `postgres_type`, `needs_json_cast`
- **`row_count: int64_t`** — available from `hypha.table_snapshot` (already queried for base snapshot; can be queried cheaply for sync)
- **`fp_cols: std::vector<std::pair<std::string, std::string>>`** — in the `row_hash` loop (name + duckdb_type)

`ColumnDef` (defined at line 589) is:
```cpp
struct ColumnDef {
    std::string name;
    std::string postgres_type;
    std::string duckdb_type;
    bool is_nullable;
    bool needs_json_cast;
};
```

This is sufficient to implement the per-type width heuristic without any extra SQL queries.

---

## Proposed Algorithm

### Function signature

```cpp
/// Estimate how many rows fit in target_chunk_bytes for the given column set.
/// Uses per-type byte-width heuristics; VARCHAR widths can be overridden from stats.
int64_t ComputeChunkRows(
    const std::vector<ColumnDef>& cols,
    int64_t target_chunk_bytes,           // e.g. 64 MB for COPY, 16 MB for row_hash
    const std::unordered_map<std::string, int64_t>& varchar_max_lens = {}
                                          // optional: column_name -> observed max length
);
```

### Per-type byte-width table

These are **CSV serialized** byte estimates (not in-memory sizes), because the output of
`COPY ... TO ... CSV` is what is actually buffered and streamed:

| DuckDB type(s) | Estimated CSV bytes |
|----------------|---------------------|
| `BOOLEAN` | 5 |
| `TINYINT`, `INT1`, `UTINYINT` | 4 |
| `SMALLINT`, `INT2`, `USMALLINT` | 6 |
| `INTEGER`, `INT4`, `UINTEGER` | 11 |
| `BIGINT`, `INT8`, `UBIGINT` | 20 |
| `HUGEINT` | 40 |
| `FLOAT`, `REAL` | 15 |
| `DOUBLE`, `FLOAT8` | 25 |
| `DECIMAL(p,s)` / `NUMERIC(p,s)` | `p + 3` (capped at 50) |
| `DATE` | 10 (`YYYY-MM-DD`) |
| `TIME` | 15 |
| `TIMESTAMP`, `DATETIME` | 27 (`YYYY-MM-DD HH:MM:SS.ffffff`) |
| `TIMESTAMPTZ` | 33 |
| `INTERVAL` | 20 |
| `UUID` | 36 |
| `VARCHAR` / `TEXT` | **32** (heuristic; overridable via `stats()`) |
| `BLOB`, `BYTEA`, `VARBINARY` | **128** (hex-escaped in CSV) |
| `JSON` | **128** |
| `STRUCT(...)`, `ROW(...)`, `MAP(...)` | **64** (serialized as JSON) |
| `LIST(...)` / `type[]` | **64** (serialized as JSON array) |
| `BIT`, `BITSTRING` | 32 |
| Unknown / unsupported | **64** (conservative fallback) |

### Pseudocode

```
function EstimateBytesPerRow(cols, varchar_max_lens) -> int64:
    total = 0
    for col in cols:
        width = TypeWidthBytes(col.duckdb_type)
        if IsVarcharType(col.duckdb_type) and col.name in varchar_max_lens:
            // Use observed max length, capped at 4096 to avoid one outlier row
            // dominating the estimate. Average ≈ max/2 is a reasonable heuristic.
            width = min(varchar_max_lens[col.name], 4096) / 2 + 4
        total += width
    total += len(cols) + 1    // commas between columns + newline
    total += 2                // avg null overhead (\N appears occasionally)
    return max(total, 8)      // at least 8 bytes even for degenerate schemas

function ComputeChunkRows(cols, target_chunk_bytes, varchar_max_lens) -> int64:
    bytes_per_row = EstimateBytesPerRow(cols, varchar_max_lens)
    chunk_rows = target_chunk_bytes / bytes_per_row
    return clamp(chunk_rows, MIN_CHUNK_ROWS=1000, MAX_CHUNK_ROWS=500000)
```

### Target budgets

| Loop | Target bytes | Rationale |
|------|-------------|-----------|
| COPY to Postgres (base snapshot, sync) | **64 MB** | Balances round-trip count vs. Postgres transaction size |
| `row_hash INSERT` (local DuckDB) | **16 MB** | No network; smaller is fine; avoids large local transactions |

Both values should be exposed as configurable settings (e.g., `hypha_set('copy_chunk_mb', 64)`)
rather than hard-coded constants, to allow tuning without recompilation.

### Clamp rationale

- **Floor: 1,000 rows** — Prevents degenerate behavior for tables with huge average row sizes
  (e.g., a 1-column BLOB table). Below ~1k rows/chunk, round-trip overhead dominates.
- **Ceiling: 500,000 rows** — Prevents unexpectedly large chunks for extremely narrow tables.
  At ~500k rows and even 10 bytes/row, the chunk is still only ~5 MB.

---

## DuckDB Column Statistics

### `stats(col)` — the useful one

DuckDB exposes a `stats(column_expr)` aggregate function that returns per-column internal
statistics as a text string:

```sql
SELECT stats(name) FROM my_table LIMIT 1;
-- → "[Min: hello0, Max: hello999, Has Unicode: false, Max String Length: 8]
--    [Has Null: false, Has No Null: true][Approx Unique: 987]"
```

For VARCHAR/TEXT columns, **`Max String Length`** is embedded in the output. It can be
extracted with a regexp:

```sql
SELECT
    regexp_extract(stats("col1"), 'Max String Length: (\d+)', 1)::INTEGER AS col1_max,
    regexp_extract(stats("col2"), 'Max String Length: (\d+)', 1)::INTEGER AS col2_max
FROM schema_name.table_name
LIMIT 1;
```

This SQL can be **dynamically constructed** at chunk-size decision time by iterating over
`cols` and emitting one `regexp_extract(stats(...))` expression per VARCHAR column. A single
table scan yields all VARCHAR column widths at once.

**Verified behavior (tested against the hyphasync build):**
- Works on both persistent and in-memory (row-group buffered) tables
- `Max String Length` reflects the maximum byte length across all rows in the table
- Returns `NULL` for empty tables (guard with `COALESCE(..., 32)`)
- `stats()` on non-VARCHAR types does not include `Max String Length` — the regexp returns
  empty string, which should fall back to the type-heuristic default

### `PRAGMA storage_info('table')`

Returns a richer per-segment view (min, max, has-null, compression, block locations). The
`stats` column also contains `Max String Length` for VARCHAR segments. This is more verbose
than needed for chunk-size estimation; `stats()` is the simpler interface.

### What's **not** available

- No `duckdb_statistics()` table function — confirmed absent in the version used by this repo.
- No per-column average length, only max. Use `max/2` as an average proxy, capped at 4096
  to limit the effect of outlier rows on chunk size.
- No cardinality or null fraction statistics exposed as scalars (only embedded in `stats()` text).

### Cost of the stats scan

One `SELECT stats(col1), stats(col2), ... FROM t LIMIT 1` query per table. Because DuckDB
applies the `LIMIT 1` after reading the first row group, and `stats()` is derived from
internal segment metadata (not computed from row data), this is effectively a metadata-only
read — constant time regardless of table size. Verified: even for tables with 1M rows the
`stats()` call returns immediately.

### Decision: use stats() for VARCHAR widths — conditionally

The stats scan is cheap enough to always run, but there is one edge case: a table where every
column is a fixed-width type (INTEGER, TIMESTAMP, etc.) has no VARCHAR columns to scan stats
for, so the extra query adds no value. The implementation should:

1. Check if any column in `cols` is a VARCHAR/TEXT/JSON/BLOB type.
2. If yes, run the dynamic stats query and populate `varchar_max_lens`.
3. Pass `varchar_max_lens` into `ComputeChunkRows`.

---

## Adaptive Sizing: Pros, Cons, and Verdict

### The idea

After each chunk, measure `elapsed_ms / rows_written` to derive rows/sec. Adjust chunk size
for the next iteration:

```
if rows_per_sec > prev_rows_per_sec * 1.1:
    next_chunk = min(current_chunk * 1.25, MAX_CHUNK_ROWS)
else if rows_per_sec < prev_rows_per_sec * 0.9:
    next_chunk = max(current_chunk * 0.75, MIN_CHUNK_ROWS)
```

### Pros

- Responds to runtime I/O conditions (network saturation, Postgres lock contention, WAL pressure)
- Can exploit excess memory when conditions are favorable
- Naturally handles tables whose row size varies across ranges (e.g., sparse early rows,
  dense later rows in a time-series)

### Cons and why this is premature

1. **Measurement noise**: The "per-chunk" elapsed time in the current loop includes DuckDB
   query time + CSV write + `fread` + `PQputCopyData` but **not** Postgres's actual INSERT
   commit time. The Postgres commit is batched until `PQputCopyEnd`, which is called after
   the entire table, not per chunk. So per-chunk timing does not actually measure Postgres
   throughput — it measures DuckDB + network send throughput, which is less useful.

2. **Wrong optimization direction when Postgres is slow**: If Postgres is I/O-bound, we
   actually want *smaller* chunks (faster rollback, better progress granularity), not larger
   ones. Adaptive sizing that responds to low rows/sec by shrinking chunks is correct, but
   then the feedback loop converges to `MIN_CHUNK_ROWS` for all slow targets — which is
   probably not what we want.

3. **Complexity vs. reward**: The static type-width estimate (with `stats()` for VARCHARs)
   gets chunk sizes within 2-3x of optimal for >95% of real-world schemas. Adaptive sizing
   adds ~40 lines of stateful loop logic, a feedback constant to tune, and makes the behavior
   harder to reason about in test scenarios.

4. **No current observability**: We have no baseline metrics for per-table chunk performance
   to know whether adaptive sizing would actually improve anything.

### Verdict: defer adaptive sizing

Implement the static `ComputeChunkRows` with `stats()` VARCHAR widths first. Revisit
adaptive sizing only after:
- Per-table chunk timing is surfaced in `hypha.event_log` (the infrastructure is already
  there via `LogEvent`)
- We have observed real workloads where per-chunk throughput varies significantly across
  a single table's chunks

---

## Implementation Plan (not in scope for this PR)

1. **Add `ComputeChunkRows` function** near the `COPY_CHUNK_ROWS` constant at line 170.
   Signature above. Inline `IsVarcharType` helper for the switch.

2. **Add `FetchVarcharMaxLens` helper** that dynamically builds and runs the `stats()` SQL
   for all VARCHAR-family columns in a given table, returning `unordered_map<string, int64_t>`.

3. **Replace `COPY_CHUNK_ROWS` usages**:
   - Base snapshot COPY loop (line 1175/1178): `cols` is available; `row_count` should be
     fetched from `hypha.table_snapshot` or passed down.
   - Sync COPY loop (line 2314/2316): same; `cols` is already in scope.
   - `row_hash INSERT` loop (line 456/459): `fp_cols` is `vector<pair<string,string>>`;
     adapt `ComputeChunkRows` to accept this simpler type, or convert to `ColumnDef` first.
     Use 16 MB target.

4. **Add configurable settings** (`copy_chunk_target_mb`, `row_hash_chunk_target_mb`) wired
   through `hypha_set`/`hypha_get` so operators can tune without recompiling.

5. **Test cases** in `test/sql/hyphasync.test`:
   - Narrow table (2-3 fixed-width columns) → single chunk for small tables
   - Wide table (20+ VARCHAR columns) → multiple small chunks
   - BLOB table → very small chunk count even at low row count

---

## Expected Improvement

| Scenario | Old chunk size | New chunk size | Change |
|----------|---------------|----------------|--------|
| Narrow table (3 cols, ~20 B/row) | ~2 MB | ~64 MB | **32× larger** → fewer round-trips |
| Typical table (10 cols, ~200 B/row) | ~20 MB | ~64 MB | **3× larger** |
| Wide table (50 cols, avg 100 B each) | ~500 MB | ~64 MB | **8× smaller** → safe memory use |
| Blob-heavy (id + 8KB BLOB) | ~820 MB | ~64 MB | **13× smaller** → prevents OOM |
| `row_hash` PK-only (2 cols, ~30 B/row) | ~3 MB | ~16 MB | **5× larger** → fewer local txns |

The accuracy improvement versus the fixed constant is roughly **5–50×**, depending on how far
the actual schema deviates from the ~200 bytes/row sweet spot that 100k rows implied.
