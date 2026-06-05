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
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

std::string RunBaseSnapshotPlan(Connection &con, bool is_base_snapshot) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException(
		    "hypha metadata is not initialized — call hypha_init() with a Postgres connection string first.");
	}

	// Ensure the JSON extension is available for LIST/STRUCT/MAP ::JSON casts.
	// INSTALL is a no-op when already at the current version; LOAD is a no-op when already loaded.
	// Both are attempted automatically so users do not need to manage this dependency themselves.
	// If the install fails (e.g. offline environment with no cached extension), we fall back
	// gracefully: compound-type columns skip fingerprinting (HASH_SKIP) and fail at COPY time
	// (caught by the per-table savepoint) rather than aborting the whole snapshot.
	con.Query("INSTALL json");
	con.Query("LOAD json");

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
	int64_t n_exact = 0;
	int64_t n_append_only = 0;
	int64_t n_mutable_entity = 0;
	std::string last_schema;

	// exact_verify mode forces the full per-row EXACT fingerprint for every table, closing the
	// MUTABLE_ENTITY in-place-update blind spot (docs/fingerprinting.md §6.4) at O(n) scan cost.
	const bool force_exact = GetExactVerify(con);
	if (force_exact) {
		LogEvent(con, "info", "base_snapshot_plan", "EXACT_VERIFY",
		         "exact_verify mode active: every table is fingerprinted with the full per-row EXACT strategy", "");
		Printer::Print(OutputStream::STREAM_STDERR,
		               "[hyphasync] exact_verify mode: forcing full per-row hash for all tables");
	}

	// Per-schema sets for collision-safe table name truncation.
	// Scoped per schema because tables in different schemas don't conflict in Postgres.
	std::unordered_map<std::string, std::unordered_set<std::string>> schema_pg_name_sets;

	// Log overall snapshot plan start for progress visibility.
	LogEvent(con, "info", "base_snapshot_plan", "START",
	         "catalog snapshot started: database '" + current_db + "', " + std::to_string(tables_result->RowCount()) +
	             " tables pending",
	         "{\"commit_id\":\"" + commit_id + "\",\"tables_pending\":" + std::to_string(tables_result->RowCount()) +
	             "}");

	Printer::Print(OutputStream::STREAM_STDERR, StringUtil::Format("[hyphasync] fingerprinting %lld tables...",
	                                                               (long long)tables_result->RowCount()));

	for (idx_t row = 0; row < tables_result->RowCount(); row++) {
		const auto schema_name = tables_result->GetValue(0, row).ToString();
		const auto table_name = tables_result->GetValue(1, row).ToString();

		// Assign a collision-safe Postgres table name (scoped per schema).
		const auto pg_table_name = SafeTruncateIdent(table_name, schema_pg_name_sets[schema_name]);
		if (pg_table_name != table_name) {
			LogEvent(con, "warn", "base_snapshot_plan", "NAME_TRUNCATED",
			         schema_name + "." + table_name + " truncated/renamed to pg identifier '" + pg_table_name + "'",
			         "{\"schema\":\"" + schema_name + "\",\"original_name\":\"" + table_name +
			             "\",\"pg_table_name\":\"" + pg_table_name + "\"}");
		}

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

		// Collect per-column metadata; build one batched INSERT for all columns.
		struct ColRow {
			std::string col_name;
			std::string ordinal;
			std::string duckdb_type;
			std::string postgres_type;
			std::string is_nullable;
			std::string default_expr_sql;
		};
		std::vector<ColRow> col_rows;

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

			// Log TYPE_COERCE for DuckDB types whose precision is truncated in Postgres.
			// TIMESTAMP_S/MS/NS all map to `timestamp without time zone` but lose sub-second
			// or sub-microsecond precision. Surface this so operators can detect potential loss.
			{
				const auto dt = StringUtil::Upper(duckdb_type);
				if (dt == "TIMESTAMP_S" || dt == "TIMESTAMP_MS" || dt == "TIMESTAMP_NS") {
					LogEvent(con, "warn", "base_snapshot_plan", "TYPE_COERCE",
					         schema_name + "." + table_name + " column '" + col_name + "': " + duckdb_type +
					             " mapped to " + postgres_type + " (precision may be truncated)",
					         "{\"schema\":\"" + schema_name + "\",\"table\":\"" + table_name + "\",\"column\":\"" +
					             col_name + "\",\"from_type\":\"" + duckdb_type + "\",\"to_type\":\"" + postgres_type +
					             "\"}");
				}
			}

			col_rows.push_back({col_name, ordinal, duckdb_type, postgres_type, is_nullable, default_expr_sql});

			fp_cols.emplace_back(col_name, duckdb_type);
			def_cols.emplace_back(col_name, ordinal_int, duckdb_type, postgres_type, is_nullable_bool,
			                      default_expr_str);
		}

		if (!col_rows.empty()) {
			static constexpr size_t COL_BATCH_SIZE = 500;
			for (size_t batch_start = 0; batch_start < col_rows.size(); batch_start += COL_BATCH_SIZE) {
				const size_t batch_end = std::min(batch_start + COL_BATCH_SIZE, col_rows.size());
				std::ostringstream col_insert;
				col_insert << "INSERT INTO hypha.column_snapshot\n"
				           << "    (commit_id, target_name, schema_name, table_name, column_name,\n"
				           << "     ordinal_position, duckdb_type, postgres_type, is_nullable, default_expr)\n"
				           << "VALUES\n";
				for (size_t ci = batch_start; ci < batch_end; ci++) {
					if (ci > batch_start) {
						col_insert << ",\n";
					}
					const auto &cr = col_rows[ci];
					col_insert << "    (" << QuoteLiteral(commit_id) << ", " << QuoteLiteral(target_name) << ", "
					           << QuoteLiteral(schema_name) << ", " << QuoteLiteral(table_name) << ", "
					           << QuoteLiteral(cr.col_name) << ", " << cr.ordinal << ", "
					           << QuoteLiteral(cr.duckdb_type) << ", " << QuoteLiteral(cr.postgres_type) << ", "
					           << cr.is_nullable << ", " << cr.default_expr_sql << ")";
				}
				Exec(con, col_insert.str(), "INSERT hypha.column_snapshot");
			}
			columns_captured += static_cast<int64_t>(col_rows.size());
		}

		// 5b. Compute fingerprints. If any column type is unsupported, log a warning and
		//     leave hashes NULL for this table — never silently skip or fabricate.
		std::string table_hash_val;
		std::string def_hash_val;
		std::string obj_fp_val;
		std::string strategy_name_val;
		int64_t row_count = 0;
		bool hashes_ok = false;

		// Per-table fingerprint start — written before classification so a monitoring query
		// can see in-progress tables even for slow fingerprints.
		LogEvent(con, "info", "base_snapshot_plan", "FINGERPRINT_START",
		         schema_name + "." + table_name + ": classifying and fingerprinting", "");
		const auto fp_t0 = std::chrono::steady_clock::now();

		try {
			const auto fp =
			    ComputeTableFingerprint(con, schema_name, table_name, fp_cols, is_base_snapshot, force_exact);
			const auto fp_ms =
			    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - fp_t0).count();
			row_count = fp.row_count;
			table_hash_val = fp.table_hash;
			strategy_name_val = fp.strategy_name;
			if (fp_ms > 10) {
				const std::string fp_label = schema_name + "." + table_name;
				const std::string fp_padded =
				    fp_label.size() < 40 ? fp_label + std::string(40 - fp_label.size(), ' ') : fp_label;
				Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync]   " + fp_padded + "  " + fp.strategy_name +
				                                                "  " + std::to_string(fp.row_count) + " rows  " +
				                                                std::to_string(fp_ms) + "ms");
			}
			def_hash_val = ComputeDefinitionHash(con, schema_name, table_name, "table", def_cols);
			// content_hash = table_hash for base tables (spec §6.3)
			obj_fp_val = ComputeObjectFingerprint(con, def_hash_val, table_hash_val);
			hashes_ok = true;
			tables_hashed++;
			// Log strategy classification and completion together.
			LogEvent(con, "info", "base_snapshot_plan", "FINGERPRINT_DONE",
			         schema_name + "." + table_name + ": strategy=" + fp.strategy_name + ", " +
			             std::to_string(row_count) + " rows, " + std::to_string(fp_ms) + "ms",
			         "{\"schema\":\"" + schema_name + "\",\"table\":\"" + table_name + "\",\"strategy\":\"" +
			             fp.strategy_name + "\",\"rationale\":\"" + fp.strategy_rationale + "\",\"rows\":" +
			             std::to_string(row_count) + ",\"duration_ms\":" + std::to_string(fp_ms) + "}");
		} catch (const std::exception &e) {
			// Any fingerprinting failure (unsupported type, missing JSON extension for nested
			// types, etc.) — warn and leave hash columns NULL rather than aborting the snapshot.
			// Tables with nested columns require `INSTALL json; LOAD json;` to be fingerprinted.
			LogEvent(con, "warn", "base_snapshot_plan", "HASH_SKIP",
			         "skipped fingerprinting " + schema_name + "." + table_name + ": " + std::string(e.what()), "");
			// Still need row_count even without hash.
			const auto cr =
			    Exec(con, "SELECT COUNT(*)::BIGINT FROM " + QuoteIdent(schema_name) + "." + QuoteIdent(table_name),
			         "COUNT rows (no hash)");
			row_count = cr->GetValue(0, 0).GetValue<int64_t>();
		}

		total_rows += row_count;

		// 5c. Detect primary key columns for this table.
		// pk_columns: comma-separated sorted column names, or empty for no PK.
		// Both single-column and composite PKs are used for row-level diff.
		const auto pk_result = Exec(con,
		                            StringUtil::Format(R"(
SELECT COALESCE(array_to_string(c.constraint_column_names, ','), '') AS pk_columns
FROM duckdb_tables() t
LEFT JOIN (
    SELECT schema_name, table_name, constraint_column_names
    FROM duckdb_constraints()
    WHERE database_name = current_database() AND constraint_type = 'PRIMARY KEY'
) c ON c.schema_name = t.schema_name AND c.table_name = t.table_name
WHERE t.database_name = current_database()
  AND t.schema_name = %s AND t.table_name = %s
LIMIT 1
)",
		                                               QuoteLiteral(schema_name), QuoteLiteral(table_name)),
		                            "detect PK");
		const std::string pk_columns = (pk_result->RowCount() > 0 && !pk_result->GetValue(0, 0).IsNull())
		                                   ? pk_result->GetValue(0, 0).ToString()
		                                   : "";

		// 5d. Insert object_snapshot with computed hashes and pk_columns.
		const auto def_sql = def_hash_val.empty() ? "NULL" : QuoteLiteral(def_hash_val);
		const auto th_sql = table_hash_val.empty() ? "NULL" : QuoteLiteral(table_hash_val);
		const auto fp_sql = obj_fp_val.empty() ? "NULL" : QuoteLiteral(obj_fp_val);
		const auto pk_sql = pk_columns.empty() ? "NULL" : QuoteLiteral(pk_columns);

		// Derive hypha_object_id: stable UUID per (schema_name, object_name).
		// Reused across every commit so the same logical table always has the same ID.
		// Only a fresh UUID is minted on first sighting (no prior snapshot for this table).
		std::string hypha_object_id;
		{
			const auto oid_res = con.Query(
			    StringUtil::Format("SELECT hypha_object_id FROM hypha.object_snapshot "
			                       "WHERE schema_name = %s AND object_name = %s AND hypha_object_id IS NOT NULL "
			                       "ORDER BY captured_at DESC LIMIT 1",
			                       QuoteLiteral(schema_name), QuoteLiteral(table_name)));
			if (oid_res && !oid_res->HasError() && oid_res->RowCount() > 0 && !oid_res->GetValue(0, 0).IsNull()) {
				hypha_object_id = oid_res->GetValue(0, 0).ToString();
			} else {
				hypha_object_id = Exec(con, "SELECT uuid()::VARCHAR", "gen hypha_object_id")->GetValue(0, 0).ToString();
			}
		}

		Exec(con,
		     StringUtil::Format(R"(
INSERT INTO hypha.object_snapshot
    (commit_id, target_name, schema_name, object_name, object_type,
     definition_hash, content_hash, object_fingerprint, pk_columns, hypha_object_id, pg_table_name)
VALUES (%s, %s, %s, %s, 'table', %s, %s, %s, %s, %s, %s)
)",
		                        QuoteLiteral(commit_id), QuoteLiteral(target_name), QuoteLiteral(schema_name),
		                        QuoteLiteral(table_name), def_sql, th_sql, fp_sql, pk_sql,
		                        QuoteLiteral(hypha_object_id), QuoteLiteral(pg_table_name)),
		     "INSERT hypha.object_snapshot");

		// 5e. Populate hypha.row_hash for every table that hashed successfully.
		// Keyed tables store pk_json (a chr(31)-separated compound key in sorted column order,
		// e.g. "42" or "1\x1falice") for targeted row-level diff. Keyless tables store NULL
		// pk_json and are keyed by row_hash alone, enabling the append-only fast path in sync.
		if (hashes_ok) {
			const bool keyless = pk_columns.empty();

			// Parse and sort PK column names for determinism (empty for keyless tables).
			std::vector<std::string> pk_col_vec;
			if (!keyless) {
				std::string rem = pk_columns;
				while (!rem.empty()) {
					const auto c = rem.find(',');
					pk_col_vec.push_back(c == std::string::npos ? rem : rem.substr(0, c));
					rem = c == std::string::npos ? "" : rem.substr(c + 1);
				}
				std::sort(pk_col_vec.begin(), pk_col_vec.end());
			}

			// Build the pk_json expression: keyed → col1_cast || chr(31) || ...; keyless → NULL.
			std::string pk_json_expr;
			if (keyless) {
				pk_json_expr = "NULL";
			} else {
				pk_json_expr = "CAST(" + QuoteIdent(pk_col_vec[0]) + " AS VARCHAR)";
				for (size_t pki = 1; pki < pk_col_vec.size(); pki++) {
					pk_json_expr += " || chr(31) || CAST(" + QuoteIdent(pk_col_vec[pki]) + " AS VARCHAR)";
				}
			}

			const auto row_hash_expr = RowHashExpr(fp_cols);

			// Detect single-integer PK for keyset pagination; keyless tables and other PK shapes
			// use LIMIT/OFFSET pagination instead.
			std::string rh_int_pk;
			if (!keyless && pk_col_vec.size() == 1) {
				for (const auto &fc : fp_cols) {
					if (fc.first == pk_col_vec[0] && IsIntegerDuckType(fc.second)) {
						rh_int_pk = pk_col_vec[0];
						break;
					}
				}
			}

			// Compute adaptive chunk size from column type widths.
			{
				std::vector<ColumnDef> rh_col_defs;
				rh_col_defs.reserve(fp_cols.size());
				for (const auto &fc : fp_cols) {
					ColumnDef cd;
					cd.name = fc.first;
					cd.duckdb_type = fc.second;
					rh_col_defs.push_back(cd);
				}
				const auto rh_varchar_widths = QueryVarcharWidths(con, schema_name, table_name, rh_col_defs);
				const int64_t rh_chunk_rows = ComputeChunkRows(rh_col_defs, HASH_TARGET_BYTES, rh_varchar_widths);

				// Insert row_hash in chunks to avoid accumulating all rows in DuckDB buffer at once.
				const int64_t rh_chunks = (row_count + rh_chunk_rows - 1) / rh_chunk_rows;
				int64_t rh_last_pk = 0;
				bool rh_have_last_pk = false;
				for (int64_t rh_chunk = 0; rh_chunk < rh_chunks; rh_chunk++) {
					std::string rh_where;
					std::string rh_suffix;
					if (!rh_int_pk.empty()) {
						if (rh_have_last_pk) {
							rh_where = " WHERE " + QuoteIdent(rh_int_pk) + " > " + std::to_string(rh_last_pk);
						}
						rh_suffix = " ORDER BY " + QuoteIdent(rh_int_pk) + " LIMIT " + std::to_string(rh_chunk_rows);
					} else {
						rh_suffix = " LIMIT " + std::to_string(rh_chunk_rows) + " OFFSET " +
						            std::to_string(rh_chunk * rh_chunk_rows);
					}
					Exec(con,
					     StringUtil::Format(R"(
INSERT INTO hypha.row_hash (commit_id, target_name, schema_name, table_name, pk_json, row_hash)
SELECT %s, %s, %s, %s, %s, %s
FROM %s.%s%s%s
)",
					                        QuoteLiteral(commit_id), QuoteLiteral(target_name),
					                        QuoteLiteral(schema_name), QuoteLiteral(table_name), pk_json_expr,
					                        row_hash_expr, QuoteIdent(schema_name), QuoteIdent(table_name), rh_where,
					                        rh_suffix),
					     "INSERT hypha.row_hash chunk");
					if (!rh_int_pk.empty()) {
						// Keyset: query the max PK in this chunk to advance the cursor.
						const auto max_res =
						    Exec(con,
						         "SELECT MAX(t." + QuoteIdent(rh_int_pk) + ") FROM (SELECT " + QuoteIdent(rh_int_pk) +
						             " FROM " + QuoteIdent(schema_name) + "." + QuoteIdent(table_name) +
						             (rh_have_last_pk
						                  ? " WHERE " + QuoteIdent(rh_int_pk) + " > " + std::to_string(rh_last_pk)
						                  : "") +
						             " ORDER BY " + QuoteIdent(rh_int_pk) + " LIMIT " + std::to_string(rh_chunk_rows) +
						             ") t",
						         "row_hash chunk max pk");
						if (max_res->RowCount() == 0 || max_res->GetValue(0, 0).IsNull()) {
							break;
						}
						rh_last_pk = max_res->GetValue(0, 0).GetValue<int64_t>();
						rh_have_last_pk = true;
					} else {
						// LIMIT/OFFSET: row_count bounds the loop; stop early if no rows for this chunk.
						if (rh_chunks > 0 && rh_chunk + 1 >= rh_chunks) {
							break;
						}
					}
				}
			} // end chunk scope
		}

		const auto strategy_sql_val = strategy_name_val.empty() ? "NULL" : QuoteLiteral(strategy_name_val);
		Exec(con,
		     StringUtil::Format(R"(
INSERT INTO hypha.table_snapshot
    (commit_id, target_name, schema_name, table_name, row_count, table_hash, fingerprint_strategy)
VALUES (%s, %s, %s, %s, %s, %s, %s)
)",
		                        QuoteLiteral(commit_id), QuoteLiteral(target_name), QuoteLiteral(schema_name),
		                        QuoteLiteral(table_name), std::to_string(row_count), th_sql, strategy_sql_val),
		     "INSERT hypha.table_snapshot");

		if (strategy_name_val == "EXACT") {
			n_exact++;
		} else if (strategy_name_val == "APPEND_ONLY") {
			n_append_only++;
		} else if (strategy_name_val == "MUTABLE_ENTITY") {
			n_mutable_entity++;
		}
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

	// 8. Print elegant summary to stderr; return empty VARCHAR.
	// U+00B7 middle dot = \xc2\xb7 (UTF-8); U+2014 em dash = \xe2\x80\x94
	const std::string short_id = commit_id.size() >= 8 ? commit_id.substr(0, 8) : commit_id;
	Printer::Print(OutputStream::STREAM_STDERR,
	               StringUtil::Format("[hyphasync] snapshot plan \xc2\xb7 database=%s \xc2\xb7 %lld tables \xc2\xb7 "
	                                  "%lld columns \xc2\xb7 %lld rows",
	                                  current_db.c_str(), (long long)tables_captured, (long long)columns_captured,
	                                  (long long)total_rows));
	Printer::Print(
	    OutputStream::STREAM_STDERR,
	    StringUtil::Format("[hyphasync]   strategies  EXACT: %lld   APPEND_ONLY: %lld   MUTABLE_ENTITY: %lld",
	                       (long long)n_exact, (long long)n_append_only, (long long)n_mutable_entity));
	Printer::Print(OutputStream::STREAM_STDERR,
	               StringUtil::Format("[hyphasync]   fingerprint algo: %s \xc2\xb7 commit: %s", HYPHA_FINGERPRINT_ALGO,
	                                  short_id.c_str()));
	Printer::Print(OutputStream::STREAM_STDERR,
	               "[hyphasync]   no Postgres writes \xe2\x80\x94 run hypha_base_snapshot() to sync");
	return "";
}

} // namespace duckdb
