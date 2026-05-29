#include "hypha_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

namespace {

void ThrowOnError(const unique_ptr<MaterializedQueryResult> &result, const char *context) {
	if (result->HasError()) {
		throw Exception(ExceptionType::CATALOG, StringUtil::Format("%s failed: %s", context, result->GetError()));
	}
}

void Exec(Connection &con, const char *sql, const char *context) {
	ThrowOnError(con.Query(sql), context);
}

const char *CREATE_SCHEMA_SQL = "CREATE SCHEMA IF NOT EXISTS hypha;";

const char *CREATE_TARGET_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.target (
    target_name VARCHAR PRIMARY KEY,
    conn_string VARCHAR NOT NULL,
    created_at TIMESTAMP DEFAULT current_timestamp,
    last_commit_id VARCHAR
);
)";

const char *CREATE_COMMIT_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.commit (
    commit_id VARCHAR PRIMARY KEY,
    parent_commit_id VARCHAR,
    target_name VARCHAR,
    kind VARCHAR,
    message VARCHAR,
    created_at TIMESTAMP DEFAULT current_timestamp,
    applied_at TIMESTAMP,
    status VARCHAR NOT NULL
);
)";

const char *CREATE_OBJECT_SNAPSHOT_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.object_snapshot (
    commit_id VARCHAR,
    target_name VARCHAR,
    schema_name VARCHAR,
    object_name VARCHAR,
    object_type VARCHAR,
    definition_hash VARCHAR,
    content_hash VARCHAR,
    object_fingerprint VARCHAR,
    captured_at TIMESTAMP DEFAULT current_timestamp
);
)";

const char *CREATE_COLUMN_SNAPSHOT_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.column_snapshot (
    commit_id VARCHAR,
    target_name VARCHAR,
    schema_name VARCHAR,
    table_name VARCHAR,
    column_name VARCHAR,
    ordinal_position INTEGER,
    duckdb_type VARCHAR,
    postgres_type VARCHAR,
    is_nullable BOOLEAN,
    default_expr VARCHAR,
    captured_at TIMESTAMP DEFAULT current_timestamp
);
)";

const char *CREATE_TABLE_SNAPSHOT_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.table_snapshot (
    commit_id VARCHAR,
    target_name VARCHAR,
    schema_name VARCHAR,
    table_name VARCHAR,
    row_count UBIGINT,
    table_hash VARCHAR,
    captured_at TIMESTAMP DEFAULT current_timestamp
);
)";

const char *CREATE_ROW_HASH_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.row_hash (
    commit_id VARCHAR,
    target_name VARCHAR,
    schema_name VARCHAR,
    table_name VARCHAR,
    pk_json VARCHAR,
    row_hash VARCHAR
);
)";

const char *CREATE_EVENT_LOG_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.event_log (
    event_id VARCHAR,
    event_time TIMESTAMP DEFAULT current_timestamp,
    level VARCHAR,
    operation VARCHAR,
    code VARCHAR,
    message VARCHAR,
    details_json VARCHAR
);
)";

} // namespace

bool IsHyphaMetadataInitialized(Connection &con) {
	auto result = con.Query(R"(
SELECT COUNT(*)::BIGINT AS table_count
FROM information_schema.tables
WHERE table_schema = 'hypha' AND table_name = 'target'
)");
	if (result->HasError()) {
		return false;
	}
	if (result->RowCount() == 0) {
		return false;
	}
	return result->GetValue(0, 0).GetValue<int64_t>() > 0;
}

void EnsureHyphaMetadata(Connection &con, const std::string &conn_string) {
	Exec(con, CREATE_SCHEMA_SQL, "CREATE SCHEMA hypha");
	Exec(con, CREATE_TARGET_SQL, "CREATE TABLE hypha.target");
	Exec(con, CREATE_COMMIT_SQL, "CREATE TABLE hypha.commit");
	Exec(con, CREATE_OBJECT_SNAPSHOT_SQL, "CREATE TABLE hypha.object_snapshot");
	Exec(con, CREATE_COLUMN_SNAPSHOT_SQL, "CREATE TABLE hypha.column_snapshot");
	Exec(con, CREATE_TABLE_SNAPSHOT_SQL, "CREATE TABLE hypha.table_snapshot");
	Exec(con, CREATE_ROW_HASH_SQL, "CREATE TABLE hypha.row_hash");
	Exec(con, CREATE_EVENT_LOG_SQL, "CREATE TABLE hypha.event_log");

	const auto escaped_conn = StringUtil::Replace(conn_string, "'", "''");
	const auto upsert_sql = StringUtil::Format(R"(
INSERT INTO hypha.target AS t (target_name, conn_string, last_commit_id)
VALUES ('default', '%s', NULL)
ON CONFLICT (target_name) DO UPDATE SET
    conn_string = excluded.conn_string
)",
	                                           escaped_conn);
	ThrowOnError(con.Query(upsert_sql), "UPSERT hypha.target default row");
}

std::string BuildDoctorReport(Connection &con) {
	const bool initialized = IsHyphaMetadataInitialized(con);
	std::string version = HYPHASYNC_VERSION;
#ifdef EXT_VERSION_HYPHASYNC
	if (version.empty()) {
		version = EXT_VERSION_HYPHASYNC;
	}
#endif
	std::string report;
	report += "hyphasync_version=" + version + "\n";
	report += "duckdb_version=" + std::string(DuckDB::LibraryVersion()) + "\n";
	report += std::string("metadata_initialized=") + (initialized ? "true" : "false") + "\n";
	report += "note=base snapshot and sync are not implemented yet; use hypha_attach_check() for Postgres probes";
	return report;
}

std::string GetDefaultTargetConnString(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		return "";
	}
	auto result = con.Query("SELECT conn_string FROM hypha.target WHERE target_name = 'default'");
	if (result->HasError() || result->RowCount() == 0) {
		return "";
	}
	return result->GetValue(0, 0).ToString();
}

} // namespace duckdb
