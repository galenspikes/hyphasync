#pragma once

#include "duckdb/main/connection.hpp"
#include <string>

namespace duckdb {

//! Walks the local DuckDB catalog (current database, all non-internal non-temporary
//! non-hypha schemas) and populates hypha.{object,column,table}_snapshot.
//! Creates a hypha.commit record (kind='base_snapshot_plan') and updates event_log.
//! Does NOT connect to Postgres and does NOT compute hashes — see docs/fingerprinting.md
//! for when hashes will be added. Returns a multi-line status report.
//! Throws if hypha metadata is not initialized (call hypha_init() first).
std::string RunBaseSnapshotPlan(Connection &con);

//! Diffs the current local catalog against the last applied snapshot, returning a
//! structured plan report (new/dropped/schema_changed/rows_changed/likely_unchanged tables).
//! Creates a hypha.commit record (kind='sync_plan', status='planned'). No Postgres writes.
//! Throws if no prior applied snapshot exists (call hypha_base_snapshot() first).
std::string RunSyncPlan(Connection &con);

//! Runs a fresh sync plan then applies it to the Postgres target:
//! schema changes get DROP+CREATE+COPY, row-changed tables get TRUNCATE+COPY,
//! dropped tables get DROP TABLE, and likely-unchanged tables are skipped.
//! Without fingerprints, tables with matching row counts may have undetected
//! row-level changes (updates/deletes) — the report notes this explicitly.
//! Throws if no prior applied snapshot exists.
std::string RunSync(Connection &con);

//! Runs a fresh snapshot plan, then creates schemas/tables on the stored Postgres target
//! and copies all data via COPY FROM STDIN (DuckDB -> temp CSV -> Postgres). Each DuckDB
//! schema is created in Postgres as "<db_name>_<schema>" (e.g. "ferc60_xbrl_main").
//! Re-running drops and re-copies every table — no partial-update logic yet.
//! Does NOT compute hashes (see docs/fingerprinting.md for when hashes will be added).
//! Returns a multi-line status report.
//! Throws if hypha metadata is not initialized or the Postgres connection fails.
std::string RunBaseSnapshot(Connection &con);

} // namespace duckdb
