# hyphasync

**hyphasync** publishes a DuckDB database to PostgreSQL — and on every run after the first, it copies only what changed.

It fingerprints each table with SHA-256 and pushes only the tables — and, where it can, only the rows — that differ from the last sync. No CDC, no WAL, no triggers, no replication slots: just a snapshot diff computed entirely inside DuckDB. It is one-directional (DuckDB → Postgres) and schema-to-schema — point it at an existing Postgres database and it mirrors your DuckDB tables into a dedicated schema there.

## Why not just DuckDB's `postgres` extension?

DuckDB can already `ATTACH` a Postgres database and write to it with `CREATE TABLE pg.t AS SELECT * FROM t`. That is the right tool for a **one-shot copy**. hyphasync is built for the **repeated** case — a DuckDB database you rebuild or update and re-publish to Postgres on a schedule:

| | DuckDB `postgres` extension | hyphasync |
|---|---|---|
| One-time copy | ✅ ideal | ✅ works (`hypha_base_snapshot()`) |
| Re-sync after changes | Re-copies the whole table | Copies only changed tables/rows via fingerprint diff |
| Change detection | None — you decide what to re-copy | SHA-256 snapshot diff, automatic |
| Schema evolution | Manual | ADD/DROP COLUMN applied automatically |
| Bookkeeping / audit | None | `hypha.event_log`, commit history, remote `hypha.sync_log` |
| Direction | Read & write, either way | DuckDB → Postgres only |

If you copy once, use the built-in extension. If you publish the same DuckDB database to Postgres again and again, hyphasync exists to make every run after the first cheap.

## Current status

**Full sync pipeline working (experimental).**

All workflow functions are implemented and end-to-end tested.

| Capability | Status |
|------------|--------|
| Local metadata + Postgres target verification | ✅ |
| Read-only Postgres health probe | ✅ |
| Local catalog snapshot (object/column/table) | ✅ |
| SHA-256 fingerprinting v3 — EXACT/APPEND_ONLY/MUTABLE_ENTITY strategies | ✅ |
| Base snapshot: DuckDB → Postgres COPY | ✅ |
| Incremental sync with fingerprint diff | ✅ |
| Row-level diff: single and composite PKs | ✅ |
| No-PK tables | ✅ TRUNCATE+COPY fallback (logged to event_log) |
| Nested types: LIST/STRUCT/MAP → `jsonb` | ✅ requires `json` extension |
| Targeted schema evolution: ADD/DROP COLUMN without DROP+CREATE | ✅ |
| Remote `hypha` metadata on Postgres | ✅ `hypha.sync_log` + `hypha.object_state` |
| tables_failed / rows_failed tracking with WARNING output | ✅ |
| Real-time event_log mirroring to Postgres | ✅ dedicated autocommit connection |
| CDC / WAL / streaming replication | non-goal |

## Platform support

| Platform | Status |
|----------|--------|
| Linux x86-64 | ✅ Supported — built, smoke-tested, and integration-tested in CI on every push |
| macOS (Apple Silicon + Intel) | ✅ Supported — built and smoke-tested in CI on every push |
| Windows | ❌ Not supported |

**Windows is not supported.** hyphasync streams DuckDB's `COPY` output to Postgres through a Unix pipe addressed via `/dev/fd`, which has no Windows equivalent, so the data-sync path cannot run there. Windows binaries are not built or distributed — use Linux or macOS.

## Quick start

### Download a prebuilt binary

Tagged releases ship prebuilt, loadable extension binaries — no compiling required. They are built with DuckDB's distribution pipeline (Linux builds run in a manylinux container with statically-linked libpq), so they're portable across distros. Grab the one for your platform from the [latest release](https://github.com/galenspikes/hyphasync/releases/latest):

```sh
# Linux x86-64
curl -L -o hyphasync.duckdb_extension \
  https://github.com/galenspikes/hyphasync/releases/latest/download/hyphasync-linux_amd64.duckdb_extension

# macOS Apple Silicon
curl -L -o hyphasync.duckdb_extension \
  https://github.com/galenspikes/hyphasync/releases/latest/download/hyphasync-osx_arm64.duckdb_extension
```

`linux_arm64` and `osx_amd64` builds are published on each release too.

The binary is unsigned and tied to a specific DuckDB minor version, so load it into a matching DuckDB with unsigned extensions allowed:

```sh
duckdb --unsigned
```
```sql
LOAD '/full/path/to/hyphasync.duckdb_extension';
SELECT hypha_hello();
```

To build from source instead, read on.

### Build from source

Clone with submodules:

```sh
git clone --recurse-submodules https://github.com/galenspikes/hyphasync.git
cd hyphasync
```

### Prerequisites

hyphasync links against **libpq** (the PostgreSQL client library) and compiles DuckDB from source, so you need a C++17 toolchain and libpq's development headers.

**macOS** (Apple Silicon or Intel) — install and expose libpq:

```sh
brew install libpq
export CMAKE_PREFIX_PATH="$(brew --prefix libpq):${CMAKE_PREFIX_PATH:-}"
export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
```

**Linux** (Debian/Ubuntu):

```sh
sudo apt-get update && sudo apt-get install -y libpq-dev
```

**Linux** (Fedora/RHEL/CentOS):

```sh
sudo dnf install libpq-devel    # older distros: sudo yum install postgresql-devel
```

### Build

```sh
# Linux: force GCC. The default Clang cannot find the gcc libstdc++ headers.
CC=gcc CXX=g++ make release

# macOS: use the default Apple Clang toolchain.
make release
```

The first build takes ~10 minutes (DuckDB compiles from source); incremental rebuilds are fast.

> **Unsigned extension — `--unsigned` required for external DuckDB:** hyphasync is not in the DuckDB extension registry, so the built `.duckdb_extension` binary is unsigned. Any DuckDB process that tries to `LOAD` it must allow unsigned extensions, or the load will fail.
>
> ```sh
> # Option A — pass the flag each time
> duckdb --unsigned mydb.duckdb
>
> # Option B — add to ~/.duckdbrc (applies to every session automatically)
> SET allow_unsigned_extensions=true;
> ```
>
> Then load by **full path** (not `LOAD hyphasync;`, which looks up the official registry):
>
> ```sql
> LOAD '/full/path/to/build/release/extension/hyphasync/hyphasync.duckdb_extension';
> ```
>
> The repo-built `./build/release/duckdb` has the extension linked in and needs no `LOAD` at all.

### Full workflow

```sql
-- Using the repo-built ./build/release/duckdb: extension is already linked in, no LOAD needed.
-- Using your own DuckDB installation: start with `duckdb --unsigned` and then run:
--   LOAD '/full/path/to/build/release/extension/hyphasync/hyphasync.duckdb_extension';

-- Optional: install JSON extension for LIST/STRUCT/MAP column support
INSTALL json; LOAD json;

-- 1. Confirm extension is loaded and inspect capabilities
SELECT hypha_hello();
SELECT hypha_doctor();

-- 2. Register a Postgres target (always verifies connection first)
SELECT hypha_init('postgresql://user:pass@host:5432/dbname');

-- 3. Probe the target (read-only; no remote writes)
SELECT hypha_target_status(NULL);           -- uses stored default target
SELECT hypha_target_status('postgresql://...'); -- explicit URL

-- 4. Capture local catalog + fingerprints (no Postgres writes)
SELECT hypha_base_snapshot_plan();
SELECT * FROM hypha.object_snapshot;   -- tables discovered
SELECT * FROM hypha.column_snapshot;   -- columns with DuckDB→Postgres type mapping
SELECT * FROM hypha.table_snapshot;    -- row counts and SHA-256 table_hash

-- 5. Push to Postgres for the first time
SELECT * FROM hypha_base_snapshot();
-- Creates schema <dbname>_<duckschema> (e.g. mydb_main) on Postgres,
-- copies every table via COPY FROM STDIN. Returns one row per table.

-- 6. After changes in DuckDB, preview what would sync
SELECT hypha_sync_plan();

-- 7. Apply the sync
SELECT * FROM hypha_sync();

-- 8. Inspect observability
SELECT * FROM hypha.event_log ORDER BY event_time DESC;
SELECT * FROM hypha.commit ORDER BY created_at DESC;
```

## Building

```sh
CC=gcc CXX=g++ make release   # Linux
make release                  # macOS (default Apple Clang toolchain)
```

Artifacts:

- `./build/release/duckdb` — DuckDB shell with extension linked in (no `LOAD` needed)
- `./build/release/test/unittest` — SQL test runner
- `./build/release/extension/hyphasync/hyphasync.duckdb_extension` — loadable binary

Optional faster rebuilds: `GEN=ninja make` (with [ninja](https://ninja-build.org/) and [ccache](https://ccache.dev/) installed).

## Smoke test

The quickest "is it working?" check. It loads the extension, runs the diagnostics, and performs a real base snapshot + incremental sync against a live Postgres target:

```sh
./scripts/smoke-test.sh          # uses an existing ./build/release/duckdb
./scripts/smoke-test.sh --build  # build first, then smoke test
```

It targets `postgresql://hypha:hypha@127.0.0.1:54329/hypha_test` by default (override with `HYPHA_PG_URL`) and, on Linux, starts the docker-compose Postgres service automatically if nothing is listening. The integration suite below is the thorough version.

## Running SQL tests

```sh
make test
```

Extension-specific tests live in `test/sql/hyphasync.test`.

## Integration tests

The integration test suite runs against a Postgres 16 instance on port 54329. The script auto-detects whether native Postgres is available and falls back to Docker Compose if not.

### Option A — Docker Compose (no local Postgres required)

```sh
./test/integration/run.sh
```

Docker must be running. The script starts the Postgres container from `docker-compose.yml` automatically.

### Option B — Native Postgres

If you already have Postgres 16 running locally, provision the test user and database first (one-time, idempotent):

```sh
./scripts/setup-postgres-test.sh        # creates hypha role + hypha_test db on port 54329
./test/integration/run.sh
```

### What it covers

Init, snapshot plan, type mapping, fingerprinting, base snapshot push, incremental sync, schema evolution, nested types, remote metadata, composite PKs, large table fingerprint strategies, max-rows guard, and basic resilience.

## Testing against sample databases

```sh
./scripts/download-testdata.sh        # TPC-H, FERC XBRL, Stack Overflow stats
./scripts/build-frankenstein.sh       # build a combined multi-schema stress DB
./scripts/test-sample-dbs.sh         # full sync each DB in testdata/, verify Postgres
```

## Fingerprinting

hyphasync uses SHA-256 fingerprinting (algorithm version `v3`) to detect all data changes:

- Every row is hashed with a canonical type-aware encoding per `docs/fingerprinting.md`.
- `table_hash = sha256(sort(row_hashes))` — order-independent and duplicate-correct.
- `definition_hash` covers schema structure (column names, types, nullability, PK columns).
- `object_fingerprint = sha256(definition_hash + ":" + table_hash)` — one comparison answers "did anything change?"

All hashing runs inside DuckDB (never Postgres). Nested types (LIST/STRUCT/MAP) use DuckDB's JSON serialization as the canonical payload (tags `L`/`R`/`M`). See [docs/fingerprinting.md](docs/fingerprinting.md).

v3 uses a cost-based strategy classifier per table:

| Strategy | When used | Cost |
|----------|-----------|------|
| **EXACT** | Estimated serialized size < 1 MB | O(n) full per-row SHA-256 (fast in practice — table is small) |
| **APPEND_ONLY** | Table has a monotonic integer PK or timestamp column | O(1) via `COUNT(*) + MAX(pk)` |
| **MUTABLE_ENTITY** | Everything else | O(1) via `COUNT(*) + MIN/MAX(rowid)` zone-map statistics |

If you have a baseline from an older algorithm version, run `hypha_base_snapshot()` once to re-establish a v3 baseline. `hypha_sync()` refuses to diff across algorithm versions and tells you exactly what to do.

### Verifying against the blind spot

The O(1) `MUTABLE_ENTITY` strategy has a documented blind spot: an in-place update that leaves `COUNT(*)`, `MIN(rowid)`, and `MAX(rowid)` unchanged is not detected, so `hypha_sync()` would skip the table and leave Postgres silently stale. Two opt-in tools close this gap:

**1. `exact_verify` mode — guaranteed-correct, slower.** Force full per-row SHA-256 hashing for every table regardless of size:

```sql
SELECT hypha_init('postgresql://...', 0, false, true);  -- 4th arg = exact_verify
```

Every `hypha_base_snapshot_plan()` / `hypha_sync()` now classifies all tables as `EXACT`. No table ever uses rowid statistics, so no change is ever missed — at the cost of an O(n) scan + hash per table. Use this when correctness matters more than throughput.

**2. `hypha_verify()` — an on-demand tripwire, fast path stays fast.** Leave fingerprinting on the default fast strategies, and periodically run:

```sql
SELECT hypha_verify();
```

It recomputes the EXACT hash of every table and compares it to a baseline stored in `hypha.verify_state`, classifying each changed table:

- **`BLIND_SPOT_DRIFT`** — the table changed in-place but the fast fingerprint did **not** notice. Postgres is stale; run `hypha_base_snapshot()` to reconcile. Logged at `warn` to `hypha.event_log` (code `VERIFY_DRIFT`).
- **`PENDING`** — the table changed and the fast fingerprint **did** notice; a normal `hypha_sync()` will apply it.
- **armed / unchanged** — first observation establishes the baseline; matching hashes are clean.

Each run advances the baseline, so `hypha_verify()` measures "in-place change since the last verify." Run it as part of your post-sync routine to keep the tripwire armed. It needs no Postgres connection — the O(n) exact scan is the explicit cost of verifying.

From R, prefer the repo CLI over loading the extension in CRAN `{duckdb}` — see [docs/r.md](docs/r.md).

## Type mapping

| DuckDB | Postgres |
|--------|----------|
| `INTEGER`, `INT4` | `integer` |
| `BIGINT` | `bigint` |
| `DECIMAL(p,s)` | `numeric(p,s)` |
| `DOUBLE`, `FLOAT` | `double precision`, `real` |
| `VARCHAR`, `TEXT` | `text` |
| `TIMESTAMP` / `TIMESTAMPTZ` | `timestamp without/with time zone` |
| `TIMESTAMP_S` / `TIMESTAMP_MS` / `TIMESTAMP_NS` | `timestamp without time zone` (sub-second precision is coerced; logged as TYPE_COERCE) |
| `DATE`, `TIME`, `TIMETZ` | `date`, `time without/with time zone` |
| `UUID`, `BOOLEAN`, `BLOB` | `uuid`, `boolean`, `bytea` |
| `INTERVAL`, `BIT` | `interval`, `bit varying` |
| `JSON` | `jsonb` |
| `T[]`, `LIST(T)`, `STRUCT(...)`, `MAP(...)` | `jsonb` (requires `json` extension) |

Columns whose types have no safe mapping are logged to `hypha.event_log` and excluded from the push; the rest of the table syncs normally.

## Schema mapping and ownership

Postgres schemas are named `<duckdb_filename>_<duckdb_schema>`:

| DuckDB | Postgres |
|--------|----------|
| `mydb.duckdb` / `main` | `mydb_main` |
| `analytics.duckdb` / `reports` | `analytics_reports` |

**hyphasync exclusively owns its target Postgres schemas.** Running `hypha_base_snapshot()` drops and recreates every table in those schemas. If the schema already contains tables not created by a prior hyphasync push, a `SCHEMA_OWNERSHIP_WARNING` is emitted to `hypha.event_log` before overwriting. Do not point hyphasync at a schema that holds data you care about outside of hyphasync.

## Sync behaviour by PK type

| Table PK | Sync method |
|----------|-------------|
| Single-column PK | Targeted DELETE + INSERT for changed rows only |
| Composite PK | Same — compound key used for row identity |
| No PK (insert-only change) | Append-only fast path — COPY just the new rows, no TRUNCATE (logged as `KEYLESS_APPEND`) |
| No PK (any delete/update) | TRUNCATE + COPY (correct, but full table; logged as `TRUNCATE_COPY` in event_log) |

## Remote metadata on Postgres

After every successful push or sync, hyphasync writes bookkeeping rows into a `hypha` schema on the Postgres target:

| Table | Contents |
|-------|----------|
| `hypha.sync_log` | One row per push/sync: source database, commit IDs, table/row counts |
| `hypha.object_state` | Current fingerprint state of each synced table |

Use `hypha_target_status()` to inspect `remote_hypha_sync_log` and `remote_hypha_object_state`.

## SQL surface

| Function | Return type | Description |
|----------|-------------|-------------|
| `hypha_hello()` | `VARCHAR` | Confirms extension is loaded |
| `hypha_doctor()` | `VARCHAR` | Versions, local DB identity, capability inventory |
| `hypha_init(conn VARCHAR)` | `BOOLEAN` | Verify Postgres connectivity, create local `hypha` metadata |
| `hypha_init(conn VARCHAR, max_rows BIGINT)` | `BOOLEAN` | Same; skip tables with row_count > max_rows |
| `hypha_init(conn VARCHAR, max_rows BIGINT, fast_mode BOOLEAN)` | `BOOLEAN` | Same; fast_mode=true sets `synchronous_commit=off` (see note below) |
| `hypha_init(conn VARCHAR, max_rows BIGINT, fast_mode BOOLEAN, exact_verify BOOLEAN)` | `BOOLEAN` | Same; exact_verify=true forces full per-row EXACT fingerprinting for every table (see note below) |
| `hypha_target_status(conn VARCHAR)` | `VARCHAR` | Read-only Postgres probe (status=ok/degraded) |
| `hypha_base_snapshot_plan()` | `VARCHAR` | Catalog walk + SHA-256 fingerprinting; no remote writes |
| `SELECT * FROM hypha_base_snapshot()` | `TABLE(table_name VARCHAR, row_count BIGINT, fingerprint_strategy VARCHAR, duration_ms DOUBLE, status VARCHAR)` | Push all tables to Postgres via COPY; one row per table |
| `hypha_sync_plan()` | `VARCHAR` | Fingerprint diff against last applied snapshot; no remote writes |
| `SELECT * FROM hypha_sync()` | `TABLE(table_name VARCHAR, action VARCHAR, rows_synced BIGINT, duration_ms DOUBLE, status VARCHAR)` | Apply incremental sync; one row per changed table |
| `hypha_status()` | `VARCHAR` | One-line summary of the last sync (commit, kind, timestamp, table counts); no Postgres connection required |
| `hypha_verify()` | `VARCHAR` | Exact full per-row reconciliation tripwire; detects in-place changes the fast fingerprint can miss (see below) |
| `hypha_drop([drop_meta BOOLEAN])` | `VARCHAR` | Drop all hyphasync-owned schemas from the stored Postgres target; `hypha_drop(true)` also drops the `hypha` meta schema |
| `hypha_help([name VARCHAR])` | `VARCHAR` | List all functions or describe a specific one |

> **fast_mode note:** `fast_mode=true` sets `synchronous_commit=off` on every Postgres connection opened by hyphasync. This skips WAL flushing on commit, which can significantly speed up large base snapshots and syncs, but means committed data **can be lost on a Postgres crash** (the last few committed pages may not reach disk). This is safe when hyphasync is used purely as a read replica — a lost commit can be recovered by re-running `hypha_base_snapshot()`.

> **exact_verify note:** `exact_verify=true` forces the full per-row `EXACT` fingerprint strategy for every table, closing the `MUTABLE_ENTITY` in-place-update blind spot (see [Verifying against the blind spot](#verifying-against-the-blind-spot)). It guarantees no change is ever missed, at the cost of an O(n) scan + SHA-256 per table on every snapshot and sync. Prefer leaving it off and running `hypha_verify()` periodically if you only need occasional reconciliation rather than always-exact fingerprints.

See [docs/functions.md](docs/functions.md) for full reference: arguments, return value format, event_log codes, and use cases for each function.

## Local metadata schema

Extension-owned schema: `hypha`

| Table | Contents |
|-------|----------|
| `hypha.target` | Stored Postgres connection strings |
| `hypha.commit` | Snapshot and sync commit history with `fingerprint_algo` |
| `hypha.object_snapshot` | Per-table `definition_hash`, `content_hash`, `object_fingerprint` |
| `hypha.column_snapshot` | Per-column DuckDB→Postgres type mapping |
| `hypha.table_snapshot` | Per-table row count and `table_hash` |
| `hypha.row_hash` | Per-row PK+hash pairs for row-level diff |
| `hypha.event_log` | All operations logged with level/code/message |
| `hypha.verify_state` | Per-table exact-hash baseline for the `hypha_verify()` tripwire |
| `hypha.meta` | Config: `metadata_schema_version`, `fingerprint_algo`, `hyphasync_version`, `fast_mode`, `exact_verify` |

## Limitations & known issues

- **In-place update blind spot for MUTABLE_ENTITY:** The `MUTABLE_ENTITY` fingerprint strategy (used for large tables) tracks row-ID statistics. An in-place update that replaces one value with a value of identical statistical signature (same min/max/count) will not be detected as a change. **Two opt-in mitigations:** (1) `hypha_init(conn, max_rows, fast_mode, exact_verify := true)` forces full per-row EXACT hashing for every table, eliminating the blind spot at an O(n) scan cost per snapshot/sync; (2) `hypha_verify()` is an on-demand tripwire that exact-hashes every table and reports any in-place drift the fast fingerprint would miss. See [Verifying against the blind spot](#verifying-against-the-blind-spot).
- **Keyless tables — append-only fast path, else full re-copy:** Tables without a primary key cannot use targeted row-level diff (no key to DELETE/UPDATE by). When such a table only *gains* rows since the last sync, hyphasync takes an append-only fast path: it COPYs just the new rows (identified by content hash) with no TRUNCATE (`KEYLESS_APPEND`). When any row is deleted or updated — which can't be reconstructed and targeted without a key — it falls back to a correct full `TRUNCATE+COPY`. Unchanged keyless tables are skipped entirely via `table_hash`.
- **`synchronous_commit=off` data-loss window:** When `fast_mode=true`, committed Postgres data may be lost if Postgres crashes before the WAL is flushed to disk. Safe only for read-replica use cases; recoverable by re-running `hypha_base_snapshot()`.
- **OFFSET pagination cost for non-integer PKs:** Tables without a single-column integer PK use `LIMIT/OFFSET` for chunk pagination during COPY, which is O(n) in the offset depth. For very large tables with composite or non-integer PKs, the last chunks may be slow.
- **Postgres 8 KB row limit for wide fixed-width tables:** Tables with many integer/numeric/date columns (hundreds of fixed-width columns) can exceed Postgres's 8 KB per-heap-row limit even after `SET STORAGE EXTERNAL` on varlena columns. Affected tables fail COPY with "row is too big" and are logged as `TABLE_FAIL` with the error message. Text-heavy tables are not affected (varlena columns are stored out-of-line via TOAST).
- **`hypha_init()` renders a result box:** Because `hypha_init` is a scalar function returning BOOLEAN, DuckDB renders a small result box. The useful output (connection status) is on stderr. Use `SELECT hypha_init(...) IS NOT NULL;` or pipe stderr to suppress the box.

## Non-goals

- No CDC, DuckDB WAL inspection, or logical transaction logs
- No streaming replication
- No multi-target or non-Postgres databases
- No Postgres → DuckDB sync (one direction only)

## Roadmap

Experimental — quality and large-workload validation come before new features or release packaging. See [docs/PROJECT_STATUS_AND_RECOMMENDATIONS.md](docs/PROJECT_STATUS_AND_RECOMMENDATIONS.md) (summary index: [docs/ROADMAP.md](docs/ROADMAP.md)).

If you upgrade DuckDB later: [docs/upgrading-duckdb.md](docs/upgrading-duckdb.md). For day-to-day use, prefer the repo-built `./build/release/duckdb` CLI over loading the extension into CRAN `{duckdb}`.

## License

Apache 2.0 — see [LICENSE](LICENSE).
