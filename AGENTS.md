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
| Integration test | `./scripts/verify-phase0.sh` |

### Important caveats

- **Must use GCC**: The default compiler is Clang 18 which cannot find libstdc++ headers from gcc-13. Always set `CC=gcc CXX=g++` when building.
- **Submodules required**: The `duckdb/` and `extension-ci-tools/` directories are git submodules. Run `git submodule update --init --recursive` if they are empty.
- **First build is slow** (~10 minutes): DuckDB itself compiles from source. Subsequent incremental builds are fast.
- **Docker required for integration tests**: `./scripts/verify-phase0.sh` needs Docker + Docker Compose to run a local Postgres 16 container on port 54329.
- **Format tools**: `make format-check` requires `pip install "black>=24" clang_format==11.0.1 cmake-format` and the `clang-format` system binary.
- **Built DuckDB binary auto-loads the extension**: After `make release`, `./build/release/duckdb` has hyphasync linked in — no `LOAD` needed.
- **Docker daemon startup**: In Cloud Agent VMs, start dockerd with `sudo dockerd &>/tmp/dockerd.log &` and ensure `sudo chmod 666 /var/run/docker.sock` for non-root access.
