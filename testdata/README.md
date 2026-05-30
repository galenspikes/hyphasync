# hyphasync testdata

Sample DuckDB databases for local testing and hardening of the extension.
These files are **not committed** (listed in `.gitignore`); download them with
`scripts/download-testdata.sh` or manually using the sources below.

Run the full harness: `scripts/test-sample-dbs.sh`

## Databases

| File | Size | Source | Description |
|------|------|--------|-------------|
| `tpch-sf1.db` | 250 MB | [DuckDB blobs](https://blobs.duckdb.org/data/tpch-sf1.db) | TPC-H benchmark, scale factor 1. 8 tables, 8.6 M rows. Heavy DECIMAL, DATE, CHAR, INTEGER. Classic analytical benchmark with FK relationships. |
| `demo-stock.duckdb` | ~16 KB | [Timestored](https://www.timestored.com/data/duckdb-demo.duckdb) | Small time-series/financial demo database. |
| `ferc6-xbrl.duckdb` | ~78 MB | [PUDL nightly](https://github.com/catalyst-cooperative/pudl) `s3://pudl.catalyst.coop/nightly/ferc6_xbrl.duckdb` | FERC Form 6 (oil pipeline companies) XBRL data, 2021-present. Many narrow tables, INTERVAL/DATE/BOOLEAN/BIGINT types. |
| `ferc60-xbrl.duckdb` | ~55 MB | PUDL nightly `s3://pudl.catalyst.coop/nightly/ferc60_xbrl.duckdb` | FERC Form 60 (natural gas companies) XBRL data. Similar to FERC6 but gas utility schema. |
| `ferc714-xbrl.duckdb` | ~63 MB | PUDL nightly `s3://pudl.catalyst.coop/nightly/ferc714_xbrl.duckdb` | FERC Form 714 (electric power transfer) XBRL data. Energy grid / transmission tables. |
| `stats-stackoverflow.duckdb` | ~618 MB | [Zenodo 19131189](https://zenodo.org/records/19131189) | Stack Overflow Stats re-packaged for DuckDB. Real-world relational data: posts, votes, comments, users, tags. CC-BY-SA 4.0. |

## Download script

```sh
scripts/download-testdata.sh          # download all (needs aws CLI for PUDL)
scripts/download-testdata.sh tpch     # just TPC-H sf1
```

## Test harness

```sh
scripts/test-sample-dbs.sh            # run snapshot plan against every DB in testdata/
```

Requires: Docker + Docker Compose running, built extension (`make release`).
