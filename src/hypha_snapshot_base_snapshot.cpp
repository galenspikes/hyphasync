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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <libpq-fe.h>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

namespace {

struct HyphaSnapshotGlobalState : GlobalTableFunctionState {
	std::string commit_id;
	std::string db_name;
	std::string conn_string;
	int64_t max_rows_per_table = 0;
	PgConn pg;
	PgConn pg_log;
	bool fast_mode = false;
	bool connection_dead = false;

	struct TableEntry {
		std::string duckdb_schema;
		std::string table_name;
		std::string pg_table_name;
		std::string pk_cols;
		int64_t row_count = 0;
	};
	std::vector<TableEntry> tables;
	size_t current_idx = 0;
	size_t total_tables = 0;
	int64_t tables_synced = 0;
	int64_t rows_synced = 0;
	int64_t error_count = 0;
	int64_t skipped_count = 0;
	int64_t tables_failed = 0;
	int64_t rows_failed = 0;
	bool finalized = false;
	std::mutex lock;

	// Memory limit: con + guard must be destroyed in reverse order (guard first).
	unique_ptr<Connection> mem_con;
	unique_ptr<MemoryLimitGuard> mem_guard;

	// Force single-threaded execution: all PQexec/PQputCopyData calls share one PGconn*.
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HyphaSnapshotBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	(void)context;
	(void)input;
	names.push_back("table_name");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("row_count");
	return_types.push_back(LogicalType::BIGINT);
	names.push_back("fingerprint_strategy");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("duration_ms");
	return_types.push_back(LogicalType::DOUBLE);
	names.push_back("status");
	return_types.push_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> HyphaSnapshotInitGlobal(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	(void)input;
	Connection con(*context.db);

	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException("hypha metadata is not initialized — call hypha_init() first.");
	}

	auto state = make_uniq<HyphaSnapshotGlobalState>();

	state->conn_string = GetDefaultTargetConnString(con);
	if (state->conn_string.empty()) {
		throw InvalidInputException(
		    "hypha_base_snapshot: no Postgres connection string found in hypha.target — call hypha_init() first.");
	}

	state->max_rows_per_table = GetMaxRowsPerTable(con);
	state->fast_mode = GetFastMode(con);
	if (state->max_rows_per_table > 0) {
		LogEvent(con, "info", "base_snapshot", "SKIP_GUARD",
		         "max_rows_per_table guard active: tables with row_count > " +
		             std::to_string(state->max_rows_per_table) + " will be skipped",
		         "{\"max_rows_per_table\":" + std::to_string(state->max_rows_per_table) + "}");
	}

	// Apply memory limit before catalog walk and schema staging.
	state->mem_con = make_uniq<Connection>(*context.db);
	{
		const idx_t mem_limit = ComputeSyncMemoryLimit(*state->mem_con, 0);
		state->mem_guard = make_uniq<MemoryLimitGuard>(*state->mem_con, mem_limit);
	}

	// Catalog walk (base snapshot mode → fast O(1) fingerprinting for all tables).
	RunBaseSnapshotPlan(con, true);

	const auto commit_res = Exec(con, R"(
SELECT commit_id FROM hypha.commit
WHERE kind = 'base_snapshot_plan' AND status = 'completed'
ORDER BY created_at DESC LIMIT 1)",
	                             "get latest plan commit");
	if (commit_res->RowCount() == 0) {
		throw IOException("hypha_base_snapshot: no completed snapshot plan found after running plan");
	}
	state->commit_id = commit_res->GetValue(0, 0).ToString();
	state->db_name = GetCurrentDatabase(con);

	// Connect to Postgres.
	state->pg.reset(OpenHyphaConnection(state->conn_string, state->fast_mode));

	// Dedicated auto-commit log connection (best-effort).
	try {
		state->pg_log.reset(OpenHyphaConnection(state->conn_string, state->fast_mode));
		EnsureRemoteEventLog(state->pg_log);
	} catch (...) {
		state->pg_log.reset();
	}

	// Create all target Postgres schemas in one transaction.
	PGExec(state->pg, "BEGIN", "BEGIN schema preamble");
	const auto schemas_res = Exec(con,
	                              StringUtil::Format("SELECT DISTINCT schema_name FROM hypha.object_snapshot "
	                                                 "WHERE commit_id = %s ORDER BY schema_name",
	                                                 QuoteLiteral(state->commit_id)),
	                              "get snapshot schemas");
	for (idx_t s = 0; s < schemas_res->RowCount(); s++) {
		const auto duckdb_schema = schemas_res->GetValue(0, s).ToString();
		const auto pg_schema = MakePostgresSchemaName(state->db_name, duckdb_schema);
		PGExec(state->pg, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdent(pg_schema), "CREATE SCHEMA");
		StampHyphaSchemaComment(state->pg, pg_schema, state->db_name, duckdb_schema, state->commit_id);
	}
	PGExec(state->pg, "COMMIT", "COMMIT schema preamble");

	// Build the ordered table list.
	const auto tables_res =
	    Exec(con,
	         StringUtil::Format("SELECT os.schema_name, os.object_name, COALESCE(os.pk_columns, ''), "
	                            "CAST(COALESCE(ts.row_count, 0) AS BIGINT), "
	                            "COALESCE(os.pg_table_name, substr(os.object_name, 1, 63)) "
	                            "FROM hypha.object_snapshot os "
	                            "LEFT JOIN hypha.table_snapshot ts "
	                            "    ON ts.commit_id = os.commit_id AND ts.schema_name = os.schema_name "
	                            "    AND ts.table_name = os.object_name "
	                            "WHERE os.commit_id = %s AND os.object_type = 'table' "
	                            "ORDER BY os.schema_name, os.object_name",
	                            QuoteLiteral(state->commit_id)),
	         "get snapshot tables");

	for (idx_t t = 0; t < tables_res->RowCount(); t++) {
		HyphaSnapshotGlobalState::TableEntry entry;
		entry.duckdb_schema = tables_res->GetValue(0, t).ToString();
		entry.table_name = tables_res->GetValue(1, t).ToString();
		entry.pk_cols = tables_res->GetValue(2, t).ToString();
		entry.row_count = tables_res->GetValue(3, t).GetValue<int64_t>();
		entry.pg_table_name = tables_res->GetValue(4, t).ToString();
		state->tables.push_back(entry);
	}
	state->total_tables = state->tables.size();

	LogAll(con, state->pg_log, state->db_name, "info", "base_snapshot", "START",
	       "base snapshot started: pushing " + std::to_string(state->tables.size()) + " tables to Postgres",
	       "{\"commit_id\":\"" + state->commit_id + "\",\"tables_pending\":" + std::to_string(state->tables.size()) +
	           "}");

	Printer::Print(OutputStream::STREAM_STDERR,
	               StringUtil::Format("[hyphasync] base_snapshot: copying %lld tables to Postgres",
	                                  (long long)state->tables.size()));
	return std::move(state);
}

//! Emit one row for a skipped or unsupported table without COPY.
static void EmitRow(DataChunk &output, const std::string &table_label, int64_t row_count, const std::string &strategy,
                    double duration_ms, const std::string &status) {
	FlatVector::GetData<string_t>(output.data[0])[0] = StringVector::AddString(output.data[0], table_label);
	FlatVector::GetData<int64_t>(output.data[1])[0] = row_count;
	FlatVector::GetData<string_t>(output.data[2])[0] = StringVector::AddString(output.data[2], strategy);
	FlatVector::GetData<double>(output.data[3])[0] = duration_ms;
	FlatVector::GetData<string_t>(output.data[4])[0] = StringVector::AddString(output.data[4], status);
	output.SetCardinality(1);
}

static void HyphaSnapshotFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = *static_cast<HyphaSnapshotGlobalState *>(data_p.global_state.get());
	Connection con(*context.db);

	// Atomically claim the next table slot.
	size_t table_idx;
	{
		std::lock_guard<std::mutex> guard(state.lock);
		if (state.current_idx >= state.tables.size()) {
			// All tables processed — finalize and signal EOF.
			if (!state.finalized) {
				state.finalized = true;
				// Only write remote metadata when all tables succeeded (no partial state).
				if (state.error_count == 0) {
					try {
						WriteRemoteHyphaMetadata(con, state.pg, state.commit_id, state.db_name, "base_snapshot",
						                         state.commit_id, "", HYPHA_FINGERPRINT_ALGO, state.tables_synced,
						                         state.rows_synced, state.tables_failed, state.rows_failed);
					} catch (const std::exception &meta_ex) {
						LogAll(con, state.pg_log, state.db_name, "warn", "base_snapshot", "REMOTE_META_FAIL",
						       std::string("remote hypha metadata write failed: ") + meta_ex.what());
					}
				}
				const std::string final_status = (state.error_count > 0) ? "partial" : "applied";
				const std::string log_code = (state.error_count > 0) ? "PARTIAL" : "OK";
				const std::string log_msg = "tables_synced=" + std::to_string(state.tables_synced) +
				                            " tables_skipped=" + std::to_string(state.skipped_count) +
				                            " tables_failed=" + std::to_string(state.tables_failed) +
				                            " rows_synced=" + std::to_string(state.rows_synced) +
				                            " rows_failed=" + std::to_string(state.rows_failed);
				LogAll(con, state.pg_log, state.db_name, "info", "base_snapshot", log_code, log_msg,
				       "{\"commit_id\":\"" + state.commit_id +
				           "\",\"tables_synced\":" + std::to_string(state.tables_synced) +
				           ",\"tables_skipped\":" + std::to_string(state.skipped_count) +
				           ",\"tables_failed\":" + std::to_string(state.tables_failed) +
				           ",\"rows_synced\":" + std::to_string(state.rows_synced) +
				           ",\"rows_failed\":" + std::to_string(state.rows_failed) + "}");
				state.pg_log.reset();
				if (state.tables_failed > 0) {
					Printer::Print(OutputStream::STREAM_STDERR,
					               "[hyphasync] WARNING: " + std::to_string(state.tables_failed) + " tables failed (" +
					                   std::to_string(state.rows_failed) + " rows lost)");
				}
				try {
					Exec(con,
					     StringUtil::Format("UPDATE hypha.commit SET status = '%s', applied_at = now() "
					                        "WHERE commit_id = %s",
					                        final_status, QuoteLiteral(state.commit_id)),
					     "mark commit finalized");
				} catch (...) {
				}
				state.pg.reset();
			}
			output.SetCardinality(0);
			return;
		}
		table_idx = state.current_idx++;
	}

	const auto &entry = state.tables[table_idx];
	const auto pg_schema = MakePostgresSchemaName(state.db_name, entry.duckdb_schema);
	const std::string table_label = entry.duckdb_schema + "." + entry.table_name;
	const auto table_t0 = std::chrono::steady_clock::now();

	// Short-circuit: emit an error row if the connection was permanently lost.
	if (state.connection_dead) {
		{
			std::lock_guard<std::mutex> guard(state.lock);
			state.error_count++;
		}
		EmitRow(output, table_label, 0, "", 0.0, "error: connection lost");
		return;
	}

	// Look up fingerprint strategy from the plan (already computed by RunBaseSnapshotPlan).
	std::string strategy_name;
	{
		const auto ts_res = con.Query(StringUtil::Format(
		    "SELECT COALESCE(fingerprint_strategy, 'UNKNOWN') FROM hypha.table_snapshot "
		    "WHERE commit_id = %s AND schema_name = %s AND table_name = %s LIMIT 1",
		    QuoteLiteral(state.commit_id), QuoteLiteral(entry.duckdb_schema), QuoteLiteral(entry.table_name)));
		if (ts_res && !ts_res->HasError() && ts_res->RowCount() > 0 && !ts_res->GetValue(0, 0).IsNull()) {
			strategy_name = ts_res->GetValue(0, 0).ToString();
		}
	}

	// Skip guard: omit tables whose row count exceeds max_rows_per_table.
	if (state.max_rows_per_table > 0 && entry.row_count > state.max_rows_per_table) {
		const std::string skipped_status = "skipped: row_count=" + std::to_string(entry.row_count) +
		                                   " > max_rows_per_table=" + std::to_string(state.max_rows_per_table);
		LogAll(con, state.pg_log, state.db_name, "info", "base_snapshot", "TABLE_SKIP",
		       "table \"" + entry.duckdb_schema + "\".\"" + entry.table_name + "\" " + skipped_status);
		{
			std::lock_guard<std::mutex> guard(state.lock);
			state.skipped_count++;
		}
		EmitRow(output, table_label, entry.row_count, strategy_name, 0.0, skipped_status);
		return;
	}

	// Build column list from hypha.column_snapshot.
	const auto cols_result = Exec(con,
	                              StringUtil::Format("SELECT column_name, postgres_type, is_nullable, duckdb_type "
	                                                 "FROM hypha.column_snapshot "
	                                                 "WHERE commit_id = %s AND schema_name = %s AND table_name = %s "
	                                                 "ORDER BY ordinal_position",
	                                                 QuoteLiteral(state.commit_id), QuoteLiteral(entry.duckdb_schema),
	                                                 QuoteLiteral(entry.table_name)),
	                              "get columns");

	std::vector<ColumnDef> cols;
	for (idx_t c = 0; c < cols_result->RowCount(); c++) {
		const auto pg_type = cols_result->GetValue(1, c).ToString();
		if (StringUtil::StartsWith(pg_type, "(unsupported")) {
			continue;
		}
		ColumnDef cd;
		cd.name = cols_result->GetValue(0, c).ToString();
		cd.postgres_type = pg_type;
		cd.is_nullable = cols_result->GetValue(2, c).GetValue<bool>();
		cd.duckdb_type = cols_result->GetValue(3, c).ToString();
		cd.needs_json_cast = NeedsJsonCastForCopy(cd.duckdb_type);
		cols.push_back(cd);
	}

	if (cols.empty()) {
		{
			std::lock_guard<std::mutex> guard(state.lock);
			state.skipped_count++;
		}
		EmitRow(output, table_label, entry.row_count, strategy_name, 0.0, "skipped: no supported columns");
		return;
	}

	int64_t rows_copied = 0;
	std::string status = "ok";

	{
		const std::string tbl_label = entry.duckdb_schema + "." + entry.table_name;
		const std::string tbl_padded =
		    tbl_label.size() < 40 ? tbl_label + std::string(40 - tbl_label.size(), ' ') : tbl_label;
		Printer::Print(OutputStream::STREAM_STDERR,
		               "[hyphasync] \xe2\x86\x92 " + tbl_padded + "  (" + std::to_string(entry.row_count) + " rows)");
	}

	try {
		const auto pg_names = ResolveColumnNames(cols);

		PGExec(state.pg, "BEGIN", "BEGIN table");
		PGExec(state.pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(entry.pg_table_name),
		       "DROP TABLE IF EXISTS");
		PGExec(state.pg, BuildCreateTableDDL(pg_schema, entry.pg_table_name, cols, pg_names, true), "CREATE TABLE");

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
			PGExec(state.pg,
			       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(entry.pg_table_name) + " " + alter_cols,
			       "SET STORAGE EXTERNAL");
		}
		{
			const int64_t fw_bytes = EstimateFixedWidthRowBytes(cols);
			if (fw_bytes > 7000) {
				const std::string tbl_qn = entry.duckdb_schema + "." + entry.table_name;
				LogAll(con, state.pg_log, state.db_name, "warn", "base_snapshot", "TABLE_TOO_WIDE",
				       "TABLE_TOO_WIDE: table '" + tbl_qn + "' has an estimated fixed-width row size of ~" +
				           std::to_string(fw_bytes) +
				           " bytes, which exceeds Postgres's 8 KB heap row limit even after TOAST-ing varlena "
				           "columns. Consider splitting the table or converting fixed-width columns to "
				           "text/numeric/jsonb.");
			}
		}

		const auto oid = GetHyphaObjectId(con, entry.duckdb_schema, entry.table_name, state.commit_id);
		if (!oid.empty()) {
			StampHyphaTableComment(state.pg, pg_schema, entry.pg_table_name, oid, state.db_name, entry.duckdb_schema,
			                       entry.table_name, state.commit_id);
		}

		// Detect integer PK for keyset pagination.
		std::string cp_int_pk;
		if (entry.pk_cols.find(',') == std::string::npos && !entry.pk_cols.empty()) {
			for (const auto &cd : cols) {
				if (cd.name == entry.pk_cols && IsIntegerDuckType(cd.duckdb_type)) {
					cp_int_pk = entry.pk_cols;
					break;
				}
			}
		}

		const auto cp_varchar_widths = QueryVarcharWidths(con, entry.duckdb_schema, entry.table_name, cols);
		const int64_t cp_chunk_rows = ComputeChunkRows(cols, COPY_TARGET_BYTES, cp_varchar_widths);

		const std::string pg_copy_sql =
		    "COPY " + QuoteIdent(pg_schema) + "." + QuoteIdent(entry.pg_table_name) + " FROM STDIN CSV NULL '\\N'";
		PGresult *copy_res = PQexec(state.pg, pg_copy_sql.c_str());
		if (!copy_res) {
			throw IOException("PQexec returned null starting COPY (out of memory): " +
			                  TrimPQ(PQerrorMessage(state.pg)));
		}
		if (PQresultStatus(copy_res) != PGRES_COPY_IN) {
			const auto err = TrimPQ(PQresultErrorMessage(copy_res));
			PQclear(copy_res);
			throw IOException(err);
		}
		PQclear(copy_res);

		int64_t cp_offset = 0;
		int64_t cp_last_pk = 0;
		bool cp_have_last_pk = false;

		while (true) {
			std::string cp_where;
			std::string cp_suffix;
			if (!cp_int_pk.empty()) {
				if (cp_have_last_pk) {
					cp_where = QuoteIdent(cp_int_pk) + " > " + std::to_string(cp_last_pk);
				}
				cp_suffix = " ORDER BY " + QuoteIdent(cp_int_pk) + " LIMIT " + std::to_string(cp_chunk_rows);
			} else {
				cp_suffix = " LIMIT " + std::to_string(cp_chunk_rows) + " OFFSET " + std::to_string(cp_offset);
			}

			const std::string cp_select =
			    BuildCopySelectList(entry.duckdb_schema, entry.table_name, cols, cp_where) + cp_suffix;
			if (CopyChunkViaPipe(con, state.pg, cp_select, ("COPY chunk: " + entry.table_name).c_str()) == 0) {
				break;
			}

			if (!cp_int_pk.empty()) {
				const auto max_res =
				    Exec(con,
				         "SELECT MAX(t." + QuoteIdent(cp_int_pk) + ") FROM (SELECT " + QuoteIdent(cp_int_pk) +
				             " FROM " + QuoteIdent(entry.duckdb_schema) + "." + QuoteIdent(entry.table_name) +
				             (cp_where.empty() ? "" : " WHERE " + cp_where) + " ORDER BY " + QuoteIdent(cp_int_pk) +
				             " LIMIT " + std::to_string(cp_chunk_rows) + ") t",
				         "copy chunk max pk");
				if (max_res->RowCount() == 0 || max_res->GetValue(0, 0).IsNull()) {
					break;
				}
				cp_last_pk = max_res->GetValue(0, 0).GetValue<int64_t>();
				cp_have_last_pk = true;
			} else {
				cp_offset += cp_chunk_rows;
			}
		}

		if (PQputCopyEnd(state.pg, nullptr) != 1) {
			throw IOException("PQputCopyEnd failed: " + TrimPQ(PQerrorMessage(state.pg)));
		}
		PGresult *copy_done = PQgetResult(state.pg);
		if (!copy_done) {
			throw IOException("PQgetResult returned null after COPY (out of memory): " +
			                  TrimPQ(PQerrorMessage(state.pg)));
		}
		if (PQresultStatus(copy_done) != PGRES_COMMAND_OK) {
			const auto err = TrimPQ(PQresultErrorMessage(copy_done));
			PQclear(copy_done);
			throw IOException(err);
		}
		const char *tag = PQcmdTuples(copy_done);
		if (tag && *tag != '\0') {
			try {
				rows_copied = std::stoll(tag);
			} catch (...) {
			}
		}
		PQclear(copy_done);

		PGExec(state.pg, "COMMIT", "COMMIT table");

		{
			std::lock_guard<std::mutex> guard(state.lock);
			state.tables_synced++;
			state.rows_synced += rows_copied;
		}

		const auto table_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - table_t0).count();
		{
			const std::string tbl_label = entry.duckdb_schema + "." + entry.table_name;
			const std::string tbl_padded =
			    tbl_label.size() < 40 ? tbl_label + std::string(40 - tbl_label.size(), ' ') : tbl_label;
			Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] \xe2\x9c\x93 " + tbl_padded + "  " +
			                                                std::to_string(rows_copied) + " rows  " +
			                                                std::to_string(table_ms) + "ms");
		}
		LogAll(con, state.pg_log, state.db_name, "info", "base_snapshot", "COPY_DONE",
		       entry.duckdb_schema + "." + entry.table_name + ": " + std::to_string(rows_copied) + " rows copied",
		       "{\"schema\":\"" + entry.duckdb_schema + "\",\"table\":\"" + entry.table_name +
		           "\",\"rows_copied\":" + std::to_string(rows_copied) + "}");

	} catch (const std::exception &ex) {
		PGresult *rb = PQexec(state.pg, "ROLLBACK");
		if (rb) {
			PQclear(rb);
		}
		// Check whether the connection survived the failure.
		if (PQstatus(state.pg) == CONNECTION_BAD) {
			PQreset(state.pg.conn);
			if (PQstatus(state.pg) != CONNECTION_OK) {
				state.connection_dead = true;
				Printer::Print(OutputStream::STREAM_STDERR,
				               "[hyphasync] connection lost and could not be re-established; "
				               "remaining tables will be skipped");
			} else {
				// Re-apply fast_mode session setting on the restored connection.
				if (state.fast_mode) {
					PQclear(PQexec(state.pg, "SET synchronous_commit = off"));
				}
			}
		}
		status = std::string("error: ") + ex.what();
		Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] \xe2\x9c\x97 " + entry.duckdb_schema + "." +
		                                                entry.table_name + "  FAILED: " + ex.what());
		if (std::string(ex.what()).find("row is too big") != std::string::npos) {
			Printer::Print(OutputStream::STREAM_STDERR,
			               "[hyphasync] TABLE_TOO_WIDE: table '" + entry.duckdb_schema + "." + entry.table_name +
			                   "' row is too wide for Postgres heap. Consider splitting or converting "
			                   "fixed-width columns to text/jsonb.");
		}
		LogAll(con, state.pg_log, state.db_name, "warn", "base_snapshot", "TABLE_FAIL",
		       "failed " + entry.duckdb_schema + "." + entry.table_name + ": " + ex.what());
	}

	const auto table_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - table_t0).count();
	if (StringUtil::StartsWith(status, "error:")) {
		std::lock_guard<std::mutex> guard(state.lock);
		state.error_count++;
		state.tables_failed++;
		state.rows_failed += entry.row_count;
	}
	EmitRow(output, table_label, rows_copied, strategy_name, static_cast<double>(table_ms), status);
}

static double HyphaSnapshotProgress(ClientContext &context, const FunctionData *bind_data,
                                    const GlobalTableFunctionState *global_state) {
	(void)context;
	(void)bind_data;
	if (!global_state) {
		return 0.0;
	}
	const auto &state = *static_cast<const HyphaSnapshotGlobalState *>(global_state);
	if (state.total_tables == 0) {
		return 100.0;
	}
	return static_cast<double>(state.current_idx) / static_cast<double>(state.total_tables) * 100.0;
}

} // anonymous namespace

void RegisterBaseSnapshotTableFunction(ExtensionLoader &loader) {
	TableFunction fn("hypha_base_snapshot", {}, HyphaSnapshotFunction, HyphaSnapshotBind, HyphaSnapshotInitGlobal);
	fn.table_scan_progress = HyphaSnapshotProgress;
	loader.RegisterFunction(fn);
}

} // namespace duckdb
