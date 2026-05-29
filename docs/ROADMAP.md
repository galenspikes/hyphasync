# hyphasync roadmap

Phase 0 (current) is an experimental scaffold: local DuckDB metadata only.

## Next phases

### Postgres attach and health checks

- `hypha_attach_check(conn_string)` — validate Postgres connectivity and permissions without mutating remote state beyond read-only probes.

### Remote hypha metadata

- Mirror or reference sync state on the Postgres target (extension-owned `hypha` schema on the remote side).
- Coordinate local `hypha.*` tables with remote bookkeeping.

### Base snapshot planning and execution

- `hypha_base_snapshot_plan()` — diff/plan from local catalog to target schema.
- `hypha_base_snapshot()` — initial full schema + data copy (DuckDB → Postgres only).

### Object lineage and comments

- Persist lineage comments and object-level provenance in metadata tables.
- Tie `hypha.object_snapshot` rows to catalog objects.

### Fingerprints and hashing

- Populate `definition_hash`, `content_hash`, `object_fingerprint`, `table_hash`, and `hypha.row_hash` during capture.
- Use fingerprints for safe incremental sync decisions.

### Incremental sync

- `hypha_sync_plan()` — on-demand snapshot-diff plan (no CDC / WAL / logical replication).
- `hypha_sync()` — apply planned changes to Postgres with safe defaults.
