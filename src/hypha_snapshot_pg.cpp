#include "hypha_snapshot.hpp"
#include "hypha_snapshot_internal.hpp"

#include "hypha_fingerprint.hpp"
#include "hypha_metadata.hpp"
#include "hypha_postgres.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libpq-fe.h>
#include <mutex>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

//! Sanitize an arbitrary string to a safe Postgres identifier component.
//! Replaces anything that isn't [a-z0-9_] with '_' and lowercases.
std::string SanitizeIdent(const std::string &name) {
	std::string out;
	out.reserve(name.size());
	for (const char c : name) {
		if (isalnum(static_cast<unsigned char>(c)) || c == '_') {
			out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
		} else {
			out += '_';
		}
	}
	return out;
}

//! Build the Postgres schema name for a given DuckDB (database, schema) pair.
//! e.g. db="ferc60-xbrl", schema="main" -> "ferc60_xbrl_main"
std::string MakePostgresSchemaName(const std::string &db_name, const std::string &duckdb_schema) {
	return SanitizeIdent(db_name) + "_" + SanitizeIdent(duckdb_schema);
}

//! Trim trailing whitespace/newlines from a libpq error message.
std::string TrimPQ(const char *value) {
	if (!value) {
		return "";
	}
	std::string out = value;
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
		out.pop_back();
	}
	return out;
}

// Postgres max identifier length (NAMEDATALEN - 1) — defined in the first anonymous namespace above.

//! Deduplicate and truncate column names to fit Postgres's 63-char NAMEDATALEN limit.
//! Collisions (two names that truncate to the same prefix) get a numeric suffix.
std::vector<std::string> ResolveColumnNames(const std::vector<ColumnDef> &cols) {
	std::unordered_set<std::string> used;
	std::vector<std::string> pg_names;
	pg_names.reserve(cols.size());
	for (const auto &col : cols) {
		std::string candidate = TruncPGIdent(col.name);
		if (used.count(candidate)) {
			const std::string base = col.name.substr(0, PG_MAX_IDENT - 3);
			int sfx = 1;
			while (true) {
				const auto sfx_str = "_" + std::to_string(sfx);
				candidate = base.substr(0, PG_MAX_IDENT - sfx_str.size()) + sfx_str;
				if (!used.count(candidate)) {
					break;
				}
				sfx++;
			}
		}
		used.insert(candidate);
		pg_names.push_back(candidate);
	}
	return pg_names;
}

//! Returns true for varlena postgres_types that benefit from SET STORAGE EXTERNAL.
//! EXTERNAL forces all values out-of-line regardless of size, which sidesteps
//! Postgres's 8 KB per-row limit on tables with many text columns.
bool NeedsExternalStorage(const std::string &pg_type) {
	const auto t = StringUtil::Lower(pg_type);
	if (t == "text" || t == "bytea" || t == "jsonb" || t == "bit varying") {
		return true;
	}
	return StringUtil::StartsWith(t, "varchar") || StringUtil::StartsWith(t, "character varying") ||
	       StringUtil::StartsWith(t, "char(") || StringUtil::StartsWith(t, "character(");
}

//! Estimates the on-heap byte width of a single row after varlena columns have been
//! TOASTed out-of-line (i.e. counts only non-varlena columns).  Used to detect tables
//! that would exceed Postgres's ~8 KB heap row limit even after SET STORAGE EXTERNAL.
int64_t EstimateFixedWidthRowBytes(const std::vector<ColumnDef> &cols) {
	int64_t sum = 0;
	for (const auto &cd : cols) {
		if (NeedsExternalStorage(cd.postgres_type)) {
			continue; // varlena — will be TOASTed out-of-line
		}
		const auto t = StringUtil::Lower(cd.postgres_type);
		if (t == "integer" || t == "int4" || t == "int" || t == "real" || t == "float4" || t == "date") {
			sum += 4;
		} else if (t == "boolean" || t == "bool") {
			sum += 1;
		} else if (t == "smallint" || t == "int2") {
			sum += 2;
		} else if (t == "uuid") {
			sum += 16;
		} else if (t == "bigint" || t == "int8" || t == "double precision" || t == "float8" ||
		           StringUtil::StartsWith(t, "numeric") || StringUtil::StartsWith(t, "decimal") ||
		           StringUtil::StartsWith(t, "time") || StringUtil::StartsWith(t, "timestamp") || t == "interval") {
			sum += 8;
		} else {
			sum += 8; // conservative default for unknown fixed-width types
		}
	}
	return sum;
}

//! Returns true for DuckDB source types that require a ::JSON cast before COPY.
//! - STRUCT/ROW/MAP: DuckDB CSV export uses single-quote notation, not valid JSON
//! - T[] / LIST(T): DuckDB CSV export uses "[a, b]" brackets; Postgres expects "{a,b}"
//! Casting to JSON produces standard double-quoted JSON accepted by Postgres JSONB.
bool NeedsJsonCastForCopy(const std::string &duckdb_type) {
	const auto t = StringUtil::Upper(duckdb_type);
	return StringUtil::EndsWith(t, "[]") || StringUtil::StartsWith(t, "LIST(") ||
	       StringUtil::StartsWith(t, "STRUCT(") || StringUtil::StartsWith(t, "ROW(") ||
	       StringUtil::StartsWith(t, "MAP(");
}

//! Builds the SELECT expression for a COPY (SELECT ...) TO ... statement.
//! Always uses an explicit column list (rather than SELECT *) so that:
//!   - Unsupported columns excluded from `cols` are not included in the CSV.
//!   - STRUCT/MAP columns (needs_json_cast=true) are cast to JSON for Postgres JSONB import.
//! Returns a full "SELECT ... FROM schema.table [WHERE filter]" string.
std::string BuildCopySelectList(const std::string &duckdb_schema, const std::string &table_name,
                                const std::vector<ColumnDef> &cols, const std::string &where_clause) {
	std::ostringstream ss;
	ss << "SELECT ";
	for (size_t i = 0; i < cols.size(); i++) {
		if (i > 0) {
			ss << ", ";
		}
		const auto &cd = cols[i];
		if (cd.needs_json_cast) {
			ss << QuoteIdent(cd.name) << "::JSON AS " << QuoteIdent(cd.name);
		} else {
			ss << QuoteIdent(cd.name);
		}
	}
	ss << " FROM " << QuoteIdent(duckdb_schema) << "." << QuoteIdent(table_name);
	if (!where_clause.empty()) {
		ss << " WHERE " << where_clause;
	}
	return ss.str();
}

std::string BuildCreateTableDDL(const std::string &pg_schema, const std::string &pg_table_name,
                                const std::vector<ColumnDef> &cols, const std::vector<std::string> &pg_names,
                                bool suppress_not_null) {
	std::ostringstream ddl;
	ddl << "CREATE TABLE " << QuoteIdent(pg_schema) << "." << QuoteIdent(pg_table_name) << " (\n";
	for (size_t i = 0; i < cols.size(); i++) {
		ddl << "    " << QuoteIdent(pg_names[i]) << " " << cols[i].postgres_type;
		if (!cols[i].is_nullable && !suppress_not_null) {
			ddl << " NOT NULL";
		}
		if (i + 1 < cols.size()) {
			ddl << ",";
		}
		ddl << "\n";
	}
	ddl << ")";
	return ddl.str();
}

//! Execute a single statement on Postgres; throw IOException on failure.
void PGExec(PGconn *pg, const std::string &sql, const char *context) {
	PGresult *res = PQexec(pg, sql.c_str());
	if (!res) {
		throw IOException(StringUtil::Format("%s: PQexec returned null (out of memory): %.120s", context, sql.c_str()));
	}
	const auto status = PQresultStatus(res);
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		const auto err = TrimPQ(PQresultErrorMessage(res));
		PQclear(res);
		throw IOException(StringUtil::Format("%s: %s", context, err));
	}
	PQclear(res);
}

// ---------------------------------------------------------------------------
// Remote hypha metadata — bookkeeping schema on the Postgres target
// ---------------------------------------------------------------------------

//! Idempotently create the hypha schema and bookkeeping tables on the remote Postgres target.
//! Called as best-effort after a successful push or sync.
void EnsureRemoteHyphaMetadata(PGconn *pg) {
	PGExec(pg, "CREATE SCHEMA IF NOT EXISTS hypha", "CREATE SCHEMA hypha");
	PGExec(pg, R"(
CREATE TABLE IF NOT EXISTS hypha.sync_log (
    sync_id          VARCHAR PRIMARY KEY,
    source_database  VARCHAR NOT NULL,
    operation_kind   VARCHAR NOT NULL,
    commit_id        VARCHAR,
    parent_commit_id VARCHAR,
    fingerprint_algo VARCHAR,
    tables_synced    INTEGER,
    rows_synced      BIGINT,
    tables_failed    INTEGER,
    rows_failed      BIGINT,
    pushed_at        TIMESTAMPTZ DEFAULT now()
))",
	       "CREATE TABLE hypha.sync_log");
	// Idempotent migration: add failure counters to existing installs.
	PGExec(pg, "ALTER TABLE hypha.sync_log ADD COLUMN IF NOT EXISTS tables_failed INTEGER",
	       "ALTER TABLE hypha.sync_log ADD tables_failed");
	PGExec(pg, "ALTER TABLE hypha.sync_log ADD COLUMN IF NOT EXISTS rows_failed BIGINT",
	       "ALTER TABLE hypha.sync_log ADD rows_failed");
	PGExec(pg, R"(
CREATE TABLE IF NOT EXISTS hypha.object_state (
    pg_schema          VARCHAR NOT NULL,
    pg_table           VARCHAR NOT NULL,
    source_database    VARCHAR,
    source_schema      VARCHAR,
    source_table       VARCHAR,
    hypha_object_id    VARCHAR,
    definition_hash    VARCHAR,
    object_fingerprint VARCHAR,
    last_sync_id       VARCHAR,
    last_synced_at     TIMESTAMPTZ,
    PRIMARY KEY (pg_schema, pg_table)
))",
	       "CREATE TABLE hypha.object_state");
	PGExec(pg, R"(
CREATE TABLE IF NOT EXISTS hypha.event_log (
    event_id   BIGSERIAL PRIMARY KEY,
    db_name    VARCHAR,
    level      VARCHAR,
    operation  VARCHAR,
    code       VARCHAR,
    message    TEXT,
    created_at TIMESTAMPTZ DEFAULT now()
))",
	       "CREATE TABLE hypha.event_log");
	// Idempotent migration: add hypha_object_id if not yet present (suppresses NOTICE).
	PQclear(PQexec(pg, R"(
DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema='hypha' AND table_name='object_state' AND column_name='hypha_object_id'
  ) THEN
    ALTER TABLE hypha.object_state ADD COLUMN hypha_object_id VARCHAR;
  END IF;
END $$)"));
}

//! Idempotently create hypha.event_log on the Postgres target.
//! Called on the dedicated log connection right after it is opened so that
//! LogEventRemote calls succeed even on the very first sync.
void EnsureRemoteEventLog(PGconn *pg) {
	if (!pg || PQstatus(pg) != CONNECTION_OK) {
		return;
	}
	PQclear(PQexec(pg, "CREATE SCHEMA IF NOT EXISTS hypha"));
	PQclear(PQexec(pg, R"(
CREATE TABLE IF NOT EXISTS hypha.event_log (
    event_id   BIGSERIAL PRIMARY KEY,
    db_name    VARCHAR,
    level      VARCHAR,
    operation  VARCHAR,
    code       VARCHAR,
    message    TEXT,
    created_at TIMESTAMPTZ DEFAULT now()
))"));
}

//! Log to both the local DuckDB event_log and (when pg_log is non-null) the remote
//! Postgres event_log.  pg_log must be a *dedicated* auto-commit connection — never
//! the same PGconn that is currently inside a BEGIN block — so each insert is
//! committed immediately and visible to observers even if the main transaction rolls back.
void LogAll(Connection &con, PGconn *pg_log, const std::string &db_name, const std::string &level,
            const std::string &operation, const std::string &code, const std::string &message,
            const std::string &details_json) {
	LogEvent(con, level, operation, code, message, details_json);
	if (pg_log) {
		LogEventRemote(pg_log, db_name, level, operation, code, message);
	}
}

//! Write remote hypha metadata after a successful sync or base snapshot.
//! Idempotently creates the bookkeeping schema/tables, records the operation in
//! hypha.sync_log, and upserts per-table state into hypha.object_state.
//! The caller must wrap this in try/catch and log any failure — it must never abort
//! an already-committed sync.
void WriteRemoteHyphaMetadata(Connection &con, PGconn *pg, const std::string &sync_id,
                              const std::string &source_database, const std::string &operation_kind,
                              const std::string &snapshot_commit_id, const std::string &parent_commit_id,
                              const std::string &fingerprint_algo, int64_t tables_synced, int64_t rows_synced,
                              int64_t tables_failed, int64_t rows_failed) {
	EnsureRemoteHyphaMetadata(pg);

	const auto parent_val = parent_commit_id.empty() ? std::string("NULL") : QuoteLiteral(parent_commit_id);
	PGExec(pg,
	       StringUtil::Format("INSERT INTO hypha.sync_log "
	                          "(sync_id, source_database, operation_kind, commit_id, parent_commit_id, "
	                          " fingerprint_algo, tables_synced, rows_synced, tables_failed, rows_failed) "
	                          "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s) "
	                          "ON CONFLICT (sync_id) DO NOTHING",
	                          QuoteLiteral(sync_id), QuoteLiteral(source_database), QuoteLiteral(operation_kind),
	                          QuoteLiteral(snapshot_commit_id), parent_val, QuoteLiteral(fingerprint_algo),
	                          std::to_string(tables_synced), std::to_string(rows_synced), std::to_string(tables_failed),
	                          std::to_string(rows_failed)),
	       "INSERT hypha.sync_log");

	// Upsert per-table state from hypha.object_snapshot for the given snapshot commit.
	// Include hypha_object_id so the remote ledger carries the stable object identity.
	const auto snap_result = Exec(con,
	                              StringUtil::Format("SELECT schema_name, object_name, "
	                                                 "COALESCE(definition_hash, ''), COALESCE(object_fingerprint, ''), "
	                                                 "COALESCE(hypha_object_id, ''), "
	                                                 "COALESCE(pg_table_name, substr(object_name, 1, 63)) "
	                                                 "FROM hypha.object_snapshot "
	                                                 "WHERE commit_id = %s AND object_type = 'table' "
	                                                 "ORDER BY schema_name, object_name",
	                                                 QuoteLiteral(snapshot_commit_id)),
	                              "get object snapshots for remote metadata");

	for (idx_t i = 0; i < snap_result->RowCount(); i++) {
		const auto duckdb_schema = snap_result->GetValue(0, i).ToString();
		const auto table_name = snap_result->GetValue(1, i).ToString();
		const auto def_hash = snap_result->GetValue(2, i).ToString();
		const auto obj_fp = snap_result->GetValue(3, i).ToString();
		const auto hypha_oid = snap_result->GetValue(4, i).ToString();
		const auto pg_schema = MakePostgresSchemaName(source_database, duckdb_schema);
		const auto pg_table = snap_result->GetValue(5, i).ToString();
		const auto def_val = def_hash.empty() ? std::string("NULL") : QuoteLiteral(def_hash);
		const auto fp_val = obj_fp.empty() ? std::string("NULL") : QuoteLiteral(obj_fp);
		const auto oid_val = hypha_oid.empty() ? std::string("NULL") : QuoteLiteral(hypha_oid);
		PGExec(pg,
		       StringUtil::Format("INSERT INTO hypha.object_state "
		                          "(pg_schema, pg_table, source_database, source_schema, source_table, "
		                          " hypha_object_id, definition_hash, object_fingerprint, "
		                          " last_sync_id, last_synced_at) "
		                          "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, now()) "
		                          "ON CONFLICT (pg_schema, pg_table) DO UPDATE SET "
		                          "  source_database    = EXCLUDED.source_database, "
		                          "  source_schema      = EXCLUDED.source_schema, "
		                          "  source_table       = EXCLUDED.source_table, "
		                          "  hypha_object_id    = EXCLUDED.hypha_object_id, "
		                          "  definition_hash    = EXCLUDED.definition_hash, "
		                          "  object_fingerprint = EXCLUDED.object_fingerprint, "
		                          "  last_sync_id       = EXCLUDED.last_sync_id, "
		                          "  last_synced_at     = EXCLUDED.last_synced_at",
		                          QuoteLiteral(pg_schema), QuoteLiteral(pg_table), QuoteLiteral(source_database),
		                          QuoteLiteral(duckdb_schema), QuoteLiteral(table_name), oid_val, def_val, fp_val,
		                          QuoteLiteral(sync_id)),
		       "UPSERT hypha.object_state");
	}
}

// ---------------------------------------------------------------------------
// Object stamping — COMMENT ON TABLE/SCHEMA stamps every hyphasync-owned
// Postgres object with a JSON identity block that is readable from Postgres
// without any hyphasync tooling.
//
// Comment format (table):
//   {"managed_by":"hyphasync","hypha_object_id":"<uuid>",
//    "source":"<db>.<schema>.<table>","commit_id":"<short>","pushed_at":"<iso>"}
//
// Comment format (schema):
//   {"managed_by":"hyphasync","source":"<db>.<schema>",
//    "commit_id":"<short>","pushed_at":"<iso>"}
//
// Querying from Postgres (no hyphasync required):
//   SELECT tablename,
//          obj_description(c.oid,'pg_class')::jsonb->>'hypha_object_id' AS id,
//          obj_description(c.oid,'pg_class')::jsonb->>'source'          AS source
//   FROM pg_tables t JOIN pg_class c ON c.relname=t.tablename
//        JOIN pg_namespace n ON n.oid=c.relnamespace AND n.nspname=t.schemaname
//   WHERE t.schemaname='mydb_main'
//     AND (obj_description(c.oid,'pg_class')::jsonb)->>'managed_by'='hyphasync';
// ---------------------------------------------------------------------------

//! Escape a string for embedding inside a JSON literal (minimal: \\ and \").
std::string JsonEscapePg(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	for (const char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	return out;
}

//! Stamp a Postgres table with a JSON COMMENT identifying it as hyphasync-managed.
//! Best-effort: a failure here must never abort a sync that has already committed data.
void StampHyphaTableComment(PGconn *pg, const std::string &pg_schema, const std::string &pg_table,
                            const std::string &hypha_object_id, const std::string &source_database,
                            const std::string &source_schema, const std::string &source_table,
                            const std::string &commit_id) {
	const std::string source =
	    JsonEscapePg(source_database) + "." + JsonEscapePg(source_schema) + "." + JsonEscapePg(source_table);
	const std::string short_commit = commit_id.size() >= 8 ? commit_id.substr(0, 8) : commit_id;
	const std::string comment = "{\"managed_by\":\"hyphasync\","
	                            "\"hypha_object_id\":\"" +
	                            JsonEscapePg(hypha_object_id) +
	                            "\","
	                            "\"source\":\"" +
	                            source +
	                            "\","
	                            "\"commit_id\":\"" +
	                            JsonEscapePg(short_commit) + "\"}";
	const std::string sql = "COMMENT ON TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " IS '" +
	                        StringUtil::Replace(comment, "'", "''") + "'";
	PQclear(PQexec(pg, sql.c_str())); // best-effort: ignore result
}

//! Stamp a Postgres schema with a JSON COMMENT identifying it as hyphasync-managed.
void StampHyphaSchemaComment(PGconn *pg, const std::string &pg_schema, const std::string &source_database,
                             const std::string &source_schema, const std::string &commit_id) {
	const std::string source = JsonEscapePg(source_database) + "." + JsonEscapePg(source_schema);
	const std::string short_commit = commit_id.size() >= 8 ? commit_id.substr(0, 8) : commit_id;
	const std::string comment = "{\"managed_by\":\"hyphasync\","
	                            "\"source\":\"" +
	                            source +
	                            "\","
	                            "\"commit_id\":\"" +
	                            JsonEscapePg(short_commit) + "\"}";
	const std::string sql =
	    "COMMENT ON SCHEMA " + QuoteIdent(pg_schema) + " IS '" + StringUtil::Replace(comment, "'", "''") + "'";
	PQclear(PQexec(pg, sql.c_str())); // best-effort: ignore result
}

//! Look up the hypha_object_id for a table from hypha.object_snapshot (latest applied commit).
//! Returns empty string if not found.
std::string GetHyphaObjectId(Connection &con, const std::string &schema_name, const std::string &table_name,
                             const std::string &commit_id) {
	const auto res = con.Query(StringUtil::Format(
	    "SELECT hypha_object_id FROM hypha.object_snapshot "
	    "WHERE commit_id = %s AND schema_name = %s AND object_name = %s AND hypha_object_id IS NOT NULL LIMIT 1",
	    QuoteLiteral(commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)));
	if (!res || res->HasError() || res->RowCount() == 0 || res->GetValue(0, 0).IsNull()) {
		return "";
	}
	return res->GetValue(0, 0).ToString();
}

MemoryLimitGuard::MemoryLimitGuard(Connection &con, idx_t new_limit)
    : con_(con), original_limit_(BufferManager::GetBufferManager(*con.context).GetMaxMemory()) {
	BufferManager::GetBufferManager(*con.context).SetMemoryLimit(new_limit);
}

MemoryLimitGuard::~MemoryLimitGuard() {
	try {
		BufferManager::GetBufferManager(*con_.context).SetMemoryLimit(original_limit_);
	} catch (...) {
	}
}

//! Compute a safe DuckDB memory limit for a sync operation.
//! Budget = min(30% sys_ram, 25% db_size, user_limit/2, 16 GB), floored to 128 MB.
//! Never exceeds the user's explicit limit.
idx_t ComputeSyncMemoryLimit(Connection &con, int64_t db_size_bytes) {
	const optional_idx avail = FileSystem::GetAvailableMemory();
	const idx_t sys_ram = avail.IsValid() ? avail.GetIndex() : (4ULL * 1024 * 1024 * 1024);

	auto &bm = BufferManager::GetBufferManager(*con.context);
	const idx_t user_limit_raw = bm.GetMaxMemory();
	const bool unlimited = (user_limit_raw == (idx_t)-1) || (user_limit_raw == 0);
	const idx_t user_limit = unlimited ? sys_ram : user_limit_raw;

	const idx_t thirty_pct_ram = static_cast<idx_t>(static_cast<double>(sys_ram) * 0.30);
	const idx_t twenty_five_pct_db =
	    (db_size_bytes > 0) ? static_cast<idx_t>(static_cast<double>(db_size_bytes) * 0.25) : thirty_pct_ram;
	const idx_t half_user = user_limit / 2;
	const idx_t cap_16gb = 16ULL * 1024 * 1024 * 1024;

	idx_t budget = thirty_pct_ram;
	if (twenty_five_pct_db < budget) {
		budget = twenty_five_pct_db;
	}
	if (half_user < budget) {
		budget = half_user;
	}
	if (cap_16gb < budget) {
		budget = cap_16gb;
	}

	const idx_t floor_128mb = 128ULL * 1024 * 1024;
	if (budget < floor_128mb) {
		budget = floor_128mb;
	}

	if (!unlimited && budget > user_limit_raw) {
		budget = user_limit_raw;
	}

	return budget;
}

} // namespace duckdb
