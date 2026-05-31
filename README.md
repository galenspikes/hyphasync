# hyphasync

**hyphasync** is a DuckDB extension that syncs a local DuckDB database to an existing PostgreSQL database — schema-to-schema, DuckDB → Postgres only, using SHA-256 fingerprinted snapshot-diffs with no CDC or WAL required.

## Current status

**Full sync pipeline working (experimental).**

All six workflow functions are implemented. The sync is correct: fingerprinting detects every change including same-count row updates and deletes that row-count-only approaches miss.

| Capability | Status |
|------------|--------|
| Local metadata + Postgres target verification | ✅ |
| Read-only Postgres health probe | ✅ |
| Local catalog snapshot (object/column/table) | ✅ |
| SHA-256 fingerprinting (table_hash, definition_hash) | ✅ |
| Base snapshot: DuckDB → Postgres COPY | ✅ |
| Incremental sync with fingerprint diff | ✅ |
| Row-level diff: single-column PKs | ✅ |
| Row-level diff: composite PKs | ✅ |
| No-PK tables | ✅ TRUNCATE+COPY fallback |
| Remote `hypha` metadata on Postgres | planned |
| Nested type fingerprinting (LIST/STRUCT/MAP) | planned |
| Targeted schema evolution DDL | planned |
| CDC / WAL / streaming replication | non-goal |

## Quick start

Clone with submodules:

```sh
git clone --recurse-submodules https://github.com/galenspikes/hyphasync.git
cd hyphasync
```

On macOS, install and expose libpq before building:

```sh
brew install libpq
export CMAKE_PREFIX_PATH="$(brew --prefix libpq):${CMAKE_PREFIX_PATH:-}"
export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
```

Build:

```sh
CC=gcc CXX=g++ make release
```

### Full workflow

```sql
LOAD hyphasync;

-- 1. Confirm extension is loaded and inspect capabilities
SELECT hypha_hello();
SELECT hypha_doctor();

-- 2. Register a Postgres target (always verifies connection first)
SELECT hypha_init('postgresql://user:pass@host:5432/dbname');

-- 3. Probe the target (read-only; no remote writes)
SELECT hypha_target_status(NULL);   -- uses stored default target
SELECT hypha_target_status('postgresql://...');  -- explicit URL

-- 4. Capture local catalog + fingerprints (no Postgres writes)
SELECT hypha_base_snapshot_plan();
SELECT * FROM hypha.object_snapshot;   -- tables discovered
SELECT * FROM hypha.column_snapshot;   -- columns with DuckDB→Postgres type mapping
SELECT * FROM hypha.table_snapshot;    -- row counts and SHA-256 table_hash

-- 5. Push to Postgres for the first time
SELECT hypha_base_snapshot();
-- Creates schema <dbname>_<duckschema> (e.g. mydb_main) on Postgres,
-- copies every table via COPY FROM STDIN. Re-running drops and re-copies.

-- 6. After changes in DuckDB, see what would sync
SELECT hypha_sync_plan();
-- Detects: new tables, dropped tables, schema changes, data changes
-- (including same-count updates/deletes via table_hash diff)

-- 7. Apply the sync
SELECT hypha_sync();

-- 8. Inspect observability
SELECT * FROM hypha.event_log ORDER BY event_time DESC;
SELECT * FROM hypha.commit ORDER BY created_at DESC;
```

## Building

```sh
CC=gcc CXX=g++ make release
```

Artifacts:

- `./build/release/duckdb` — shell with extension linked in
- `./build/release/test/unittest` — SQL test runner
- `./build/release/extension/hyphasync/hyphasync.duckdb_extension` — loadable binary

Optional faster rebuilds: `GEN=ninja make` (with [ninja](https://ninja-build.org/) and [ccache](https://ccache.dev/) installed).

## Running SQL tests

```sh
make test
```

Extension-specific tests live in `test/sql/hyphasync.test`.

## Integration checks

Requires Docker + Docker Compose.

```sh
./scripts/verify-phase0.sh   # catalog + snapshot + event_log + mutation guard
./scripts/verify-phase1.sh   # deeper probe coverage + type mapping + idempotency
```

## Testing against sample databases

Download real-world DuckDB databases and run the full sync harness against them:

```sh
./scripts/download-testdata.sh        # TPC-H, FERC XBRL, Stack Overflow stats
./scripts/build-frankenstein.sh       # build a combined multi-schema stress DB
./scripts/test-sample-dbs.sh         # full sync each DB in testdata/, verify Postgres
```

## Fingerprinting

hyphasync uses SHA-256 fingerprinting to detect all data changes:

- Every row is hashed with a canonical type-aware encoding (`field_encoding_expr`).
- `table_hash = sha256(sort(row_hashes))` — order-independent and duplicate-correct.
- `definition_hash` covers schema structure (column names, types, nullability).
- `object_fingerprint = sha256(definition_hash + ":" + table_hash)` — one comparison to rule them all.

All hashing is done inside DuckDB (vectorized, parallel) using its built-in `sha256()` function. Fingerprints are never compared cross-engine. See [docs/fingerprinting.md](docs/fingerprinting.md) for the frozen v1 spec.

## Schema mapping

Postgres schemas are named `<duckdb_filename>_<duckdb_schema>`:

| DuckDB | Postgres |
|--------|----------|
| `mydb.duckdb` / `main` | `mydb_main` |
| `analytics.duckdb` / `reports` | `analytics_reports` |

Column types are mapped from DuckDB to Postgres (e.g. `DECIMAL(10,2)` → `numeric(10,2)`, `TIMESTAMPTZ` → `timestamp with time zone`). Unsupported nested types (LIST/STRUCT/MAP) are skipped with a `warn` entry in `hypha.event_log` — planned for a future release.

## Sync behaviour by PK type

| Table PK | Sync method |
|----------|-------------|
| Single-column PK | Targeted DELETE + INSERT for changed rows only |
| Composite PK | Same — compound key used for row identity |
| No PK | TRUNCATE + COPY (correct, not as efficient) |

The sync first computes `table_hash` (SHA-256 of all row hashes) to detect whether any data changed. If the hash matches the prior snapshot, the table is skipped entirely — zero Postgres work regardless of row count. Only changed tables trigger any row-level operations.

## SQL surface

| Function | Status | Description |
|----------|--------|-------------|
| `hypha_hello()` | available | Confirms extension is loaded |
| `hypha_doctor()` | available | Versions, local DB identity, capability status |
| `hypha_init(conn VARCHAR)` | available | Verify Postgres connectivity, create local `hypha` metadata |
| `hypha_target_status(conn VARCHAR)` | available | Read-only Postgres probe (status=ok/degraded/error) |
| `hypha_base_snapshot_plan()` | available | Catalog walk + SHA-256 fingerprinting; no remote writes |
| `hypha_base_snapshot()` | available | Push all tables to Postgres via COPY |
| `hypha_sync_plan()` | available | Fingerprint diff against last applied snapshot |
| `hypha_sync()` | available | Apply diff: new/changed tables COPY, dropped tables DROP |

`hypha_doctor()` reports each capability's status and the active `fingerprint_algo`.

## Local metadata schema

Extension-owned schema: `hypha`

| Table | Contents |
|-------|----------|
| `hypha.target` | Stored Postgres connection strings |
| `hypha.commit` | Snapshot and sync commit history with `fingerprint_algo` |
| `hypha.object_snapshot` | Per-table `definition_hash`, `content_hash`, `object_fingerprint` |
| `hypha.column_snapshot` | Per-column DuckDB→Postgres type mapping |
| `hypha.table_snapshot` | Per-table row count and `table_hash` |
| `hypha.row_hash` | Reserved for v2 row-level diff |
| `hypha.event_log` | All operations logged with level/code/message |
| `hypha.meta` | Config: `metadata_schema_version`, `fingerprint_algo`, `hyphasync_version` |

## Non-goals

- No CDC, DuckDB WAL inspection, or logical transaction logs
- No streaming replication
- No multi-target or non-Postgres databases
- No Postgres → DuckDB sync (one direction only)

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) for planned v2 features.

## License

Apache 2.0 — see [LICENSE](LICENSE).
