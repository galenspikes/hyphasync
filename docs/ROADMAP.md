# hyphasync roadmap

## Completed

### Phase 0 — local metadata scaffold
- `hypha_hello()`, `hypha_doctor()`, `hypha_init()`
- Local `hypha` schema and metadata tables
- `hypha_init()` always verifies Postgres connectivity before writing anything
- Password redaction in all output; `hypha.event_log` live; `hypha.meta` config table

### Phase 1 — Postgres attach and health checks
- `hypha_target_status(conn_string)` — read-only libpq probe (connect, `version()`, remote `hypha` schema inventory)
- Throws on hard failures; returns `status=ok|degraded` with SQLSTATE on connected-but-broken probes

### Phase 2 — catalog snapshot and fingerprinting
- `hypha_base_snapshot_plan()` — full local catalog walk; populates `object_snapshot`, `column_snapshot`, `table_snapshot`
- SHA-256 fingerprinting (v1): `table_hash`, `definition_hash`, `object_fingerprint`
- Frozen spec: [docs/fingerprinting.md](fingerprinting.md)
- DuckDB→Postgres type mapper covering all common scalar types
- `fingerprint_algo` recorded on every `hypha.commit`

### Phase 3 — base snapshot (DuckDB → Postgres)
- `hypha_base_snapshot()` — creates Postgres schemas (`<dbname>_<schema>`), copies all tables via `COPY FROM STDIN`
- `SET STORAGE EXTERNAL` for text columns avoids Postgres 8 KB row-size limit on wide tables
- Per-table savepoints: wide/unsupported tables skip with `event_log` warn, rest succeed
- Identifier truncation with collision disambiguation for Postgres's 63-char limit

### Phase 4 — incremental sync
- `hypha_sync_plan()` — fingerprint-first diff: `object_fingerprint` → `definition_hash` → `table_hash` → row_count
- `hypha_sync()` — applies plan: new tables CREATE+COPY, dropped tables DROP, data changes TRUNCATE+COPY
- Detects all changes including same-count row updates/deletes via `table_hash` diff
- Graceful fallback to column-signature + row-count for pre-fingerprint snapshots
- Version-mismatch warning when syncing against an unfingerprinted prior snapshot

## Next (v2)

### Row-level diff
- Populate `hypha.row_hash` with per-row hashes keyed by `pk_json`
- Generate surgical `INSERT / UPDATE / DELETE` statements instead of TRUNCATE+COPY
- Requires PK detection and keyless-table policy (full-row identity, documented in fingerprinting spec)

### Remote `hypha` metadata
- Mirror sync state on the Postgres target (extension-owned `hypha` schema on the remote side)
- Enable resumable syncs and remote audit history

### Object lineage and comments
- Persist lineage comments and object-level provenance
- Tie `hypha.object_snapshot` rows to catalog objects

### Fingerprint version migration
- Detect `fingerprint_algo` mismatch between old and new snapshots
- Force full re-snapshot on version mismatch rather than a partial ambiguous diff

### Watermark-column optimization
- Optional user-declared `updated_at` column to bound row scans
- Avoids full-table hash on tables with a reliable watermark
