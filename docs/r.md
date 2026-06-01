# Using hyphasync from R

**Recommended pattern:** call the repo-built DuckDB CLI for sync operations. Do not `LOAD hyphasync` into CRAN `{duckdb}` — the extension binary must match the DuckDB engine minor version, and CRAN updates on its own schedule.

See also: [upgrading-duckdb.md](upgrading-duckdb.md).

---

## Why `system2()` instead of `{DBI}`/`dbExecute()`?

| Approach | Works for sync? |
|----------|---------------|
| `./build/release/duckdb mydb.duckdb -c "SELECT hypha_sync();"` | Yes — extension linked at build time |
| `LOAD hyphasync` inside CRAN `{duckdb}` | Fragile — version mismatch after any DuckDB upgrade |

Decouple analytics (CRAN `{duckdb}`) from sync (repo CLI).

---

## Minimal example

```r
duckdb_cli <- "/path/to/hyphasync/build/release/duckdb"
db_file    <- "/path/to/mydb.duckdb"

run_hypha <- function(sql) {
  out <- system2(
    duckdb_cli,
    args = c(db_file, "-c", shQuote(sql)),
    stdout = TRUE,
    stderr = TRUE
  )
  out
}

# Smoke test
run_hypha("SELECT hypha_doctor();")

# Full sync (Postgres target must already be initialized via hypha_init)
run_hypha("SELECT hypha_sync();")
```

Use absolute paths. The built binary auto-loads hyphasync — no `LOAD` statement needed.

---

## Preview before applying

```r
run_hypha("SELECT hypha_sync_plan();")   # read-only diff report
run_hypha("SELECT hypha_sync();")        # apply
```

Parse the returned `key=value` lines if you need structured output in R:

```r
report <- paste(run_hypha("SELECT hypha_sync_plan();"), collapse = "\n")
grep("^tables_to_sync=", strsplit(report, "\n")[[1]], value = TRUE)
```

---

## Connection strings

Pass Postgres URLs as a single quoted SQL string (shell-safe via `shQuote`):

```r
conn <- "postgresql://user:pass@host:5432/dbname"
run_hypha(sprintf("SELECT hypha_init(%s);", shQuote(conn, type = "cmd")))
run_hypha("SELECT hypha_target_status(NULL);")
```

Never embed raw passwords in R scripts committed to git — use environment variables and `sprintf`/`shQuote` at call time.

---

## What stays in `{duckdb}`

Use CRAN `{duckdb}` for read-only analytics against the same `.duckdb` file:

```r
library(DBI)
con <- dbConnect(duckdb::duckdb(), dbdir = db_file, read_only = TRUE)
dbGetQuery(con, "SELECT * FROM main.my_table LIMIT 10")
dbDisconnect(con, shutdown = TRUE)
```

Open read-only while sync runs via the CLI to avoid file-lock conflicts.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `LOAD hyphasync` fails in `{duckdb}` | Stop loading in-process; use repo CLI via `system2()` |
| CLI not found | Run `CC=gcc CXX=g++ make release` in the hyphasync repo |
| Sync refuses fingerprint mismatch | `SELECT * FROM hypha_base_snapshot();` once to rebaseline (see upgrading doc) |
| Need recent errors | `SELECT * FROM hypha.event_log ORDER BY event_time DESC LIMIT 20;` via CLI |
