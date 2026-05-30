#include "hypha_snapshot.hpp"

#include "hypha_fingerprint.hpp"
#include "hypha_metadata.hpp"
#include "hypha_postgres.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <cstdio>
#include <cstring>
#include <libpq-fe.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace duckdb {

namespace {

//! Double-quotes an identifier and escapes any embedded double-quotes.
std::string QuoteIdent(const std::string &name) {
	std::string out = "\"";
	for (const char c : name) {
		if (c == '"') {
			out += '"';
		}
		out += c;
	}
	out += '"';
	return out;
}

//! Single-quote a SQL string literal value, escaping embedded single quotes.
std::string QuoteLiteral(const std::string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

//! Run a query and throw with context on error.
unique_ptr<MaterializedQueryResult> Exec(Connection &con, const std::string &sql, const char *context) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw Exception(ExceptionType::EXECUTOR, StringUtil::Format("%s failed: %s", context, result->GetError()));
	}
	return result;
}

//! Map a DuckDB data_type string to its closest Postgres equivalent.
//! Returns "(unsupported: <type>)" for types with no safe mapping.
std::string DuckTypeToPostgres(const std::string &raw) {
	const auto t = StringUtil::Upper(raw);

	// Exact matches for common scalars
	if (t == "INTEGER" || t == "INT4" || t == "INT" || t == "SIGNED") {
		return "integer";
	}
	if (t == "BIGINT" || t == "INT8" || t == "LONG") {
		return "bigint";
	}
	if (t == "SMALLINT" || t == "INT2" || t == "SHORT") {
		return "smallint";
	}
	if (t == "TINYINT" || t == "INT1") {
		return "smallint"; // PG has no TINYINT; SMALLINT (int2) is the smallest
	}
	if (t == "HUGEINT") {
		return "numeric(39,0)";
	}
	if (t == "UBIGINT") {
		return "numeric(20,0)";
	}
	if (t == "UINTEGER") {
		return "bigint";
	}
	if (t == "USMALLINT") {
		return "integer";
	}
	if (t == "UTINYINT") {
		return "smallint";
	}
	if (t == "FLOAT" || t == "FLOAT4" || t == "REAL") {
		return "real";
	}
	if (t == "DOUBLE" || t == "FLOAT8" || t == "DOUBLE PRECISION") {
		return "double precision";
	}
	if (t == "BOOLEAN" || t == "BOOL" || t == "LOGICAL") {
		return "boolean";
	}
	if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || t == "CHAR VARYING" || t == "CHARACTER VARYING") {
		return "text";
	}
	if (t == "DATE") {
		return "date";
	}
	if (t == "TIME" || t == "TIME WITHOUT TIME ZONE") {
		return "time without time zone";
	}
	if (t == "TIMETZ" || t == "TIME WITH TIME ZONE") {
		return "time with time zone";
	}
	if (t == "TIMESTAMP" || t == "DATETIME" || t == "TIMESTAMP WITHOUT TIME ZONE") {
		return "timestamp without time zone";
	}
	if (t == "TIMESTAMPTZ" || t == "TIMESTAMP WITH TIME ZONE") {
		return "timestamp with time zone";
	}
	if (t == "INTERVAL") {
		return "interval";
	}
	if (t == "BLOB" || t == "BYTEA" || t == "BINARY" || t == "VARBINARY") {
		return "bytea";
	}
	if (t == "UUID") {
		return "uuid";
	}
	if (t == "JSON") {
		return "jsonb";
	}
	if (t == "BIT" || t == "BITSTRING") {
		return "bit varying";
	}

	// Parameterized types: DECIMAL(p,s) / NUMERIC(p,s) / CHAR(n) / VARCHAR(n)
	if (StringUtil::StartsWith(t, "DECIMAL(") || StringUtil::StartsWith(t, "NUMERIC(")) {
		// Re-emit as lowercase numeric(p,s)
		const auto paren = raw.find('(');
		return "numeric" + raw.substr(paren);
	}
	if (StringUtil::StartsWith(t, "CHAR(") || StringUtil::StartsWith(t, "CHARACTER(")) {
		const auto paren = raw.find('(');
		return "char" + raw.substr(paren);
	}
	if (StringUtil::StartsWith(t, "VARCHAR(")) {
		const auto paren = raw.find('(');
		return "varchar" + raw.substr(paren);
	}
	// Plain DECIMAL / NUMERIC without precision
	if (t == "DECIMAL" || t == "NUMERIC") {
		return "numeric";
	}

	// List/array types: INTEGER[], VARCHAR[], etc.
	if (StringUtil::EndsWith(t, "[]")) {
		const auto elem = DuckTypeToPostgres(raw.substr(0, raw.size() - 2));
		if (StringUtil::StartsWith(elem, "(unsupported")) {
			return "(unsupported: " + raw + ")";
		}
		return elem + "[]";
	}

	return "(unsupported: " + raw + ")";
}

struct TableInfo {
	std::string schema_name;
	std::string table_name;
};

} // namespace

std::string RunBaseSnapshotPlan(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException(
		    "hypha metadata is not initialized — call hypha_init() with a Postgres connection string first.");
	}

	// 1. Generate a commit ID.
	const auto id_result = Exec(con, "SELECT uuid()::VARCHAR", "generate commit_id");
	const std::string commit_id = id_result->GetValue(0, 0).ToString();

	// 2. Determine target_name (default if one exists, else 'default' as a label).
	const auto target_result = con.Query("SELECT target_name FROM hypha.target WHERE target_name = 'default'");
	const std::string target_name =
	    (!target_result->HasError() && target_result->RowCount() > 0) ? "default" : "default";

	// 3. Insert commit record (status='running'; updated to 'completed' at the end).
	const std::string current_db =
	    Exec(con, "SELECT current_database()", "get current_database")->GetValue(0, 0).ToString();
	const std::string commit_msg = "base snapshot plan: local catalog of database '" + current_db + "' captured";
	Exec(con,
	     StringUtil::Format(R"(
INSERT INTO hypha.commit (commit_id, parent_commit_id, target_name, kind, message, status)
VALUES (%s, NULL, %s, 'base_snapshot_plan', %s, 'running')
)",
	                        QuoteLiteral(commit_id), QuoteLiteral(target_name), QuoteLiteral(commit_msg)),
	     "INSERT hypha.commit");

	// 4. Enumerate user tables: current database, non-internal, non-temporary, not in 'hypha'.
	const auto tables_result = Exec(con, R"(
SELECT schema_name, table_name
FROM duckdb_tables()
WHERE database_name = current_database()
  AND internal = false
  AND temporary = false
  AND schema_name NOT IN ('hypha', 'information_schema')
ORDER BY schema_name, table_name
)",
	                                "enumerate user tables");

	int64_t tables_captured = 0;
	int64_t columns_captured = 0;
	int64_t total_rows = 0;
	int64_t schemas_seen = 0;
	int64_t tables_hashed = 0;
	std::string last_schema;

	for (idx_t row = 0; row < tables_result->RowCount(); row++) {
		const auto schema_name = tables_result->GetValue(0, row).ToString();
		const auto table_name = tables_result->GetValue(1, row).ToString();

		if (schema_name != last_schema) {
			last_schema = schema_name;
			schemas_seen++;
		}

		// 5a. Enumerate columns, insert into hypha.column_snapshot, and collect fingerprint inputs.
		const auto cols_result = Exec(con,
		                              StringUtil::Format(R"(
SELECT column_name, column_index, data_type, is_nullable, coalesce(column_default, '')
FROM duckdb_columns()
WHERE database_name = current_database()
  AND schema_name = %s
  AND table_name  = %s
ORDER BY column_index
)",
		                                                 QuoteLiteral(schema_name), QuoteLiteral(table_name)),
		                              "enumerate columns");

		std::vector<std::pair<std::string, std::string>> fp_cols;
		std::vector<std::tuple<std::string, int, std::string, std::string, bool, std::string>> def_cols;

		for (idx_t c = 0; c < cols_result->RowCount(); c++) {
			const auto col_name = cols_result->GetValue(0, c).ToString();
			const auto ordinal_int = cols_result->GetValue(1, c).GetValue<int32_t>();
			const auto ordinal = cols_result->GetValue(1, c).ToString();
			const auto duckdb_type = cols_result->GetValue(2, c).ToString();
			const auto is_nullable_bool = cols_result->GetValue(3, c).GetValue<bool>();
			const auto is_nullable = is_nullable_bool ? "true" : "false";
			const auto default_expr_raw = cols_result->GetValue(4, c);
			const auto default_expr_str = default_expr_raw.IsNull() ? "" : default_expr_raw.ToString();
			const auto default_expr_sql = default_expr_raw.IsNull() ? "NULL" : QuoteLiteral(default_expr_str);
			const auto postgres_type = DuckTypeToPostgres(duckdb_type);

			Exec(con,
			     StringUtil::Format(R"(
INSERT INTO hypha.column_snapshot
    (commit_id, target_name, schema_name, table_name, column_name,
     ordinal_position, duckdb_type, postgres_type, is_nullable, default_expr)
VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
)",
			                        QuoteLiteral(commit_id), QuoteLiteral(target_name), QuoteLiteral(schema_name),
			                        QuoteLiteral(table_name), QuoteLiteral(col_name), ordinal,
			                        QuoteLiteral(duckdb_type), QuoteLiteral(postgres_type), is_nullable,
			                        default_expr_sql),
			     "INSERT hypha.column_snapshot");
			columns_captured++;

			fp_cols.emplace_back(col_name, duckdb_type);
			def_cols.emplace_back(col_name, ordinal_int, duckdb_type, postgres_type, is_nullable_bool,
			                      default_expr_str);
		}

		// 5b. Compute fingerprints. If any column type is unsupported, log a warning and
		//     leave hashes NULL for this table — never silently skip or fabricate.
		std::string table_hash_val;
		std::string def_hash_val;
		std::string obj_fp_val;
		int64_t row_count = 0;
		bool hashes_ok = false;

		try {
			const auto fp = ComputeTableFingerprint(con, schema_name, table_name, fp_cols);
			row_count = fp.row_count;
			table_hash_val = fp.table_hash;
			def_hash_val = ComputeDefinitionHash(con, schema_name, table_name, "table", def_cols);
			// content_hash = table_hash for base tables (spec §6.3)
			obj_fp_val = ComputeObjectFingerprint(con, def_hash_val, table_hash_val);
			hashes_ok = true;
			tables_hashed++;
		} catch (const NotImplementedException &e) {
			// Unsupported column type — hash that table. Warn and leave hash columns NULL.
			LogEvent(con, "warn", "base_snapshot_plan", "HASH_SKIP",
			         "skipped fingerprinting " + schema_name + "." + table_name + ": " + std::string(e.what()), "");
			// Still need row_count even without hash.
			const auto cr =
			    Exec(con, "SELECT COUNT(*)::BIGINT FROM " + QuoteIdent(schema_name) + "." + QuoteIdent(table_name),
			         "COUNT rows (no hash)");
			row_count = cr->GetValue(0, 0).GetValue<int64_t>();
		}

		total_rows += row_count;

		// 5c. Insert object_snapshot with computed hashes (or NULL if unsupported).
		const auto def_sql = def_hash_val.empty() ? "NULL" : QuoteLiteral(def_hash_val);
		const auto th_sql = table_hash_val.empty() ? "NULL" : QuoteLiteral(table_hash_val);
		const auto fp_sql = obj_fp_val.empty() ? "NULL" : QuoteLiteral(obj_fp_val);

		Exec(con,
		     StringUtil::Format(R"(
INSERT INTO hypha.object_snapshot
    (commit_id, target_name, schema_name, object_name, object_type,
     definition_hash, content_hash, object_fingerprint)
VALUES (%s, %s, %s, %s, 'table', %s, %s, %s)
)",
		                        QuoteLiteral(commit_id), QuoteLiteral(target_name), QuoteLiteral(schema_name),
		                        QuoteLiteral(table_name), def_sql, th_sql, fp_sql),
		     "INSERT hypha.object_snapshot");

		Exec(con,
		     StringUtil::Format(R"(
INSERT INTO hypha.table_snapshot
    (commit_id, target_name, schema_name, table_name, row_count, table_hash)
VALUES (%s, %s, %s, %s, %s, %s)
)",
		                        QuoteLiteral(commit_id), QuoteLiteral(target_name), QuoteLiteral(schema_name),
		                        QuoteLiteral(table_name), std::to_string(row_count), th_sql),
		     "INSERT hypha.table_snapshot");

		tables_captured++;
	}

	// 6. Mark commit as completed with fingerprint_algo.
	Exec(con,
	     StringUtil::Format(R"(
UPDATE hypha.commit
SET status = 'completed', fingerprint_algo = %s
WHERE commit_id = %s
)",
	                        QuoteLiteral(HYPHA_FINGERPRINT_ALGO), QuoteLiteral(commit_id)),
	     "UPDATE hypha.commit status");

	// 7. Log to event_log.
	const std::string details = "{\"commit_id\":\"" + commit_id + "\",\"tables\":" + std::to_string(tables_captured) +
	                            ",\"tables_hashed\":" + std::to_string(tables_hashed) +
	                            ",\"columns\":" + std::to_string(columns_captured) +
	                            ",\"total_rows\":" + std::to_string(total_rows) + "}";
	LogEvent(con, "info", "base_snapshot_plan", "OK",
	         "catalog snapshot: " + std::to_string(tables_captured) + " table(s), " + std::to_string(tables_hashed) +
	             " fingerprinted",
	         details);

	// 8. Build and return the status report.
	const bool all_hashed = (tables_hashed == tables_captured);
	std::ostringstream report;
	report << "commit_id=" << commit_id << "\n";
	report << "status=completed\n";
	report << "target_name=" << target_name << "\n";
	report << "database=" << current_db << "\n";
	report << "schemas_scanned=" << schemas_seen << "\n";
	report << "tables_captured=" << tables_captured << "\n";
	report << "tables_hashed=" << tables_hashed << "\n";
	report << "columns_captured=" << columns_captured << "\n";
	report << "total_rows=" << total_rows << "\n";
	report << "hashes_computed=" << (all_hashed ? "true" : "partial") << "\n";
	report << "fingerprint_algo=" << HYPHA_FINGERPRINT_ALGO << "\n";
	report << "note=local catalog captured; no remote writes";
	if (!all_hashed) {
		report << "; some tables have unsupported column types and were not fingerprinted (see event_log)";
	}
	return report.str();
}

// ---------------------------------------------------------------------------
// hypha_base_snapshot(): push local catalog + data to the Postgres target
// ---------------------------------------------------------------------------

namespace {

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

struct ColumnDef {
	std::string name;
	std::string postgres_type;
	bool is_nullable;
};

// Postgres max identifier length (NAMEDATALEN - 1).
static constexpr size_t PG_MAX_IDENT = 63;

//! Truncate to PG_MAX_IDENT bytes (UTF-8 safe via simple byte truncation; identifiers are
//! typically ASCII). Returns the truncated string.
std::string TruncPGIdent(const std::string &name) {
	if (name.size() <= PG_MAX_IDENT) {
		return name;
	}
	return name.substr(0, PG_MAX_IDENT);
}

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

std::string BuildCreateTableDDL(const std::string &pg_schema, const std::string &table_name,
                                const std::vector<ColumnDef> &cols, const std::vector<std::string> &pg_names) {
	std::ostringstream ddl;
	ddl << "CREATE TABLE " << QuoteIdent(pg_schema) << "." << QuoteIdent(TruncPGIdent(table_name)) << " (\n";
	for (size_t i = 0; i < cols.size(); i++) {
		ddl << "    " << QuoteIdent(pg_names[i]) << " " << cols[i].postgres_type;
		if (!cols[i].is_nullable) {
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
	const auto status = PQresultStatus(res);
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		const auto err = TrimPQ(PQresultErrorMessage(res));
		PQclear(res);
		throw IOException(StringUtil::Format("%s: %s", context, err));
	}
	PQclear(res);
}

} // anonymous namespace

std::string RunBaseSnapshot(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException("hypha metadata is not initialized — call hypha_init() with a Postgres URL first.");
	}

	const auto conn_string = GetDefaultTargetConnString(con);
	if (conn_string.empty()) {
		throw InvalidInputException("hypha_base_snapshot: no Postgres connection string found in hypha.target — "
		                            "call hypha_init() first.");
	}

	// Run a fresh snapshot plan to capture the current local catalog.
	RunBaseSnapshotPlan(con);

	// Retrieve the commit_id that was just created.
	const auto commit_result = Exec(con,
	                                R"(SELECT commit_id FROM hypha.commit
         WHERE kind = 'base_snapshot_plan' AND status = 'completed'
         ORDER BY created_at DESC LIMIT 1)",
	                                "get latest plan commit");
	if (commit_result->RowCount() == 0) {
		throw IOException("hypha_base_snapshot: no completed snapshot plan found after running plan");
	}
	const std::string commit_id = commit_result->GetValue(0, 0).ToString();
	const std::string db_name = GetCurrentDatabase(con);

	// Connect to Postgres.
	PGconn *pg = OpenHyphaConnection(conn_string);

	// Temp directory for intermediate CSVs (one file per table, deleted after use).
	const std::string tmp_dir = "/tmp/hypha_snap_" + commit_id.substr(0, 8);
	mkdir(tmp_dir.c_str(), 0700);

	// Begin a single transaction covering all schemas and tables.
	int64_t tables_synced = 0;
	int64_t rows_synced = 0;
	int64_t tables_skipped = 0;

	try {
		PGExec(pg, "BEGIN", "BEGIN transaction");

		// Create all required Postgres schemas.
		const auto schemas_result = Exec(con,
		                                 StringUtil::Format("SELECT DISTINCT schema_name FROM hypha.object_snapshot "
		                                                    "WHERE commit_id = %s ORDER BY schema_name",
		                                                    QuoteLiteral(commit_id)),
		                                 "get snapshot schemas");

		for (idx_t s = 0; s < schemas_result->RowCount(); s++) {
			const auto duckdb_schema = schemas_result->GetValue(0, s).ToString();
			const auto pg_schema = MakePostgresSchemaName(db_name, duckdb_schema);
			PGExec(pg, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdent(pg_schema), "CREATE SCHEMA");
		}

		// Process each table in the plan.
		const auto tables_result = Exec(con,
		                                StringUtil::Format("SELECT schema_name, object_name FROM hypha.object_snapshot "
		                                                   "WHERE commit_id = %s AND object_type = 'table' "
		                                                   "ORDER BY schema_name, object_name",
		                                                   QuoteLiteral(commit_id)),
		                                "get snapshot tables");

		for (idx_t t = 0; t < tables_result->RowCount(); t++) {
			const auto duckdb_schema = tables_result->GetValue(0, t).ToString();
			const auto table_name = tables_result->GetValue(1, t).ToString();
			const auto pg_schema = MakePostgresSchemaName(db_name, duckdb_schema);

			// Build column list from hypha.column_snapshot.
			const auto cols_result =
			    Exec(con,
			         StringUtil::Format("SELECT column_name, postgres_type, is_nullable "
			                            "FROM hypha.column_snapshot "
			                            "WHERE commit_id = %s AND schema_name = %s AND table_name = %s "
			                            "ORDER BY ordinal_position",
			                            QuoteLiteral(commit_id), QuoteLiteral(duckdb_schema), QuoteLiteral(table_name)),
			         "get columns");

			std::vector<ColumnDef> cols;
			for (idx_t c = 0; c < cols_result->RowCount(); c++) {
				const auto pg_type = cols_result->GetValue(1, c).ToString();
				// Skip columns whose DuckDB type has no Postgres mapping yet.
				if (StringUtil::StartsWith(pg_type, "(unsupported")) {
					continue;
				}
				ColumnDef cd;
				cd.name = cols_result->GetValue(0, c).ToString();
				cd.postgres_type = pg_type;
				cd.is_nullable = cols_result->GetValue(2, c).GetValue<bool>();
				cols.push_back(cd);
			}

			if (cols.empty()) {
				// Nothing to create if every column type is unsupported.
				continue;
			}

			// Use a savepoint per table so a per-table failure (e.g. Postgres row-too-big,
			// type mismatch) rolls back only that table and the rest of the sync continues.
			const std::string sp = "sp_" + SanitizeIdent(table_name).substr(0, 50);
			PGExec(pg, "SAVEPOINT " + sp, "SAVEPOINT");

			bool table_ok = true;
			std::string table_err;
			int64_t table_rows = 0;
			const std::string tmp_file =
			    tmp_dir + "/" + SanitizeIdent(duckdb_schema) + "_" + SanitizeIdent(table_name) + ".csv";

			try {
				// Drop-if-exists + recreate (schema is owned by hyphasync for this DB).
				// Resolve column names once — shared between DDL and SET STORAGE.
				const auto pg_names = ResolveColumnNames(cols);

				PGExec(pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(TruncPGIdent(table_name)),
				       "DROP TABLE IF EXISTS");
				PGExec(pg, BuildCreateTableDDL(pg_schema, table_name, cols, pg_names), "CREATE TABLE");

				// Force EXTERNAL storage for all variable-length columns so that
				// short-but-numerous text values are stored out-of-line, sidestepping
				// Postgres's 8 KB per-row limit on tables with many text columns.
				std::string alter_cols;
				for (size_t ci = 0; ci < cols.size(); ci++) {
					if (NeedsExternalStorage(cols[ci].postgres_type)) {
						if (!alter_cols.empty()) {
							alter_cols += ", ";
						}
						alter_cols += "ALTER COLUMN " + QuoteIdent(pg_names[ci]) + " SET STORAGE EXTERNAL";
					}
				}
				if (!alter_cols.empty()) {
					PGExec(pg,
					       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(TruncPGIdent(table_name)) + " " +
					           alter_cols,
					       "SET STORAGE EXTERNAL");
				}

				// Export DuckDB table to CSV, then stream into Postgres.
				Exec(con,
				     StringUtil::Format("COPY (SELECT * FROM %s.%s) TO %s "
				                        "(FORMAT CSV, HEADER FALSE, NULL '\\N')",
				                        QuoteIdent(duckdb_schema), QuoteIdent(table_name), QuoteLiteral(tmp_file)),
				     ("COPY TO CSV: " + table_name).c_str());

				const std::string copy_sql = "COPY " + QuoteIdent(pg_schema) + "." +
				                             QuoteIdent(TruncPGIdent(table_name)) + " FROM STDIN CSV NULL '\\N'";
				PGresult *copy_res = PQexec(pg, copy_sql.c_str());
				if (PQresultStatus(copy_res) != PGRES_COPY_IN) {
					const auto err = TrimPQ(PQresultErrorMessage(copy_res));
					PQclear(copy_res);
					remove(tmp_file.c_str());
					throw IOException(err);
				}
				PQclear(copy_res);

				FILE *f = fopen(tmp_file.c_str(), "r");
				if (!f) {
					PQputCopyEnd(pg, "could not open temp CSV file");
					remove(tmp_file.c_str());
					throw IOException("could not open temp file: " + tmp_file);
				}
				char buf[65536];
				int n;
				bool send_ok = true;
				while ((n = static_cast<int>(fread(buf, 1, sizeof(buf), f))) > 0) {
					if (PQputCopyData(pg, buf, n) != 1) {
						send_ok = false;
						break;
					}
				}
				fclose(f);
				remove(tmp_file.c_str());

				if (!send_ok) {
					PQputCopyEnd(pg, "data send error");
					throw IOException("PQputCopyData failed: " + TrimPQ(PQerrorMessage(pg)));
				}
				if (PQputCopyEnd(pg, nullptr) != 1) {
					throw IOException("PQputCopyEnd failed: " + TrimPQ(PQerrorMessage(pg)));
				}

				PGresult *copy_done = PQgetResult(pg);
				if (PQresultStatus(copy_done) != PGRES_COMMAND_OK) {
					const auto err = TrimPQ(PQresultErrorMessage(copy_done));
					PQclear(copy_done);
					throw IOException(err);
				}
				const char *tag = PQcmdTuples(copy_done);
				if (tag && *tag != '\0') {
					try {
						table_rows = std::stoll(tag);
					} catch (...) {
					}
				}
				PQclear(copy_done);

			} catch (const std::exception &ex) {
				table_ok = false;
				table_err = ex.what();
				remove(tmp_file.c_str());
				// Postgres transaction is now aborted; roll back to the savepoint.
				PGresult *rb = PQexec(pg, ("ROLLBACK TO SAVEPOINT " + sp).c_str());
				PQclear(rb);
			}

			PGExec(pg, "RELEASE SAVEPOINT " + sp, "RELEASE SAVEPOINT");

			if (table_ok) {
				rows_synced += table_rows;
				tables_synced++;
			} else {
				tables_skipped++;
				// If the error mentions row size, add a hint so the user knows what to do.
				std::string hint;
				if (table_err.find("too big") != std::string::npos || table_err.find("8160") != std::string::npos) {
					hint = " — table likely has too many fixed-width columns for Postgres's 8 KB page limit; "
					       "consider unpivoting to a long (entity, metric, value) format before syncing";
				}
				LogEvent(con, "warn", "base_snapshot", "TABLE_SKIP",
				         "skipped " + duckdb_schema + "." + table_name + ": " + table_err + hint, "");
			}
		}

		PGExec(pg, "COMMIT", "COMMIT transaction");

	} catch (...) {
		PGresult *rb = PQexec(pg, "ROLLBACK");
		if (rb) {
			PQclear(rb);
		}
		PQfinish(pg);
		rmdir(tmp_dir.c_str());
		throw;
	}

	PQfinish(pg);
	rmdir(tmp_dir.c_str());

	// Mark the plan commit as applied.
	Exec(con,
	     StringUtil::Format("UPDATE hypha.commit SET status = 'applied', applied_at = now() "
	                        "WHERE commit_id = %s",
	                        QuoteLiteral(commit_id)),
	     "mark commit applied");

	const std::string details = "{\"commit_id\":\"" + commit_id + "\",\"tables\":" + std::to_string(tables_synced) +
	                            ",\"rows\":" + std::to_string(rows_synced) + ",\"database\":\"" + db_name + "\"}";
	LogEvent(con, "info", "base_snapshot", "OK",
	         "base snapshot applied: " + std::to_string(tables_synced) + " table(s), " + std::to_string(rows_synced) +
	             " row(s)",
	         details);

	const std::string pg_schema_prefix = SanitizeIdent(db_name) + "_";
	std::ostringstream report;
	report << "commit_id=" << commit_id << "\n";
	report << "status=applied\n";
	report << "target_name=default\n";
	report << "database=" << db_name << "\n";
	report << "tables_synced=" << tables_synced << "\n";
	report << "tables_skipped=" << tables_skipped << "\n";
	report << "rows_synced=" << rows_synced << "\n";
	report << "postgres_schema_prefix=" << pg_schema_prefix << "\n";
	report << "note=schemas created as " << pg_schema_prefix
	       << "<duckdb_schema>; fingerprints computed by snapshot plan; "
	       << "re-running hypha_base_snapshot() will DROP and re-copy all tables";
	return report.str();
}

// ---------------------------------------------------------------------------
// Sync diff helpers
// ---------------------------------------------------------------------------

namespace {

struct TableAction {
	enum class Kind { NEW, DROPPED, SCHEMA_CHANGED, ROWS_CHANGED, LIKELY_UNCHANGED };
	Kind kind;
	std::string schema_name;
	std::string table_name;
	int64_t old_rows = 0;
	int64_t new_rows = 0;
	std::string detail;
};

std::string ActionLabel(TableAction::Kind k) {
	switch (k) {
	case TableAction::Kind::NEW:
		return "new";
	case TableAction::Kind::DROPPED:
		return "dropped";
	case TableAction::Kind::SCHEMA_CHANGED:
		return "schema_changed";
	case TableAction::Kind::ROWS_CHANGED:
		return "rows_changed";
	case TableAction::Kind::LIKELY_UNCHANGED:
		return "likely_unchanged";
	}
	return "unknown";
}

//! Computes the diff between old and new snapshot commits using a fingerprint-first
//! comparison hierarchy (spec §7). Falls back to column-signature + row-count when
//! fingerprints are NULL (snapshots captured before fingerprinting was implemented).
std::vector<TableAction> ComputeDiff(Connection &con, const std::string &old_commit_id,
                                     const std::string &new_commit_id) {
	std::vector<TableAction> actions;

	// New tables.
	const auto new_tables = Exec(con,
	                             StringUtil::Format(R"(
SELECT n.schema_name, n.object_name, COALESCE(ts.row_count, 0) AS new_rows
FROM hypha.object_snapshot n
LEFT JOIN hypha.table_snapshot ts
  ON ts.commit_id = n.commit_id AND ts.schema_name = n.schema_name AND ts.table_name = n.object_name
WHERE n.commit_id = %s AND n.object_type = 'table'
  AND NOT EXISTS (SELECT 1 FROM hypha.object_snapshot o
    WHERE o.commit_id = %s AND o.schema_name = n.schema_name AND o.object_name = n.object_name)
ORDER BY n.schema_name, n.object_name
)",
	                                                QuoteLiteral(new_commit_id), QuoteLiteral(old_commit_id)),
	                             "diff: new tables");
	for (idx_t i = 0; i < new_tables->RowCount(); i++) {
		TableAction a;
		a.kind = TableAction::Kind::NEW;
		a.schema_name = new_tables->GetValue(0, i).ToString();
		a.table_name = new_tables->GetValue(1, i).ToString();
		a.new_rows = new_tables->GetValue(2, i).GetValue<int64_t>();
		actions.push_back(a);
	}

	// Dropped tables.
	const auto dropped = Exec(con,
	                          StringUtil::Format(R"(
SELECT o.schema_name, o.object_name
FROM hypha.object_snapshot o
WHERE o.commit_id = %s AND o.object_type = 'table'
  AND NOT EXISTS (SELECT 1 FROM hypha.object_snapshot n
    WHERE n.commit_id = %s AND n.schema_name = o.schema_name AND n.object_name = o.object_name)
ORDER BY o.schema_name, o.object_name
)",
	                                             QuoteLiteral(old_commit_id), QuoteLiteral(new_commit_id)),
	                          "diff: dropped tables");
	for (idx_t i = 0; i < dropped->RowCount(); i++) {
		TableAction a;
		a.kind = TableAction::Kind::DROPPED;
		a.schema_name = dropped->GetValue(0, i).ToString();
		a.table_name = dropped->GetValue(1, i).ToString();
		actions.push_back(a);
	}

	// Tables present in both — fingerprint-first comparison (spec §7).
	const auto common = Exec(con,
	                         StringUtil::Format(R"(
SELECT
    n.schema_name, n.object_name,
    n.object_fingerprint AS new_fp,  o.object_fingerprint AS old_fp,
    n.definition_hash    AS new_def, o.definition_hash    AS old_def,
    nt.row_count         AS new_rows,ot.row_count         AS old_rows,
    nt.table_hash        AS new_th,  ot.table_hash        AS old_th
FROM hypha.object_snapshot n
JOIN hypha.object_snapshot o
  ON o.commit_id = %s AND o.schema_name = n.schema_name AND o.object_name = n.object_name
LEFT JOIN hypha.table_snapshot nt
  ON nt.commit_id = n.commit_id AND nt.schema_name = n.schema_name AND nt.table_name = n.object_name
LEFT JOIN hypha.table_snapshot ot
  ON ot.commit_id = o.commit_id AND ot.schema_name = o.schema_name AND ot.table_name = o.object_name
WHERE n.commit_id = %s AND n.object_type = 'table'
ORDER BY n.schema_name, n.object_name
)",
	                                            QuoteLiteral(old_commit_id), QuoteLiteral(new_commit_id)),
	                         "diff: common tables");

	for (idx_t i = 0; i < common->RowCount(); i++) {
		const auto schema = common->GetValue(0, i).ToString();
		const auto table = common->GetValue(1, i).ToString();
		const auto new_fp = common->GetValue(2, i);
		const auto old_fp = common->GetValue(3, i);
		const auto new_def = common->GetValue(4, i);
		const auto old_def = common->GetValue(5, i);
		const auto new_rows = common->GetValue(6, i).IsNull() ? 0LL : common->GetValue(6, i).GetValue<int64_t>();
		const auto old_rows = common->GetValue(7, i).IsNull() ? 0LL : common->GetValue(7, i).GetValue<int64_t>();
		const auto new_th = common->GetValue(8, i);
		const auto old_th = common->GetValue(9, i);

		TableAction a;
		a.schema_name = schema;
		a.table_name = table;
		a.new_rows = new_rows;
		a.old_rows = old_rows;

		// Level 1: object_fingerprint equal → definitively unchanged (cheapest).
		if (!new_fp.IsNull() && !old_fp.IsNull() && new_fp.ToString() == old_fp.ToString()) {
			a.kind = TableAction::Kind::LIKELY_UNCHANGED;
			a.detail = "fingerprint_match";

			// Level 2: definition_hash differs → schema changed.
		} else if (!new_def.IsNull() && !old_def.IsNull() && new_def.ToString() != old_def.ToString()) {
			a.kind = TableAction::Kind::SCHEMA_CHANGED;
			a.detail = "definition_hash_changed";

			// Level 3: table_hash equal (and row_count matches) → data unchanged.
		} else if (!new_th.IsNull() && !old_th.IsNull() && new_rows == old_rows &&
		           new_th.ToString() == old_th.ToString()) {
			a.kind = TableAction::Kind::LIKELY_UNCHANGED;
			a.detail = "table_hash_match";

			// Level 4: table_hash differs → data changed (detects same-count updates/deletes).
		} else if (!new_th.IsNull() && !old_th.IsNull() && new_th.ToString() != old_th.ToString()) {
			a.kind = TableAction::Kind::ROWS_CHANGED;
			a.detail = "table_hash_changed";

			// Fallback (no fingerprints): row count comparison only.
		} else if (new_rows != old_rows) {
			a.kind = TableAction::Kind::ROWS_CHANGED;
			a.detail = "row_count_changed";

		} else {
			// No fingerprints, same count — check column signature as last resort.
			a.kind = TableAction::Kind::LIKELY_UNCHANGED;
			a.detail = "no_hashes_same_count";
			const auto col_diff =
			    Exec(con,
			         StringUtil::Format(R"(
WITH ns AS (SELECT string_agg(column_name||':'||duckdb_type,',' ORDER BY ordinal_position) AS sig
            FROM hypha.column_snapshot WHERE commit_id=%s AND schema_name=%s AND table_name=%s),
     os AS (SELECT string_agg(column_name||':'||duckdb_type,',' ORDER BY ordinal_position) AS sig
            FROM hypha.column_snapshot WHERE commit_id=%s AND schema_name=%s AND table_name=%s)
SELECT ns.sig IS DISTINCT FROM os.sig AS changed FROM ns, os
)",
			                            QuoteLiteral(new_commit_id), QuoteLiteral(schema), QuoteLiteral(table),
			                            QuoteLiteral(old_commit_id), QuoteLiteral(schema), QuoteLiteral(table)),
			         "diff: col sig fallback");
			if (col_diff->RowCount() > 0 && !col_diff->GetValue(0, 0).IsNull() &&
			    col_diff->GetValue(0, 0).GetValue<bool>()) {
				a.kind = TableAction::Kind::SCHEMA_CHANGED;
				a.detail = "column_signature_changed";
			}
		}
		actions.push_back(a);
	}

	return actions;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// hypha_sync_plan(): diff only, no Postgres writes
// ---------------------------------------------------------------------------

std::string RunSyncPlan(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException("hypha metadata is not initialized — call hypha_init() first.");
	}

	// Require a prior applied snapshot to diff against.
	const auto base_result = Exec(con, R"(
SELECT commit_id, COALESCE(fingerprint_algo, 'none') AS old_algo FROM hypha.commit
WHERE status = 'applied'
ORDER BY applied_at DESC NULLS LAST, created_at DESC
LIMIT 1)",
	                              "get last applied commit");
	if (base_result->RowCount() == 0) {
		throw InvalidInputException(
		    "hypha_sync_plan: no applied base snapshot found — call hypha_base_snapshot() first "
		    "to establish the initial Postgres state before planning incremental syncs.");
	}
	const std::string old_commit_id = base_result->GetValue(0, 0).ToString();
	const std::string old_fp_algo = base_result->GetValue(1, 0).ToString();
	const std::string db_name = GetCurrentDatabase(con);

	// Warn if the prior snapshot has no fingerprints; the diff will fall back to row-count only.
	if (old_fp_algo == "none") {
		LogEvent(con, "warn", "sync_plan", "OLD_SNAPSHOT_UNFINGERPRINTED",
		         "last applied snapshot has no fingerprints (pre-v1); diff falls back to "
		         "column-signature + row-count only. Run hypha_base_snapshot() for a full "
		         "fingerprint baseline before relying on hypha_sync() to detect all changes.",
		         "");
	}

	// Run a fresh catalog walk to capture the current local state.
	RunBaseSnapshotPlan(con);
	const auto new_commit_result = Exec(con, R"(
SELECT commit_id FROM hypha.commit
WHERE kind = 'base_snapshot_plan' AND status = 'completed'
ORDER BY created_at DESC LIMIT 1)",
	                                    "get new plan commit");
	const std::string new_commit_id = new_commit_result->GetValue(0, 0).ToString();

	const auto actions = ComputeDiff(con, old_commit_id, new_commit_id);

	// Tally by action kind.
	int64_t n_new = 0, n_dropped = 0, n_schema = 0, n_rows = 0, n_skip = 0;
	for (const auto &a : actions) {
		switch (a.kind) {
		case TableAction::Kind::NEW:
			n_new++;
			break;
		case TableAction::Kind::DROPPED:
			n_dropped++;
			break;
		case TableAction::Kind::SCHEMA_CHANGED:
			n_schema++;
			break;
		case TableAction::Kind::ROWS_CHANGED:
			n_rows++;
			break;
		case TableAction::Kind::LIKELY_UNCHANGED:
			n_skip++;
			break;
		}
	}
	const int64_t n_to_sync = n_new + n_dropped + n_schema + n_rows;

	// Record a sync_plan commit for traceability.
	const auto plan_id_result = Exec(con, "SELECT uuid()::VARCHAR", "gen sync_plan id");
	const std::string plan_commit_id = plan_id_result->GetValue(0, 0).ToString();
	Exec(con,
	     StringUtil::Format(R"(
INSERT INTO hypha.commit (commit_id, parent_commit_id, target_name, kind, message, status)
VALUES (%s, %s, 'default', 'sync_plan', 'sync plan: %s tables to sync', 'planned')
)",
	                        QuoteLiteral(plan_commit_id), QuoteLiteral(new_commit_id),
	                        std::to_string(n_to_sync).c_str()),
	     "INSERT sync_plan commit");

	LogEvent(con, "info", "sync_plan", "OK",
	         "sync plan computed: " + std::to_string(n_to_sync) + " tables to sync, " + std::to_string(n_skip) +
	             " skipped",
	         "");

	std::ostringstream report;
	report << "commit_id=" << plan_commit_id << "\n";
	report << "status=planned\n";
	report << "target_name=default\n";
	report << "database=" << db_name << "\n";
	report << "based_on_commit=" << old_commit_id << "\n";
	report << "fingerprint_algo=" << HYPHA_FINGERPRINT_ALGO << "\n";
	report << "old_fingerprint_algo=" << old_fp_algo << "\n";
	report << "new_tables=" << n_new << "\n";
	report << "dropped_tables=" << n_dropped << "\n";
	report << "schema_changed_tables=" << n_schema << "\n";
	report << "rows_changed_tables=" << n_rows << "\n";
	report << "likely_unchanged_tables=" << n_skip << "\n";
	report << "tables_to_sync=" << n_to_sync << "\n";

	for (const auto &a : actions) {
		if (a.kind == TableAction::Kind::LIKELY_UNCHANGED) {
			continue; // Don't spam the report with unchanged tables
		}
		report << "action=" << ActionLabel(a.kind) << " schema=" << a.schema_name << " table=" << a.table_name;
		if (a.kind == TableAction::Kind::SCHEMA_CHANGED) {
			report << " detail=" << a.detail;
		} else if (a.kind != TableAction::Kind::DROPPED) {
			report << " old_rows=" << a.old_rows << " new_rows=" << a.new_rows;
		}
		report << "\n";
	}

	report << "note=fingerprint_algo=" << HYPHA_FINGERPRINT_ALGO << "; table_hash diff detects all changes "
	       << "including same-count updates/deletes; row-level INSERT/UPDATE/DELETE diff planned for v2";
	return report.str();
}

// ---------------------------------------------------------------------------
// hypha_sync(): apply the plan
// ---------------------------------------------------------------------------

std::string RunSync(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException("hypha metadata is not initialized — call hypha_init() first.");
	}

	// Require a prior applied snapshot.
	const auto base_result = Exec(con, R"(
SELECT commit_id, COALESCE(fingerprint_algo, 'none') AS old_algo FROM hypha.commit
WHERE status = 'applied'
ORDER BY applied_at DESC NULLS LAST, created_at DESC
LIMIT 1)",
	                              "get last applied commit");
	if (base_result->RowCount() == 0) {
		throw InvalidInputException("hypha_sync: no applied base snapshot found — call hypha_base_snapshot() first.");
	}
	const std::string old_commit_id = base_result->GetValue(0, 0).ToString();
	const std::string old_fp_algo = base_result->GetValue(1, 0).ToString();
	const std::string conn_string = GetDefaultTargetConnString(con);
	if (conn_string.empty()) {
		throw InvalidInputException("hypha_sync: no Postgres connection string found — call hypha_init() first.");
	}
	const std::string db_name = GetCurrentDatabase(con);

	// Fresh catalog walk.
	RunBaseSnapshotPlan(con);
	const auto new_commit_result = Exec(con, R"(
SELECT commit_id FROM hypha.commit
WHERE kind = 'base_snapshot_plan' AND status = 'completed'
ORDER BY created_at DESC LIMIT 1)",
	                                    "get new plan commit");
	const std::string new_commit_id = new_commit_result->GetValue(0, 0).ToString();

	const auto actions = ComputeDiff(con, old_commit_id, new_commit_id);

	// Connect to Postgres.
	PGconn *pg = OpenHyphaConnection(conn_string);
	const std::string tmp_dir = "/tmp/hypha_sync_" + new_commit_id.substr(0, 8);
	mkdir(tmp_dir.c_str(), 0700);

	int64_t tables_synced = 0, tables_skipped = 0, tables_dropped = 0, rows_synced = 0;

	try {
		PGExec(pg, "BEGIN", "BEGIN transaction");

		for (const auto &a : actions) {
			const auto pg_schema = MakePostgresSchemaName(db_name, a.schema_name);
			const auto pg_table = TruncPGIdent(a.table_name);

			if (a.kind == TableAction::Kind::LIKELY_UNCHANGED) {
				tables_skipped++;
				continue;
			}

			// Use a savepoint so a per-table failure skips that table.
			const auto sp = "sp_" + SanitizeIdent(a.table_name).substr(0, 50);
			PGExec(pg, "SAVEPOINT " + sp, "SAVEPOINT");

			bool ok = true;
			std::string err_msg;
			int64_t table_rows = 0;

			try {
				if (a.kind == TableAction::Kind::DROPPED) {
					PGExec(pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table),
					       "DROP TABLE");
					tables_dropped++;

				} else {
					// NEW, SCHEMA_CHANGED, ROWS_CHANGED all need the column list.
					const auto cols_result =
					    Exec(con,
					         StringUtil::Format(R"(
SELECT column_name, postgres_type, is_nullable
FROM hypha.column_snapshot
WHERE commit_id=%s AND schema_name=%s AND table_name=%s
ORDER BY ordinal_position)",
					                            QuoteLiteral(new_commit_id), QuoteLiteral(a.schema_name),
					                            QuoteLiteral(a.table_name)),
					         "get columns");

					std::vector<ColumnDef> cols;
					for (idx_t c = 0; c < cols_result->RowCount(); c++) {
						const auto pt = cols_result->GetValue(1, c).ToString();
						if (StringUtil::StartsWith(pt, "(unsupported")) {
							continue;
						}
						ColumnDef cd;
						cd.name = cols_result->GetValue(0, c).ToString();
						cd.postgres_type = pt;
						cd.is_nullable = cols_result->GetValue(2, c).GetValue<bool>();
						cols.push_back(cd);
					}
					if (cols.empty()) {
						tables_skipped++;
						PGExec(pg, "RELEASE SAVEPOINT " + sp, "RELEASE SAVEPOINT");
						continue;
					}

					const auto pg_names = ResolveColumnNames(cols);

					if (a.kind == TableAction::Kind::NEW || a.kind == TableAction::Kind::SCHEMA_CHANGED) {
						// For schema changes: drop the old table; for new: no-op.
						PGExec(pg, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdent(pg_schema),
						       "CREATE SCHEMA IF NOT EXISTS");
						PGExec(pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table),
						       "DROP TABLE IF EXISTS");
						PGExec(pg, BuildCreateTableDDL(pg_schema, a.table_name, cols, pg_names), "CREATE TABLE");
						// Force EXTERNAL storage for text columns.
						std::string alter;
						for (size_t ci = 0; ci < cols.size(); ci++) {
							if (NeedsExternalStorage(cols[ci].postgres_type)) {
								if (!alter.empty()) {
									alter += ", ";
								}
								alter += "ALTER COLUMN " + QuoteIdent(pg_names[ci]) + " SET STORAGE EXTERNAL";
							}
						}
						if (!alter.empty()) {
							PGExec(pg,
							       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " " + alter,
							       "SET STORAGE EXTERNAL");
						}
					} else {
						// ROWS_CHANGED: keep schema, just replace data.
						PGExec(pg, "TRUNCATE TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table), "TRUNCATE");
					}

					// COPY data.
					const std::string tmp_file =
					    tmp_dir + "/" + SanitizeIdent(a.schema_name) + "_" + SanitizeIdent(a.table_name) + ".csv";
					Exec(con,
					     StringUtil::Format("COPY (SELECT * FROM %s.%s) TO %s (FORMAT CSV, HEADER FALSE, NULL '\\N')",
					                        QuoteIdent(a.schema_name), QuoteIdent(a.table_name),
					                        QuoteLiteral(tmp_file)),
					     "COPY TO CSV");

					const std::string copy_sql =
					    "COPY " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " FROM STDIN CSV NULL '\\N'";
					PGresult *cr = PQexec(pg, copy_sql.c_str());
					if (PQresultStatus(cr) != PGRES_COPY_IN) {
						const auto e = TrimPQ(PQresultErrorMessage(cr));
						PQclear(cr);
						remove(tmp_file.c_str());
						throw IOException(e);
					}
					PQclear(cr);

					FILE *f = fopen(tmp_file.c_str(), "r");
					if (!f) {
						PQputCopyEnd(pg, "open failed");
						remove(tmp_file.c_str());
						throw IOException("could not open " + tmp_file);
					}
					char buf[65536];
					int n;
					bool send_ok = true;
					while ((n = static_cast<int>(fread(buf, 1, sizeof(buf), f))) > 0) {
						if (PQputCopyData(pg, buf, n) != 1) {
							send_ok = false;
							break;
						}
					}
					fclose(f);
					remove(tmp_file.c_str());
					if (!send_ok) {
						PQputCopyEnd(pg, "send error");
						throw IOException("PQputCopyData failed");
					}
					if (PQputCopyEnd(pg, nullptr) != 1) {
						throw IOException("PQputCopyEnd failed");
					}

					PGresult *done = PQgetResult(pg);
					if (PQresultStatus(done) != PGRES_COMMAND_OK) {
						const auto e = TrimPQ(PQresultErrorMessage(done));
						PQclear(done);
						throw IOException(e);
					}
					const char *tag = PQcmdTuples(done);
					if (tag && *tag != '\0') {
						try {
							table_rows = std::stoll(tag);
						} catch (...) {
						}
					}
					PQclear(done);
					tables_synced++;
					rows_synced += table_rows;
				}

			} catch (const std::exception &ex) {
				ok = false;
				err_msg = ex.what();
				PGresult *rb = PQexec(pg, ("ROLLBACK TO SAVEPOINT " + sp).c_str());
				PQclear(rb);
				tables_skipped++;
				LogEvent(con, "warn", "sync", "TABLE_SKIP",
				         "skipped " + a.schema_name + "." + a.table_name + ": " + err_msg, "");
			}
			(void)ok;
			PGExec(pg, "RELEASE SAVEPOINT " + sp, "RELEASE SAVEPOINT");
		}

		PGExec(pg, "COMMIT", "COMMIT transaction");

	} catch (...) {
		PGresult *rb = PQexec(pg, "ROLLBACK");
		if (rb)
			PQclear(rb);
		PQfinish(pg);
		rmdir(tmp_dir.c_str());
		throw;
	}

	PQfinish(pg);
	rmdir(tmp_dir.c_str());

	// Mark as applied.
	const auto sync_id_result = Exec(con, "SELECT uuid()::VARCHAR", "gen sync commit id");
	const std::string sync_commit_id = sync_id_result->GetValue(0, 0).ToString();
	Exec(con,
	     StringUtil::Format(R"(
INSERT INTO hypha.commit (commit_id, parent_commit_id, target_name, kind, message, applied_at, status)
VALUES (%s, %s, 'default', 'sync',
        'sync: %s table(s) synced, %s dropped, %s skipped',
        now(), 'applied')
)",
	                        QuoteLiteral(sync_commit_id), QuoteLiteral(new_commit_id),
	                        std::to_string(tables_synced).c_str(), std::to_string(tables_dropped).c_str(),
	                        std::to_string(tables_skipped).c_str()),
	     "INSERT sync commit");

	const std::string details = "{\"commit_id\":\"" + sync_commit_id +
	                            "\",\"tables_synced\":" + std::to_string(tables_synced) +
	                            ",\"tables_dropped\":" + std::to_string(tables_dropped) +
	                            ",\"rows_synced\":" + std::to_string(rows_synced) + "}";
	LogEvent(con, "info", "sync", "OK",
	         "sync complete: " + std::to_string(tables_synced) + " table(s) synced, " + std::to_string(tables_dropped) +
	             " dropped, " + std::to_string(tables_skipped) + " skipped",
	         details);

	std::ostringstream report;
	report << "commit_id=" << sync_commit_id << "\n";
	report << "status=applied\n";
	report << "target_name=default\n";
	report << "database=" << db_name << "\n";
	report << "tables_synced=" << tables_synced << "\n";
	report << "tables_dropped=" << tables_dropped << "\n";
	report << "tables_skipped=" << tables_skipped << "\n";
	report << "rows_synced=" << rows_synced << "\n";
	report << "note=fingerprint_algo=" << HYPHA_FINGERPRINT_ALGO << "; table_hash diff detects all changes; "
	       << "row-level INSERT/UPDATE/DELETE diff planned for v2";
	return report.str();
}

} // namespace duckdb
