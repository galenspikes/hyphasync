# AGENTS.md

## Cursor Cloud specific instructions

This is a C++ DuckDB extension (`hyphasync`). It builds via DuckDB's extension-ci-tools CMake/Make system.

### Branching & docs

- `main` is the **publicly consumable** branch: `README.md` + public reference docs in `docs/`.
- `develop` carries planning/roadmap/status/design under `docs/planning/` — **kept off `main`**.
- Edit roadmap/status on `develop` only — its `docs/planning/README.md` explains the model and
  how to promote public changes to `main`. Don't link public docs into `docs/planning/`.
- Distribution is downloadable per-platform binaries (`release.yml` on a `v*` tag). Publishing to
  DuckDB's community-extensions registry is a **non-goal** — load with `--unsigned`.

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

- **libpq required**: Install the Postgres client library + development headers before building.
  - **macOS**: `brew install libpq`, then expose it:
    ```sh
    export CMAKE_PREFIX_PATH="$(brew --prefix libpq):${CMAKE_PREFIX_PATH:-}"
    export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    ```
    CMake also checks `/opt/homebrew/opt/libpq` and `/usr/local/opt/libpq` automatically on macOS.
  - **Linux (Debian/Ubuntu)**: `sudo apt-get update && sudo apt-get install -y libpq-dev` (the `apt-get update` matters — a stale package index can 404 on the pinned libpq-dev version).
- **Compiler**: On **Linux**, set `CC=gcc CXX=g++` — the default Clang 18 cannot find the gcc-13 libstdc++ headers. On **macOS**, use the default Apple Clang (plain `make release`).
- **Submodules required**: The `duckdb/` and `extension-ci-tools/` directories are git submodules. Run `git submodule update --init --recursive` if they are empty.
- **First build is slow** (~10 minutes): DuckDB itself compiles from source. Subsequent incremental builds are fast.
- **Integration tests prefer native Postgres**: `./test/integration/run.sh` checks for native Postgres 16 on port 54329 first. Set it up once with `./scripts/setup-postgres-test.sh`. Docker Compose is a fallback only — if no native Postgres is found on 54329, the script starts a container automatically.
- **Docker daemon for integration test fallback**: The Docker Compose fallback requires the Docker daemon to be running. In Cloud Agent VMs: `sudo dockerd &>/tmp/dockerd.log &` then `sudo chmod 666 /var/run/docker.sock`.
- **Format tools**: `make format-check` requires `pip install black==26.5.1 clang_format==11.0.1 cmake-format==0.6.13` and the `clang-format` system binary. All three are version-pinned (in CI too) because formatter output drifts between releases — an unpinned bump silently breaks `format-check` on unchanged files.
- **Built DuckDB binary auto-loads the extension**: After `make release`, `./build/release/duckdb` has hyphasync linked in — no `LOAD` needed.
- **Unsigned extension**: The built `.duckdb_extension` is not registry-signed. Any external DuckDB process that `LOAD`s it must allow unsigned extensions: `duckdb --unsigned` or `SET allow_unsigned_extensions=true;` in `~/.duckdbrc`. Without this, `LOAD` will fail. Load by full path — `LOAD hyphasync;` looks up the official registry and will not find it.
- **Windows is not a supported target**: The sync path streams DuckDB `COPY` through a Unix pipe (`/dev/fd`) with no Windows equivalent, so `windows_amd64;windows_amd64_mingw` are excluded from the distribution pipeline and the source no longer carries `_WIN32` branches. Build and develop on Linux or macOS.

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
