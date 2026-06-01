#pragma once

#include "duckdb/main/connection.hpp"
#include <string>

namespace duckdb {

// Forward-declare ExtensionLoader to avoid pulling in all of duckdb.hpp here.
class ExtensionLoader;

//! Register the hypha_base_snapshot() TableFunction with the extension loader.
//! Replaces the old scalar-function registration.
void RegisterBaseSnapshotTableFunction(ExtensionLoader &loader);

//! Walks the local DuckDB catalog (current database, all non-internal non-temporary
//! non-hypha schemas) and populates hypha.{object,column,table}_snapshot.
//! Creates a hypha.commit record (kind='base_snapshot_plan') and updates event_log.
//! When is_base_snapshot=true, applies full strategy classification (EXACT, APPEND_ONLY,
//! MUTABLE_ENTITY) — same as incremental sync. The flag is kept for API compatibility.
//! Returns a multi-line status report.
//! Throws if hypha metadata is not initialized (call hypha_init() first).
std::string RunBaseSnapshotPlan(Connection &con, bool is_base_snapshot = false);

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
//! Returns one SyncTableResult per changed table (or one summary row if nothing changed).
//! Called internally by RegisterSyncTableFunction.

//! Register the hypha_sync() TableFunction with the extension loader.
//! Replaces the old scalar-function registration.
void RegisterSyncTableFunction(ExtensionLoader &loader);

//! Reads the stored default Postgres target, connects, lists all non-system schemas,
//! and drops them with DROP SCHEMA IF EXISTS ... CASCADE. When drop_meta=true also
//! drops the hypha bookkeeping schema. Returns a summary string. Throws if no target
//! has been stored (call hypha_init() first).
std::string RunHyphaDrop(Connection &con, bool drop_meta = false);

//! Returns a one-line summary of the last sync state by reading local DuckDB metadata
//! (hypha.commit, hypha.object_snapshot). No Postgres connection required.
//! Returns a help message when no sync history exists.
std::string RunHyphaStatus(Connection &con);

} // namespace duckdb
