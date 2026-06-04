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

struct HyphaSyncGlobalState : GlobalTableFunctionState {
	std::vector<SyncTableResult> results;
	size_t current_idx = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HyphaSyncBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	(void)context;
	(void)input;
	names.push_back("table_name");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("action");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("rows_synced");
	return_types.push_back(LogicalType::BIGINT);
	names.push_back("duration_ms");
	return_types.push_back(LogicalType::DOUBLE);
	names.push_back("status");
	return_types.push_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> HyphaSyncInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	(void)input;
	Connection con(*context.db);
	auto state = make_uniq<HyphaSyncGlobalState>();
	state->results = RunSync(con);
	return std::move(state);
}

static void HyphaSyncFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	(void)context;
	auto &state = *static_cast<HyphaSyncGlobalState *>(data_p.global_state.get());
	if (state.current_idx >= state.results.size()) {
		output.SetCardinality(0);
		return;
	}
	const auto &r = state.results[state.current_idx++];
	FlatVector::GetData<string_t>(output.data[0])[0] = StringVector::AddString(output.data[0], r.table_name);
	FlatVector::GetData<string_t>(output.data[1])[0] = StringVector::AddString(output.data[1], r.action);
	FlatVector::GetData<int64_t>(output.data[2])[0] = r.rows_synced;
	FlatVector::GetData<double>(output.data[3])[0] = r.duration_ms;
	FlatVector::GetData<string_t>(output.data[4])[0] = StringVector::AddString(output.data[4], r.status);
	output.SetCardinality(1);
}

} // anonymous namespace

void RegisterSyncTableFunction(ExtensionLoader &loader) {
	TableFunction fn("hypha_sync", {}, HyphaSyncFunction, HyphaSyncBind, HyphaSyncInitGlobal);
	loader.RegisterFunction(fn);
}

std::string RunHyphaDrop(Connection &con, bool drop_meta) {
	const std::string conn_string = GetDefaultTargetConnString(con);
	if (conn_string.empty()) {
		throw InvalidInputException("hypha_drop(): no stored target — call hypha_init() first");
	}

	PgConn pg(OpenHyphaConnection(conn_string, false));

	// Collect non-system schemas. The hypha meta schema is handled separately via drop_meta.
	static const char *SCHEMA_SQL = R"(
SELECT schema_name FROM information_schema.schemata
WHERE schema_name NOT IN ('pg_catalog', 'information_schema', 'public', 'pg_toast', 'pg_temp_1', 'pg_internal')
  AND schema_name NOT LIKE 'pg_%'
ORDER BY schema_name)";

	PGresult *res = PQexec(pg, SCHEMA_SQL);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
		const std::string err = res ? TrimPQ(PQresultErrorMessage(res)) : TrimPQ(PQerrorMessage(pg));
		if (res) {
			PQclear(res);
		}
		throw IOException("hypha_drop(): failed to list schemas: " + err);
	}

	std::vector<std::string> to_drop;
	const int nrows = PQntuples(res);
	for (int i = 0; i < nrows; i++) {
		const std::string name = PQgetvalue(res, i, 0);
		if (name == "hypha") {
			if (drop_meta) {
				to_drop.push_back(name);
			}
		} else {
			to_drop.push_back(name);
		}
	}
	PQclear(res);

	// Log the list to stderr before touching anything.
	std::string schema_list;
	for (size_t i = 0; i < to_drop.size(); i++) {
		if (i > 0) {
			schema_list += ", ";
		}
		schema_list += to_drop[i];
	}
	Printer::Print(OutputStream::STREAM_STDERR,
	               StringUtil::Format("[hyphasync] hypha_drop(): dropping %lld schemas: %s", (long long)to_drop.size(),
	                                  schema_list));

	for (const auto &schema : to_drop) {
		const std::string drop_sql = "DROP SCHEMA IF EXISTS " + QuoteIdent(schema) + " CASCADE";
		PGresult *drop_res = PQexec(pg, drop_sql.c_str());
		if (!drop_res || PQresultStatus(drop_res) != PGRES_COMMAND_OK) {
			const std::string err = drop_res ? TrimPQ(PQresultErrorMessage(drop_res)) : TrimPQ(PQerrorMessage(pg));
			if (drop_res) {
				PQclear(drop_res);
			}
			throw IOException("hypha_drop(): failed to drop schema \"" + schema + "\": " + err);
		}
		PQclear(drop_res);
	}

	return "dropped " + std::to_string(to_drop.size()) + " schemas from " + RedactConnString(conn_string);
}

std::string RunHyphaStatus(Connection &con) {
	// U+00B7 middle dot = \xc2\xb7 (UTF-8); U+2014 em dash = \xe2\x80\x94
	static const char *NO_HISTORY_LINE =
	    "[hyphasync] status \xc2\xb7 no sync history \xe2\x80\x94 run hypha_init() then hypha_base_snapshot() first";

	// Check if the hypha schema exists at all.
	auto schema_res = con.Query("SELECT count(*) FROM information_schema.schemata WHERE schema_name = 'hypha'");
	if (schema_res->HasError() || schema_res->RowCount() == 0 || schema_res->GetValue(0, 0).GetValue<int64_t>() == 0) {
		Printer::Print(OutputStream::STREAM_STDERR, NO_HISTORY_LINE);
		return "";
	}

	// Check if hypha.commit table exists.
	auto table_res = con.Query("SELECT count(*) FROM information_schema.tables "
	                           "WHERE table_schema = 'hypha' AND table_name = 'commit'");
	if (table_res->HasError() || table_res->RowCount() == 0 || table_res->GetValue(0, 0).GetValue<int64_t>() == 0) {
		Printer::Print(OutputStream::STREAM_STDERR, NO_HISTORY_LINE);
		return "";
	}

	// Query the most recent applied commit (base snapshot or incremental sync).
	// Format timestamp as 'YYYY-MM-DD HH:MM UTC' for readability.
	auto commit_res = con.Query(R"(
SELECT commit_id, kind,
       strftime(COALESCE(applied_at, created_at), '%Y-%m-%d %H:%M UTC') AS ts,
       message
FROM hypha.commit
WHERE kind IN ('base_snapshot_plan', 'sync')
  AND status IN ('applied', 'partial')
ORDER BY COALESCE(applied_at, created_at) DESC
LIMIT 1)");
	if (commit_res->HasError() || commit_res->RowCount() == 0) {
		Printer::Print(OutputStream::STREAM_STDERR, NO_HISTORY_LINE);
		return "";
	}

	const std::string commit_id = commit_res->GetValue(0, 0).ToString();
	const std::string kind = commit_res->GetValue(1, 0).ToString();
	const std::string ts = commit_res->GetValue(2, 0).IsNull() ? "" : commit_res->GetValue(2, 0).ToString();

	// Count tables in hypha.object_snapshot for this commit (0 if table absent).
	int64_t table_count = 0;
	{
		auto snap_check = con.Query("SELECT count(*) FROM information_schema.tables "
		                            "WHERE table_schema = 'hypha' AND table_name = 'object_snapshot'");
		if (!snap_check->HasError() && snap_check->RowCount() > 0 &&
		    snap_check->GetValue(0, 0).GetValue<int64_t>() > 0) {
			const auto cnt_sql = StringUtil::Format("SELECT count(*) FROM hypha.object_snapshot WHERE commit_id = %s",
			                                        QuoteLiteral(commit_id));
			auto cnt_res = con.Query(cnt_sql);
			if (!cnt_res->HasError() && cnt_res->RowCount() > 0) {
				table_count = cnt_res->GetValue(0, 0).GetValue<int64_t>();
			}
		}
	}

	const std::string short_id = commit_id.size() >= 8 ? commit_id.substr(0, 8) : commit_id;
	Printer::Print(
	    OutputStream::STREAM_STDERR,
	    StringUtil::Format("[hyphasync] status \xc2\xb7 last sync: %s \xc2\xb7 kind: %s \xc2\xb7 %lld tables "
	                       "\xc2\xb7 commit: %s",
	                       ts.c_str(), kind.c_str(), (long long)table_count, short_id.c_str()));
	return "";
}

std::string RunHyphaVerify(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		const std::string msg = "verify: hypha metadata not initialized \xe2\x80\x94 run hypha_init() first";
		Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] " + msg);
		return msg;
	}

	// Ensure the tripwire baseline table exists (covers databases initialized before
	// hypha.verify_state was introduced).
	con.Query("CREATE TABLE IF NOT EXISTS hypha.verify_state ("
	          "schema_name VARCHAR, object_name VARCHAR, verify_hash VARCHAR, "
	          "verified_at TIMESTAMP DEFAULT current_timestamp, PRIMARY KEY (schema_name, object_name))");

	// JSON extension enables exact hashing of LIST/STRUCT/MAP columns (best-effort, like the plan).
	con.Query("INSTALL json");
	con.Query("LOAD json");

	// Load the stored fast-strategy table_hash baseline from the most recent snapshot so a detected
	// change can be classified: would hypha_sync()'s fast fingerprint catch it (PENDING) or miss it
	// (BLIND_SPOT_DRIFT, i.e. Postgres is silently stale)?
	std::unordered_map<std::string, std::string> stored_fast; // schema\x1ftable -> table_hash
	{
		auto snap = con.Query(R"(
SELECT schema_name, table_name, table_hash
FROM hypha.table_snapshot
WHERE commit_id = (SELECT commit_id FROM hypha.table_snapshot ORDER BY captured_at DESC LIMIT 1)
)");
		if (snap && !snap->HasError()) {
			for (idx_t i = 0; i < snap->RowCount(); i++) {
				if (snap->GetValue(2, i).IsNull()) {
					continue;
				}
				const auto key = snap->GetValue(0, i).ToString() + "\x1f" + snap->GetValue(1, i).ToString();
				stored_fast[key] = snap->GetValue(2, i).ToString();
			}
		}
	}

	// Enumerate user tables (same scope as the snapshot plan).
	auto tables = con.Query(R"(
SELECT schema_name, table_name
FROM duckdb_tables()
WHERE database_name = current_database()
  AND internal = false AND temporary = false
  AND schema_name NOT IN ('hypha', 'information_schema')
ORDER BY schema_name, table_name
)");
	if (!tables || tables->HasError()) {
		throw IOException("hypha_verify(): failed to enumerate tables");
	}

	int64_t n_checked = 0, n_drift = 0, n_pending = 0, n_armed = 0, n_ok = 0, n_skipped = 0;

	for (idx_t t = 0; t < tables->RowCount(); t++) {
		const auto schema_name = tables->GetValue(0, t).ToString();
		const auto table_name = tables->GetValue(1, t).ToString();
		const std::string label = schema_name + "." + table_name;

		// Gather columns for the exact hash.
		auto cols_res = con.Query(StringUtil::Format(R"(
SELECT column_name, data_type
FROM duckdb_columns()
WHERE database_name = current_database() AND schema_name = %s AND table_name = %s
ORDER BY column_index
)",
		                                             QuoteLiteral(schema_name), QuoteLiteral(table_name)));
		if (!cols_res || cols_res->HasError() || cols_res->RowCount() == 0) {
			n_skipped++;
			continue;
		}
		std::vector<std::pair<std::string, std::string>> cols;
		for (idx_t c = 0; c < cols_res->RowCount(); c++) {
			cols.emplace_back(cols_res->GetValue(0, c).ToString(), cols_res->GetValue(1, c).ToString());
		}

		// Compute the exact (blind-spot-free) full per-row hash now.
		std::string exact_now;
		try {
			auto exact_res = con.Query(BuildExactTableHashSQL(schema_name, table_name, cols));
			if (!exact_res || exact_res->HasError()) {
				throw IOException(exact_res ? exact_res->GetError() : std::string("exact hash query failed"));
			}
			if (exact_res->RowCount() > 0 && !exact_res->GetValue(0, 0).IsNull()) {
				exact_now = exact_res->GetValue(0, 0).ToString();
			}
		} catch (const std::exception &e) {
			LogEvent(con, "warn", "verify", "VERIFY_SKIP",
			         label + ": cannot compute exact hash (unsupported column type?): " + std::string(e.what()), "");
			n_skipped++;
			continue;
		}
		n_checked++;

		// Read the prior tripwire baseline for this table.
		std::string prior;
		{
			auto pr = con.Query(StringUtil::Format(
			    "SELECT verify_hash FROM hypha.verify_state WHERE schema_name = %s AND object_name = %s",
			    QuoteLiteral(schema_name), QuoteLiteral(table_name)));
			if (pr && !pr->HasError() && pr->RowCount() > 0 && !pr->GetValue(0, 0).IsNull()) {
				prior = pr->GetValue(0, 0).ToString();
			}
		}

		if (prior.empty()) {
			n_armed++;
			Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync]   armed    " + label);
		} else if (exact_now != prior) {
			// Changed since last verify. Determine whether hypha_sync()'s fast fingerprint would
			// notice: compare the current fast hash against the stored snapshot fast hash.
			std::string cur_fast;
			auto strat = ClassifyTable(con, schema_name, table_name, cols, false, false);
			auto fr = con.Query(strat.sql);
			if (fr && !fr->HasError() && fr->RowCount() > 0 && !fr->GetValue(0, 0).IsNull()) {
				cur_fast = fr->GetValue(0, 0).ToString();
			}
			const auto it = stored_fast.find(schema_name + "\x1f" + table_name);
			const bool sync_blind = (it != stored_fast.end() && !cur_fast.empty() && cur_fast == it->second);
			if (sync_blind) {
				n_drift++;
				LogEvent(con, "warn", "verify", "VERIFY_DRIFT",
				         label + ": in-place change NOT detectable by the '" + strat.name +
				             "' fast fingerprint — Postgres is silently stale",
				         "{\"schema\":\"" + schema_name + "\",\"table\":\"" + table_name + "\",\"strategy\":\"" +
				             strat.name + "\"}");
				Printer::Print(OutputStream::STREAM_STDERR,
				               "[hyphasync]   DRIFT    " + label +
				                   "  (blind spot — run hypha_base_snapshot() to reconcile)");
			} else {
				n_pending++;
				LogEvent(con, "info", "verify", "VERIFY_PENDING",
				         label + ": changed since last verify; hypha_sync() will catch it", "");
				Printer::Print(OutputStream::STREAM_STDERR,
				               "[hyphasync]   pending  " + label + "  (run hypha_sync() to apply)");
			}
		} else {
			n_ok++;
		}

		// Advance the tripwire baseline to the current exact hash so the next run measures
		// change-since-now (and a reported drift is not re-reported every run).
		con.Query(StringUtil::Format(
		    "INSERT INTO hypha.verify_state (schema_name, object_name, verify_hash, verified_at) "
		    "VALUES (%s, %s, %s, now()) "
		    "ON CONFLICT (schema_name, object_name) DO UPDATE SET verify_hash = excluded.verify_hash, "
		    "verified_at = excluded.verified_at",
		    QuoteLiteral(schema_name), QuoteLiteral(table_name), QuoteLiteral(exact_now)));
	}

	const std::string summary = StringUtil::Format(
	    "verify: %lld checked \xc2\xb7 %lld drift \xc2\xb7 %lld pending \xc2\xb7 %lld armed \xc2\xb7 %lld skipped",
	    (long long)n_checked, (long long)n_drift, (long long)n_pending, (long long)n_armed, (long long)n_skipped);
	LogEvent(con, n_drift > 0 ? "warn" : "info", "verify", "VERIFY_SUMMARY", summary,
	         "{\"checked\":" + std::to_string(n_checked) + ",\"drift\":" + std::to_string(n_drift) + ",\"pending\":" +
	             std::to_string(n_pending) + ",\"armed\":" + std::to_string(n_armed) + ",\"ok\":" +
	             std::to_string(n_ok) + ",\"skipped\":" + std::to_string(n_skipped) + "}");
	Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] " + summary);
	return summary;
}

} // namespace duckdb
