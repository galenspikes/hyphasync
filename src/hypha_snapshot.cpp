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

} // namespace duckdb
