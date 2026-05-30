#pragma once

#include "duckdb/main/connection.hpp"
#include <string>

namespace duckdb {

//! hyphasync release version (Phase 0 scaffold).
constexpr const char *HYPHASYNC_VERSION = "0.2.0";

//! Version of the local hypha.* metadata layout. Bump when the schema changes (drives migrations).
constexpr const char *HYPHA_METADATA_SCHEMA_VERSION = "1";

//! Frozen fingerprint algorithm version (see docs/fingerprinting.md).
constexpr const char *HYPHA_FINGERPRINT_ALGO = "v1";

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

} // namespace duckdb
