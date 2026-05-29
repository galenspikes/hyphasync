#pragma once

#include "duckdb/main/connection.hpp"
#include <string>

namespace duckdb {

//! hyphasync release version (Phase 0 scaffold).
constexpr const char *HYPHASYNC_VERSION = "0.1.0";

//! Returns true when the hypha.target metadata table exists.
bool IsHyphaMetadataInitialized(Connection &con);

//! Creates hypha schema/tables and upserts the default Postgres target (idempotent).
void EnsureHyphaMetadata(Connection &con, const std::string &conn_string);

//! Human-readable diagnostic report for hypha_doctor().
std::string BuildDoctorReport(Connection &con);

} // namespace duckdb
