#include "hypha_metadata.hpp"

#include "hypha_postgres.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <mutex>

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
    hypha_object_id VARCHAR,
    pg_table_name VARCHAR,
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
    fingerprint_strategy VARCHAR,
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

// hypha_verify() tripwire baseline: the last exact (full per-row) table_hash seen by a
// hypha_verify() run, per table. Lets a subsequent verify detect in-place changes that the
// fast O(1) fingerprint strategies (MUTABLE_ENTITY/APPEND_ONLY) can miss.
const char *CREATE_VERIFY_STATE_SQL = R"(
CREATE TABLE IF NOT EXISTS hypha.verify_state (
    schema_name VARCHAR,
    object_name VARCHAR,
    verify_hash VARCHAR,
    verified_at TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (schema_name, object_name)
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
	Exec(con, CREATE_META_SQL, "CREATE TABLE hypha.meta");
	Exec(con, CREATE_VERIFY_STATE_SQL, "CREATE TABLE hypha.verify_state");

	// Read stored schema version for migration decisions.
	int stored_version = 0;
	{
		auto ver_res = con.Query("SELECT value FROM hypha.meta WHERE key = 'schema_version'");
		if (!ver_res->HasError() && ver_res->RowCount() > 0 && !ver_res->GetValue(0, 0).IsNull()) {
			try {
				stored_version = std::stoi(ver_res->GetValue(0, 0).ToString());
			} catch (...) {
				stored_version = 0;
			}
		}
	}

	// Guard: refuse to open a database written by a newer extension version.
	if (stored_version > HYPHA_SCHEMA_VERSION) {
		throw InvalidInputException(
		    "hyphasync: database schema version %d is newer than extension version %d; upgrade the extension.",
		    stored_version, HYPHA_SCHEMA_VERSION);
	}

	// Migration v1 → v2: add indexes and missing columns (idempotent, IF NOT EXISTS guards).
	if (stored_version < 2) {
		con.Query("CREATE INDEX IF NOT EXISTS idx_row_hash_table_commit "
		          "ON hypha.row_hash(table_name, commit_id)");
		con.Query("CREATE INDEX IF NOT EXISTS idx_object_snapshot_table_commit "
		          "ON hypha.object_snapshot(table_name, commit_id)");
		con.Query("ALTER TABLE hypha.commit ADD COLUMN IF NOT EXISTS fingerprint_algo VARCHAR");
		con.Query("ALTER TABLE hypha.object_snapshot ADD COLUMN IF NOT EXISTS pk_columns VARCHAR");
		con.Query("ALTER TABLE hypha.object_snapshot ADD COLUMN IF NOT EXISTS hypha_object_id VARCHAR");
		con.Query("ALTER TABLE hypha.table_snapshot ADD COLUMN IF NOT EXISTS fingerprint_strategy VARCHAR");
	}

	// Migration v2 → v3: add pg_table_name for collision-safe identifier truncation.
	if (stored_version < 3) {
		con.Query("ALTER TABLE hypha.object_snapshot ADD COLUMN IF NOT EXISTS pg_table_name VARCHAR");
	}

	// Write the current schema version (covers fresh installs and post-migration updates).
	const auto set_ver_sql =
	    StringUtil::Format("INSERT INTO hypha.meta (key, value) VALUES ('schema_version', %s) "
	                       "ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = now()",
	                       QuoteLiteral(std::to_string(HYPHA_SCHEMA_VERSION)));
	con.Query(set_ver_sql);

	const auto seed_meta_sql =
	    StringUtil::Format(R"(
INSERT INTO hypha.meta (key, value) VALUES
    ('metadata_schema_version', %s),
    ('fingerprint_algo', %s),
    ('hyphasync_version', %s)
ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = now()
)",
	                       QuoteLiteral(HYPHA_METADATA_SCHEMA_VERSION), QuoteLiteral(HYPHA_FINGERPRINT_ALGO),
	                       QuoteLiteral(HYPHASYNC_VERSION));
	ThrowOnError(con.Query(seed_meta_sql), "SEED hypha.meta");

	const auto upsert_sql = StringUtil::Format(R"(
INSERT INTO hypha.target AS t (target_name, conn_string, last_commit_id)
VALUES ('default', %s, NULL)
ON CONFLICT (target_name) DO UPDATE SET
    conn_string = excluded.conn_string
)",
	                                           QuoteLiteral(conn_string));
	ThrowOnError(con.Query(upsert_sql), "UPSERT hypha.target default row");
}

void LogEvent(Connection &con, const std::string &level, const std::string &operation, const std::string &code,
              const std::string &message, const std::string &details_json) {
	// Best-effort observability: the event_log table only exists once metadata is
	// initialized, and a logging failure must never abort the caller's operation.
	// Use once_flag to guard the initialization check against concurrent calls from
	// multiple threads (e.g., parallel table processing in HyphaSnapshotFunction).
	static std::once_flag s_event_log_once;
	static bool s_event_log_ok = false;
	std::call_once(s_event_log_once, [&] { s_event_log_ok = IsHyphaMetadataInitialized(con); });
	if (!s_event_log_ok) {
		return;
	}
	const std::string details = details_json.empty() ? std::string("NULL") : QuoteLiteral(details_json);
	const auto sql = StringUtil::Format(
	    R"(INSERT INTO hypha.event_log (event_id, level, operation, code, message, details_json)
VALUES (uuid()::VARCHAR, %s, %s, %s, %s, %s))",
	    QuoteLiteral(level), QuoteLiteral(operation), QuoteLiteral(code), QuoteLiteral(message), details);
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
	report += "capability_init=available\n";
	report += "capability_target_status=available\n";
	report += "capability_base_snapshot_plan=available\n";
	report += "capability_base_snapshot=available\n";
	report += "capability_sync_plan=available\n";
	report += "capability_sync=available\n";
	report += "nested_types=LIST/STRUCT/MAP/JSON → text holding canonical JSON (lossless; requires json extension)\n";
	report += "schema_evolution=ADD/DROP COLUMN without DROP+CREATE when possible\n";
	report += "remote_metadata=hypha.sync_log and hypha.object_state written after each push\n";
	report += "row_level_diff=targeted DELETE+INSERT for single and composite PKs\n";
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

int64_t GetMaxRowsPerTable(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		return 0;
	}
	auto result = con.Query("SELECT value FROM hypha.meta WHERE key = 'max_rows_per_table'");
	if (!result || result->HasError() || result->RowCount() == 0) {
		return 0;
	}
	const auto val = result->GetValue(0, 0);
	if (val.IsNull()) {
		return 0;
	}
	try {
		return std::stoll(val.ToString());
	} catch (...) {
		return 0;
	}
}

void SetMaxRowsPerTable(Connection &con, int64_t limit) {
	if (!IsHyphaMetadataInitialized(con)) {
		return;
	}
	const auto sql = StringUtil::Format("INSERT INTO hypha.meta (key, value) VALUES ('max_rows_per_table', %s) "
	                                    "ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = now()",
	                                    QuoteLiteral(std::to_string(limit)));
	con.Query(sql);
}

bool GetFastMode(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		return false;
	}
	auto result = con.Query("SELECT value FROM hypha.meta WHERE key = 'fast_mode'");
	if (!result || result->HasError() || result->RowCount() == 0) {
		return false;
	}
	const auto val = result->GetValue(0, 0);
	if (val.IsNull()) {
		return false;
	}
	return val.ToString() == "true";
}

void SetFastMode(Connection &con, bool fast_mode) {
	if (!IsHyphaMetadataInitialized(con)) {
		return;
	}
	const auto sql = StringUtil::Format("INSERT INTO hypha.meta (key, value) VALUES ('fast_mode', %s) "
	                                    "ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = now()",
	                                    QuoteLiteral(fast_mode ? std::string("true") : std::string("false")));
	con.Query(sql);
}

bool GetExactVerify(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		return false;
	}
	auto result = con.Query("SELECT value FROM hypha.meta WHERE key = 'exact_verify'");
	if (!result || result->HasError() || result->RowCount() == 0) {
		return false;
	}
	const auto val = result->GetValue(0, 0);
	if (val.IsNull()) {
		return false;
	}
	return val.ToString() == "true";
}

void SetExactVerify(Connection &con, bool exact_verify) {
	if (!IsHyphaMetadataInitialized(con)) {
		return;
	}
	const auto sql = StringUtil::Format("INSERT INTO hypha.meta (key, value) VALUES ('exact_verify', %s) "
	                                    "ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = now()",
	                                    QuoteLiteral(exact_verify ? std::string("true") : std::string("false")));
	con.Query(sql);
}

} // namespace duckdb
