# hyphasync

**hyphasync** is a DuckDB extension for local-first scientific and analytical workflows. The long-term goal is to sync a local DuckDB database to an existing PostgreSQL database using a base snapshot followed by on-demand snapshot-diff syncs (DuckDB → Postgres only, schema-to-schema, with lineage/fingerprints and safe defaults).

## Current status

**Phase 0 + Phase 1 (experimental).**

- **Phase 0:** local `hypha` metadata via `hypha_init()` (no remote writes).
- **Phase 1:** read-only Postgres probes via `hypha_attach_check()` (libpq; no remote writes).
- **Not yet:** base snapshot, sync, remote `hypha` metadata, CDC/WAL.

## Quick start

Clone with submodules, then build:

```sh
git clone --recurse-submodules https://github.com/galenspikes/hyphasync.git
cd hyphasync
make
```

On macOS you may need Postgres client libraries, e.g. `brew install libpq` (and export `PKG_CONFIG_PATH` if CMake cannot find libpq). CI uses vcpkg (`libpq` in `vcpkg.json`).

### Basic usage

```sql
LOAD hyphasync;
SELECT hypha_hello();
SELECT hypha_doctor();
SELECT hypha_init('postgresql://user:pass@host:5432/dbname');
SELECT hypha_attach_check('postgresql://user:pass@host:5432/dbname');
-- Or use the stored default target:
SELECT hypha_attach_check(NULL);
```

`hypha_init()` creates the local `hypha` schema and metadata tables, and stores the Postgres connection string in `hypha.target` (default target name: `default`). It is idempotent.

`hypha_attach_check()` connects with libpq, runs **read-only** catalog queries (`version()`, `current_database()`, whether a remote `hypha` schema exists), and returns a multi-line status report. It does not create or modify objects on Postgres.

## Building

```sh
make
```

Artifacts:

- `./build/release/duckdb` — shell with extension linked
- `./build/release/test/unittest` — SQL test runner
- `./build/release/extension/hyphasync/hyphasync.duckdb_extension` — loadable extension binary

Optional faster rebuilds: `GEN=ninja make` (with [ninja](https://ninja-build.org/) and [ccache](https://ccache.dev/) installed).

## Running SQL tests

```sh
make test
```

Extension-specific tests live in `test/sql/hyphasync.test`.

## Loading the extension locally

**Embedded (after `make`):** the built `./build/release/duckdb` binary loads hyphasync automatically.

**Loadable binary:**

```sql
LOAD './build/release/extension/hyphasync/hyphasync.duckdb_extension';
```

You may need `allow_unsigned_extensions` when loading a local artifact outside the official distribution pipeline.

## Local Postgres integration check

Verifies Docker Postgres, `hypha_init()`, local metadata, **read-only** `hypha_attach_check()`, and that Postgres is not mutated by hyphasync:

```sh
./scripts/verify-phase0.sh
```

Requires Docker and Docker Compose.

## Non-goals (current)

- No CDC, DuckDB WAL inspection, or logical transaction logs
- No streaming replication
- No multi-target or non-Postgres databases
- No base snapshot or sync implementation yet
- No remote DDL/DML from hyphasync (attach check is read-only)

## SQL surface

| Function | Description |
|----------|-------------|
| `hypha_hello()` | Confirms the extension is loaded |
| `hypha_doctor()` | Version, DuckDB version, metadata init flag, capability note |
| `hypha_init(conn_string VARCHAR)` | Create local `hypha` metadata; store Postgres URL in `hypha.target` |
| `hypha_attach_check(conn_string VARCHAR)` | Read-only Postgres connectivity / inventory probe |

## Local metadata schema

Extension-owned schema: `hypha` (tables: `target`, `commit`, `object_snapshot`, `column_snapshot`, `table_snapshot`, `row_hash`, `event_log`).

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) for planned functions and phases.

## License

Apache 2.0 — see [LICENSE](LICENSE).
