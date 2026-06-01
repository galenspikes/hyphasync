# Upgrading DuckDB (with hyphasync)

**You probably do not need this yet.** hyphasync is experimental; most users should stay on the DuckDB binary built from this repo until we publish versioned releases with clear compatibility notes.

This page is for when you **do** upgrade DuckDB — or when CRAN `{duckdb}` updates and you wonder whether anything broke.

---

## What is tied to DuckDB version?

| Component | Survives DuckDB upgrade? |
|-----------|--------------------------|
| Your `.duckdb` data files | Yes — DuckDB reads its own files across versions (with normal DuckDB migration rules) |
| Local `hypha.*` metadata in those files | Yes |
| Data already on Postgres | Yes |
| The **hyphasync extension binary** (`.duckdb_extension`) | **No** — must match the DuckDB **minor** version (1.5.x, 1.6.x, …) |

If DuckDB and the extension version do not match, `LOAD hyphasync` fails. That is a DuckDB platform rule, not something hyphasync can hide today.

---

## Recommended workflow today

Use the CLI built with the extension — not `{duckdb}` from CRAN for sync operations:

```sh
# From repo root, after make release
./build/release/duckdb mydb.duckdb -c "SELECT hypha_sync();"
```

From R, call that binary instead of loading the extension in-process:

```r
system2(
  "/path/to/hyphasync/build/release/duckdb",
  args = c("mydb.duckdb", "-c", shQuote("SELECT hypha_sync();"))
)
```

That way R's `{duckdb}` version and hyphasync's DuckDB version are **decoupled**. You can update CRAN `{duckdb}` for analytics without touching sync.

---

## If you upgrade DuckDB anyway

### 1. CLI / built-from-source users

1. Rebuild hyphasync against the new DuckDB tag ([docs/UPDATING.md](UPDATING.md) for maintainers).
2. Use the new `./build/release/duckdb` for all `hypha_*` calls.
3. Your existing `hypha.*` metadata and Postgres data stay valid.
4. Run `SELECT hypha_doctor();` — confirm `metadata_initialized=true` and capabilities look right.
5. Optional smoke test: `SELECT hypha_sync_plan();` on a copy of the database before applying.

### 2. If you were loading a `.duckdb_extension` file

1. Obtain (or rebuild) an extension built for **your** DuckDB version.
2. `LOAD` the new file.
3. Same metadata/postgres story as above.

### 3. Fingerprint algorithm changes (rare)

If `hypha_doctor()` reports a new `fingerprint_algo` (e.g. v2 → v3):

- Incremental sync will refuse to diff against the old baseline.
- Run `SELECT * FROM hypha_base_snapshot();` once to rebaseline Postgres (full re-push of managed schemas).

---

## What we plan for later (not promised yet)

To reduce upgrade anxiety:

- **Versioned extension artifacts** per DuckDB minor, documented in release notes
- **R helper** that wraps the known-good CLI (no manual version pairing)
- **`hypha_doctor()`** reporting whether the loaded extension matches the engine

Until then: **keep sync on the repo-built CLI; use CRAN `{duckdb}` only for read-only queries** if you want zero coupling.

---

## Quick reference

| Situation | Action |
|-----------|--------|
| CRAN `{duckdb}` updated, sync still via CLI | Nothing — ignore CRAN version for sync |
| Rebuilt `./build/release/duckdb` after `git pull` | Use new binary; metadata unchanged |
| `LOAD hyphasync` fails after DuckDB upgrade | Rebuild extension or use repo CLI |
| `hypha_sync()` refuses fingerprint mismatch | `SELECT * FROM hypha_base_snapshot()` to rebaseline |

Questions or breakage: check `SELECT * FROM hypha.event_log ORDER BY event_time DESC LIMIT 20;`
