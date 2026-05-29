# hyphasync

**hyphasync** is a DuckDB extension for local-first scientific and analytical workflows. The long-term goal is to sync a local DuckDB database to an existing PostgreSQL database using a base snapshot followed by on-demand snapshot-diff syncs (DuckDB → Postgres only, schema-to-schema, with lineage/fingerprints and safe defaults).

## Current status

**Experimental scaffold only (Phase 0).** The extension builds, loads, and creates **local** `hypha` metadata in DuckDB. It does **not** connect to Postgres, perform snapshots, or copy data yet.

## Quick start

Clone with submodules, then build:

```sh
git clone --recurse-submodules https://github.com/<you>/hyphasync.git
cd hyphasync
make
```

### Basic usage

```sql
LOAD hyphasync;
SELECT hypha_hello();
SELECT hypha_doctor();
SELECT hypha_init('postgresql://user@host:5432/dbname');
```

`hypha_init()` creates the local `hypha` schema and metadata tables, and stores the Postgres connection string in `hypha.target` (default target name: `default`). It is idempotent.

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

## Local Postgres integration check (Phase 0)

Verifies Docker Postgres is up, builds the extension, runs `hypha_init()` with a local connection string, checks DuckDB metadata, and confirms **no** remote `hypha` schema or other mutations from the extension:

```sh
./scripts/verify-phase0.sh
```

Requires Docker and Docker Compose.

## Non-goals (Phase 0)

- No CDC, DuckDB WAL inspection, or logical transaction logs
- No streaming replication
- No multi-target or non-Postgres databases
- No base snapshot or sync implementation yet
- No remote database mutations

## SQL surface (Phase 0)

| Function | Description |
|----------|-------------|
| `hypha_hello()` | Confirms the extension is loaded |
| `hypha_doctor()` | Version, DuckDB version, metadata init flag, capability note |
| `hypha_init(conn_string VARCHAR)` | Create local `hypha` metadata; store Postgres URL in `hypha.target` |

## Local metadata schema

Extension-owned schema: `hypha` (tables: `target`, `commit`, `object_snapshot`, `column_snapshot`, `table_snapshot`, `row_hash`, `event_log`).

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) for planned functions and phases.

## License

Apache 2.0 — see [LICENSE](LICENSE).
