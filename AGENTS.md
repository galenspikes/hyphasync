# AGENTS.md

## Cursor Cloud specific instructions

This is a C++ DuckDB extension (`hyphasync`). It builds via DuckDB's extension-ci-tools CMake/Make system.

### Build & Test commands

| Task | Command |
|------|---------|
| Build (release) | `CC=gcc CXX=g++ make release` |
| Run SQL tests | `CC=gcc CXX=g++ make test` |
| Format check | `make format-check` |
| Format fix | `make format-fix` |
| Integration test | `./test/integration/run.sh` |
| Setup local Postgres target (one-time) | `./scripts/setup-postgres-test.sh` |
| Performance benchmark | `BINARY=./build/release/duckdb ./scripts/benchmark-fingerprint.sh 30` |

### Important caveats

- **libpq required (Phase 1+)**: Install Postgres client libraries before building:
  ```sh
  brew install libpq
  export CMAKE_PREFIX_PATH="$(brew --prefix libpq):${CMAKE_PREFIX_PATH:-}"
  export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  ```
  CMake also checks `/opt/homebrew/opt/libpq` and `/usr/local/opt/libpq` automatically on macOS.
- **Must use GCC**: The default compiler is Clang 18 which cannot find libstdc++ headers from gcc-13. Always set `CC=gcc CXX=g++` when building.
- **Submodules required**: The `duckdb/` and `extension-ci-tools/` directories are git submodules. Run `git submodule update --init --recursive` if they are empty.
- **First build is slow** (~10 minutes): DuckDB itself compiles from source. Subsequent incremental builds are fast.
- **Integration tests prefer native Postgres**: `./test/integration/run.sh` checks for native Postgres 16 on port 54329 first. Set it up once with `./scripts/setup-postgres-test.sh`. Docker Compose is a fallback only — if no native Postgres is found on 54329, the script starts a container automatically.
- **Docker daemon for integration test fallback**: The Docker Compose fallback requires the Docker daemon to be running. In Cloud Agent VMs: `sudo dockerd &>/tmp/dockerd.log &` then `sudo chmod 666 /var/run/docker.sock`.
- **Format tools**: `make format-check` requires `pip install "black>=24" clang_format==11.0.1 cmake-format` and the `clang-format` system binary.
- **Built DuckDB binary auto-loads the extension**: After `make release`, `./build/release/duckdb` has hyphasync linked in — no `LOAD` needed.

### Source structure

The sync pipeline lives in `src/` as a set of focused modules (split from the former monolithic `hypha_snapshot.cpp`):

| File | Role |
|------|------|
| `hypha_snapshot_internal.hpp` | Shared internal types and helpers (included by all modules below) |
| `hypha_snapshot_common.cpp` | Common utilities: COPY-via-pipe, Postgres connection helpers |
| `hypha_snapshot_plan.cpp` | `hypha_base_snapshot_plan()` — catalog walk + fingerprinting |
| `hypha_snapshot_pg.cpp` | Low-level Postgres DDL/DML execution (CREATE TABLE, COPY, ALTER) |
| `hypha_snapshot_diff.cpp` | Fingerprint diff and row-level diff logic |
| `hypha_snapshot_base_snapshot.cpp` | `hypha_base_snapshot()` table function |
| `hypha_snapshot_sync.cpp` | `hypha_sync_plan()` and `hypha_sync()` table function |
| `hypha_snapshot.cpp` | Entry point: registers all table functions and scalar shims |

Other source files: `hypha_fingerprint.cpp` (SHA-256 hashing), `hypha_metadata.cpp` (local `hypha.*` schema), `hypha_postgres.cpp` (libpq connection management), `hyphasync_extension.cpp` (DuckDB extension entry point).
