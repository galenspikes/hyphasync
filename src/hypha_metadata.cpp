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
    status VARCHAR NOT NULL,
    fingerprint_algo VARCHAR
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
    pk_columns VARCHAR,
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

const char *CREATE_META_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.meta (
    key VARCHAR PRIMARY KEY,
    value VARCHAR,
    updated_at TIMESTAMP DEFAULT current_timestamp
);
)";

std::string SqlQuote(const std::string &value) {
	return StringUtil::Replace(value, "'", "''");
}

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
	Exec(con, CREATE_META_SQL, "CREATE TABLE hypha.meta");

	// Migrations: safe to re-run; ADD COLUMN IF NOT EXISTS is idempotent.
	con.Query("ALTER TABLE hypha.commit ADD COLUMN IF NOT EXISTS fingerprint_algo VARCHAR");
	con.Query("ALTER TABLE hypha.object_snapshot ADD COLUMN IF NOT EXISTS pk_columns VARCHAR");

	const auto seed_meta_sql = StringUtil::Format(R"(
INSERT INTO hypha.meta (key, value) VALUES
    ('metadata_schema_version', '%s'),
    ('fingerprint_algo', '%s'),
    ('hyphasync_version', '%s')
ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = now()
)",
	                                              SqlQuote(HYPHA_METADATA_SCHEMA_VERSION),
	                                              SqlQuote(HYPHA_FINGERPRINT_ALGO), SqlQuote(HYPHASYNC_VERSION));
	ThrowOnError(con.Query(seed_meta_sql), "SEED hypha.meta");

	const auto upsert_sql = StringUtil::Format(R"(
INSERT INTO hypha.target AS t (target_name, conn_string, last_commit_id)
VALUES ('default', '%s', NULL)
ON CONFLICT (target_name) DO UPDATE SET
    conn_string = excluded.conn_string
)",
	                                           SqlQuote(conn_string));
	ThrowOnError(con.Query(upsert_sql), "UPSERT hypha.target default row");
}

void LogEvent(Connection &con, const std::string &level, const std::string &operation, const std::string &code,
              const std::string &message, const std::string &details_json) {
	// Best-effort observability: the event_log table only exists once metadata is
	// initialized, and a logging failure must never abort the caller's operation.
	if (!IsHyphaMetadataInitialized(con)) {
		return;
	}
	const std::string details = details_json.empty() ? std::string("NULL") : ("'" + SqlQuote(details_json) + "'");
	const auto sql = StringUtil::Format(
	    R"(INSERT INTO hypha.event_log (event_id, level, operation, code, message, details_json)
VALUES (uuid()::VARCHAR, '%s', '%s', '%s', '%s', %s))",
	    SqlQuote(level), SqlQuote(operation), SqlQuote(code), SqlQuote(message), details);
	con.Query(sql);
}

namespace {

//! Returns the first column of the first row as text, or empty string on error/NULL.
std::string QueryScalarText(Connection &con, const char *sql) {
	auto result = con.Query(sql);
	if (result->HasError() || result->RowCount() == 0) {
		return "";
	}
	const auto value = result->GetValue(0, 0);
	if (value.IsNull()) {
		return "";
	}
	return value.ToString();
}

} // namespace

std::string BuildDoctorReport(Connection &con) {
	const bool initialized = IsHyphaMetadataInitialized(con);
	std::string version = HYPHASYNC_VERSION;
#ifdef EXT_VERSION_HYPHASYNC
	if (version.empty()) {
		version = EXT_VERSION_HYPHASYNC;
	}
#endif

	// Local DuckDB identity, so the user can sanity-check what they are attached to.
	const auto local_database = QueryScalarText(con, "SELECT current_database()");
	auto local_database_path =
	    QueryScalarText(con, "SELECT path FROM duckdb_databases() WHERE database_name = current_database()");
	if (local_database_path.empty()) {
		local_database_path = ":memory:";
	}

	std::string report;
	report += "hyphasync_version=" + version + "\n";
	report += "duckdb_version=" + std::string(DuckDB::LibraryVersion()) + "\n";
	report += "local_database=" + local_database + "\n";
	report += "local_database_path=" + local_database_path + "\n";
	report += std::string("metadata_initialized=") + (initialized ? "true" : "false") + "\n";
	report += "metadata_schema_version=" + std::string(HYPHA_METADATA_SCHEMA_VERSION) + "\n";
	report += "fingerprint_algo=" + std::string(HYPHA_FINGERPRINT_ALGO) + "\n";
	// Capability status: anything that is a scaffolded placeholder says so explicitly.
	report += "capability_init=available\n";
	report += "capability_target_status=available\n";
	report += "capability_base_snapshot_plan=available\n";
	report += "capability_base_snapshot=available\n";
	report += "capability_sync_plan=available (SHA-256 fingerprint diff; detects all changes)\n";
	report += "capability_sync=available (SHA-256 fingerprint diff; row-level diff planned for v2)\n";
	report += "note=use hypha_target_status() to probe the Postgres target; hypha_base_snapshot() to push; "
	          "hypha_sync() to sync";
	return report;
}

std::string GetCurrentDatabase(Connection &con) {
	return QueryScalarText(con, "SELECT current_database()");
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
