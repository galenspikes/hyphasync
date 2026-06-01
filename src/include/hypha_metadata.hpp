#pragma once

#include "duckdb/main/connection.hpp"
#include <string>

namespace duckdb {

//! hyphasync release version (Phase 0 scaffold).
constexpr const char *HYPHASYNC_VERSION = "0.2.0";

//! Version of the local hypha.* metadata layout. Bump when the schema changes (drives migrations).
//! v1 → v2: added indexes and fast_mode key.
constexpr const char *HYPHA_METADATA_SCHEMA_VERSION = "3";
constexpr int HYPHA_SCHEMA_VERSION = 3;

//! Frozen fingerprint algorithm version (see docs/fingerprinting.md).
//! v2 adds support for nested types (LIST/STRUCT/MAP) using DuckDB JSON serialization as payload.
//! v3 replaces the O(n) full-row sha256 scan with a fast rowid-statistics fingerprint (O(1) via zone maps).
constexpr const char *HYPHA_FINGERPRINT_ALGO = "v3";

//! Returns true when the hypha.target metadata table exists.
bool IsHyphaMetadataInitialized(Connection &con);

//! Creates hypha schema/tables and upserts the default Postgres target (idempotent).
void EnsureHyphaMetadata(Connection &con, const std::string &conn_string);

//! Human-readable diagnostic report for hypha_doctor().
std::string BuildDoctorReport(Connection &con);

//! Returns conn_string for target_name='default', or empty when unavailable.
std::string GetDefaultTargetConnString(Connection &con);

//! Returns current_database() name from DuckDB (e.g. "ferc60-xbrl" for ferc60-xbrl.duckdb).
std::string GetCurrentDatabase(Connection &con);

//! Best-effort append to hypha.event_log. No-op (never throws) when metadata is not initialized,
//! so observability never breaks the caller's operation.
void LogEvent(Connection &con, const std::string &level, const std::string &operation, const std::string &code,
              const std::string &message, const std::string &details_json = "");

//! Read max_rows_per_table from hypha.meta (0 = unlimited / no guard).
int64_t GetMaxRowsPerTable(Connection &con);

//! Persist max_rows_per_table into hypha.meta (0 = unlimited / no guard).
void SetMaxRowsPerTable(Connection &con, int64_t limit);

//! Read fast_mode from hypha.meta (false when not set or not initialized).
bool GetFastMode(Connection &con);

//! Persist fast_mode into hypha.meta.
void SetFastMode(Connection &con, bool fast_mode);

} // namespace duckdb
