#define DUCKDB_EXTENSION_MAIN

#include "hyphasync_extension.hpp"
#include "hypha_metadata.hpp"
#include "hypha_postgres.hpp"
#include "hypha_snapshot.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

static std::string JsonEscape(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (const char c : value) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	return out;
}

static void HyphaHelloFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	(void)state;
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, "hyphasync extension loaded");
}

static void HyphaDoctorFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = BuildDoctorReport(con);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
}

static std::string BuildInitStatusLine(const HyphaConnectionProbe &probe) {
	// Format: [hyphasync] connected · host=<host>:<port> · db=<db> · user=<user> · <N>ms
	// U+00B7 middle dot encoded as UTF-8 \xc2\xb7
	std::string line = "[hyphasync] connected";
	if (!probe.host.empty()) {
		line += " \xc2\xb7 host=" + probe.host;
		if (!probe.port.empty()) {
			line += ":" + probe.port;
		}
	}
	if (!probe.database.empty()) {
		line += " \xc2\xb7 db=" + probe.database;
	}
	if (!probe.user.empty()) {
		line += " \xc2\xb7 user=" + probe.user;
	}
	line += " \xc2\xb7 " + std::to_string(probe.latency_ms) + "ms";
	return line;
}

static void HyphaInitFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Connection con(*context.db);

	auto &input = args.data[0];
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		const auto value = input.GetValue(i);
		if (value.IsNull()) {
			throw InvalidInputException("hypha_init requires a Postgres connection string, but received NULL. "
			                            "Pass a URL like 'postgresql://user:pass@host:5432/dbname'.");
		}
		const auto conn_string = value.ToString();
		if (conn_string.empty()) {
			throw InvalidInputException("hypha_init requires a non-empty Postgres connection string. "
			                            "Pass a URL like 'postgresql://user:pass@host:5432/dbname'.");
		}

		// Always verify we can actually reach the Postgres target before storing it.
		const auto probe = ProbeHyphaConnection(conn_string);
		if (!probe.connected) {
			std::string detail = probe.error.empty() ? "could not connect to Postgres target" : probe.error;
			if (!probe.error_code.empty()) {
				detail += " (SQLSTATE " + probe.error_code + ")";
			}
			throw ConnectionException(
			    "hypha_init could not connect to the Postgres target, so nothing was initialized: %s. "
			    "Verify the connection string, that the server is reachable, and that credentials are correct.",
			    detail);
		}

		EnsureHyphaMetadata(con, conn_string);
		// No explicit limit: reset to 0 (unlimited) so a prior limit doesn't persist.
		SetMaxRowsPerTable(con, 0);
		SetFastMode(con, false);

		const auto redacted = RedactConnString(conn_string);
		const std::string details = "{\"target\":\"default\",\"database\":\"" + JsonEscape(probe.database) +
		                            "\",\"user\":\"" + JsonEscape(probe.user) +
		                            "\",\"latency_ms\":" + std::to_string(probe.latency_ms) + ",\"conn\":\"" +
		                            JsonEscape(redacted) + "\"}";
		LogEvent(con, "info", "init", "OK", "metadata initialized for target 'default'", details);

		Printer::Print(OutputStream::STREAM_STDERR, BuildInitStatusLine(probe));
		result_data[i] = StringVector::AddString(result, "");
	}
}

static void HyphaInitWithLimitFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Connection con(*context.db);

	auto &input = args.data[0];
	auto &limit_input = args.data[1];
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		const auto value = input.GetValue(i);
		if (value.IsNull()) {
			throw InvalidInputException("hypha_init requires a Postgres connection string, but received NULL. "
			                            "Pass a URL like 'postgresql://user:pass@host:5432/dbname'.");
		}
		const auto conn_string = value.ToString();
		if (conn_string.empty()) {
			throw InvalidInputException("hypha_init requires a non-empty Postgres connection string. "
			                            "Pass a URL like 'postgresql://user:pass@host:5432/dbname'.");
		}

		// Read max_rows_per_table; default 0 (unlimited) when NULL or negative.
		int64_t max_rows = 0;
		{
			const auto lv = limit_input.GetValue(i);
			if (!lv.IsNull()) {
				const int64_t raw = lv.GetValue<int64_t>();
				max_rows = (raw > 0) ? raw : 0;
			}
		}

		const auto probe = ProbeHyphaConnection(conn_string);
		if (!probe.connected) {
			std::string detail = probe.error.empty() ? "could not connect to Postgres target" : probe.error;
			if (!probe.error_code.empty()) {
				detail += " (SQLSTATE " + probe.error_code + ")";
			}
			throw ConnectionException(
			    "hypha_init could not connect to the Postgres target, so nothing was initialized: %s. "
			    "Verify the connection string, that the server is reachable, and that credentials are correct.",
			    detail);
		}

		EnsureHyphaMetadata(con, conn_string);
		SetMaxRowsPerTable(con, max_rows);
		SetFastMode(con, false);

		const auto redacted = RedactConnString(conn_string);
		const std::string details =
		    "{\"target\":\"default\",\"database\":\"" + JsonEscape(probe.database) + "\",\"user\":\"" +
		    JsonEscape(probe.user) + "\",\"latency_ms\":" + std::to_string(probe.latency_ms) + ",\"conn\":\"" +
		    JsonEscape(redacted) + "\",\"max_rows_per_table\":" + std::to_string(max_rows) + "}";
		LogEvent(con, "info", "init", "OK", "metadata initialized for target 'default'", details);

		Printer::Print(OutputStream::STREAM_STDERR, BuildInitStatusLine(probe));
		result_data[i] = StringVector::AddString(result, "");
	}
}

static void HyphaInitWithFastModeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Connection con(*context.db);

	auto &input = args.data[0];
	auto &limit_input = args.data[1];
	auto &fast_mode_input = args.data[2];
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		const auto value = input.GetValue(i);
		if (value.IsNull()) {
			throw InvalidInputException("hypha_init requires a Postgres connection string, but received NULL. "
			                            "Pass a URL like 'postgresql://user:pass@host:5432/dbname'.");
		}
		const auto conn_string = value.ToString();
		if (conn_string.empty()) {
			throw InvalidInputException("hypha_init requires a non-empty Postgres connection string. "
			                            "Pass a URL like 'postgresql://user:pass@host:5432/dbname'.");
		}

		int64_t max_rows = 0;
		{
			const auto lv = limit_input.GetValue(i);
			if (!lv.IsNull()) {
				const int64_t raw = lv.GetValue<int64_t>();
				max_rows = (raw > 0) ? raw : 0;
			}
		}

		bool fast_mode = false;
		{
			const auto fv = fast_mode_input.GetValue(i);
			if (!fv.IsNull()) {
				fast_mode = fv.GetValue<bool>();
			}
		}

		const auto probe = ProbeHyphaConnection(conn_string);
		if (!probe.connected) {
			std::string detail = probe.error.empty() ? "could not connect to Postgres target" : probe.error;
			if (!probe.error_code.empty()) {
				detail += " (SQLSTATE " + probe.error_code + ")";
			}
			throw ConnectionException(
			    "hypha_init could not connect to the Postgres target, so nothing was initialized: %s. "
			    "Verify the connection string, that the server is reachable, and that credentials are correct.",
			    detail);
		}

		EnsureHyphaMetadata(con, conn_string);
		SetMaxRowsPerTable(con, max_rows);
		SetFastMode(con, fast_mode);

		const auto redacted = RedactConnString(conn_string);
		const std::string details = "{\"target\":\"default\",\"database\":\"" + JsonEscape(probe.database) +
		                            "\",\"user\":\"" + JsonEscape(probe.user) +
		                            "\",\"latency_ms\":" + std::to_string(probe.latency_ms) + ",\"conn\":\"" +
		                            JsonEscape(redacted) + "\",\"max_rows_per_table\":" + std::to_string(max_rows) +
		                            ",\"fast_mode\":" + (fast_mode ? "true" : "false") + "}";
		LogEvent(con, "info", "init", "OK", "metadata initialized for target 'default'", details);

		Printer::Print(OutputStream::STREAM_STDERR, BuildInitStatusLine(probe));
		result_data[i] = StringVector::AddString(result, "");
	}
}

static void HyphaHelpFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)state;

	// Lookup table: function name → description line.
	static const char *HELP_TABLE[][2] = {
	    {"hypha_hello", "hypha_hello()  →  VARCHAR  Confirm the extension is loaded (smoke test)"},
	    {"hypha_doctor", "hypha_doctor()  →  VARCHAR  Diagnostic report: version, metadata status, capabilities"},
	    {"hypha_init",
	     "hypha_init(conn_string [, max_rows [, fast_mode]])  →  VARCHAR  Initialize hyphasync for a Postgres target"},
	    {"hypha_target_status",
	     "hypha_target_status([conn_string])  →  VARCHAR  Read-only Postgres health probe; reports latency and remote "
	     "metadata"},
	    {"hypha_base_snapshot_plan",
	     "hypha_base_snapshot_plan()  →  VARCHAR  Walk local catalog, compute fingerprints; no Postgres writes"},
	    {"hypha_base_snapshot",
	     "hypha_base_snapshot()  →  TABLE(table_name VARCHAR, row_count BIGINT, fingerprint_strategy VARCHAR, "
	     "duration_ms DOUBLE, status VARCHAR)  Stream full base snapshot to Postgres; one row per table"},
	    {"hypha_sync_plan",
	     "hypha_sync_plan()  →  VARCHAR  Diff current state vs last applied snapshot; no Postgres writes"},
	    {"hypha_sync", "hypha_sync()  →  TABLE  Apply incremental sync to Postgres; yields one row per changed table "
	                   "(table_name, action, rows_synced, duration_ms, status). Use: SELECT * FROM hypha_sync()"},
	    {"hypha_drop",
	     "hypha_drop([drop_meta])  →  VARCHAR  DROP all hyphasync-owned schemas from the stored Postgres target. "
	     "hypha_drop(true) also drops the hypha meta schema."},
	    {"hypha_status",
	     "hypha_status()  \xe2\x86\x92  VARCHAR  Return a one-line summary of the last sync: commit_id, kind, "
	     "timestamp, and table counts. Returns a help message if no sync has been run."},
	    {"hypha_help",
	     "hypha_help([function_name])  \xe2\x86\x92  VARCHAR  List all functions or describe a specific function"},
	    {nullptr, nullptr}};

	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		std::string filter;
		if (args.data[0].GetValue(i).IsNull()) {
			// No argument — return all descriptions.
		} else {
			filter = args.data[0].GetValue(i).ToString();
		}

		std::string out;
		for (int j = 0; HELP_TABLE[j][0] != nullptr; j++) {
			if (!filter.empty() && filter != HELP_TABLE[j][0]) {
				continue;
			}
			if (!out.empty()) {
				out += "\n";
			}
			out += HELP_TABLE[j][1];
		}
		if (out.empty()) {
			out = "no help found for '" + filter + "'";
		}
		result_data[i] = StringVector::AddString(result, out);
	}
}

static void HyphaTargetStatusFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Connection con(*context.db);

	auto &input = args.data[0];
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		const auto raw = input.GetValue(i);
		std::string conn_string;
		bool explicit_url = false;
		if (!raw.IsNull()) {
			conn_string = raw.ToString();
		}
		if (!conn_string.empty()) {
			explicit_url = true;
		} else {
			conn_string = GetDefaultTargetConnString(con);
		}
		const std::string label = explicit_url ? "(explicit)" : "default";
		const std::string details =
		    "{\"target\":\"" + JsonEscape(label) + "\",\"conn\":\"" + JsonEscape(RedactConnString(conn_string)) + "\"}";
		try {
			const auto report = RunHyphaTargetStatus(conn_string, label);
			const bool degraded = report.find("status=degraded") != std::string::npos;
			LogEvent(con, degraded ? "warn" : "info", "target_status", degraded ? "DEGRADED" : "OK",
			         "probe of target '" + label + "' " + (degraded ? "degraded" : "ok"), details);
			result_data[i] = StringVector::AddString(result, report);
		} catch (const std::exception &ex) {
			LogEvent(con, "error", "target_status", "FAILED", "probe of target '" + label + "' failed: " + ex.what(),
			         details);
			throw;
		}
	}
}

static void HyphaBaseSnapshotPlanFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = RunBaseSnapshotPlan(con);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
}

// Compatibility shim: SELECT hypha_base_snapshot() (scalar syntax) now throws a clear migration error.
// Use SELECT * FROM hypha_base_snapshot() to invoke the TableFunction.
static void HyphaBaseSnapshotCompatFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	(void)state;
	(void)result;
	throw InvalidInputException(
	    "hypha_base_snapshot() is now a table function. Use: SELECT * FROM hypha_base_snapshot()");
}

static void HyphaSyncPlanFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = RunSyncPlan(con);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
}

static void HyphaSyncCompatFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	(void)state;
	(void)result;
	throw InvalidInputException("hypha_sync() is now a table function. Use: SELECT * FROM hypha_sync()");
}

static void HyphaDropFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = RunHyphaDrop(con, false);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
}

static void HyphaDropWithMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Connection con(*context.db);

	auto &drop_meta_input = args.data[0];
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		bool drop_meta = false;
		const auto mv = drop_meta_input.GetValue(i);
		if (!mv.IsNull()) {
			drop_meta = mv.GetValue<bool>();
		}
		const auto report = RunHyphaDrop(con, drop_meta);
		result_data[i] = StringVector::AddString(result, report);
	}
}

static void HyphaStatusFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = RunHyphaStatus(con);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
}

static void LoadInternal(ExtensionLoader &loader) {
	// Ensure the json extension is loaded for ::JSON casts on LIST/STRUCT/MAP columns used in
	// fingerprinting and COPY. When json is statically bundled (extension_config.cmake), this
	// is a no-op. For loadable-extension usage against an external DuckDB binary it covers the
	// common case where json is installed but not yet activated in the host process.
	{
		Connection json_con(loader.GetDatabaseInstance());
		json_con.Query("LOAD json");
	}

	ScalarFunction hypha_hello("hypha_hello", {}, LogicalType::VARCHAR, HyphaHelloFun);
	loader.RegisterFunction(hypha_hello);

	ScalarFunction hypha_doctor("hypha_doctor", {}, LogicalType::VARCHAR, HyphaDoctorFun);
	hypha_doctor.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_doctor);

	// hypha_init has three overloads:
	//   hypha_init(conn_string)                              -- no row limit, fast_mode=false (default)
	//   hypha_init(conn_string, max_rows_per_table)          -- skip tables with row_count > N
	//   hypha_init(conn_string, max_rows_per_table, fast_mode) -- set fast_mode (synchronous_commit=off)
	ScalarFunctionSet hypha_init_set("hypha_init");

	ScalarFunction hypha_init_1arg({LogicalType::VARCHAR}, LogicalType::VARCHAR, HyphaInitFun);
	hypha_init_1arg.SetStability(FunctionStability::VOLATILE);
	hypha_init_1arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	hypha_init_set.AddFunction(hypha_init_1arg);

	ScalarFunction hypha_init_2arg({LogicalType::VARCHAR, LogicalType::BIGINT}, LogicalType::VARCHAR,
	                               HyphaInitWithLimitFun);
	hypha_init_2arg.SetStability(FunctionStability::VOLATILE);
	hypha_init_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	hypha_init_set.AddFunction(hypha_init_2arg);

	ScalarFunction hypha_init_3arg({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BOOLEAN},
	                               LogicalType::VARCHAR, HyphaInitWithFastModeFun);
	hypha_init_3arg.SetStability(FunctionStability::VOLATILE);
	hypha_init_3arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	hypha_init_set.AddFunction(hypha_init_3arg);

	loader.RegisterFunction(hypha_init_set);

	ScalarFunction hypha_target_status("hypha_target_status", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                   HyphaTargetStatusFun);
	hypha_target_status.SetStability(FunctionStability::VOLATILE);
	hypha_target_status.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(hypha_target_status);

	// Scaffolded main-workflow placeholders (throw NotImplementedException until built out).
	ScalarFunction hypha_base_snapshot_plan("hypha_base_snapshot_plan", {}, LogicalType::VARCHAR,
	                                        HyphaBaseSnapshotPlanFun);
	hypha_base_snapshot_plan.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_base_snapshot_plan);

	// hypha_base_snapshot() is a streaming TableFunction (yields one row per table).
	RegisterBaseSnapshotTableFunction(loader);

	// Scalar shim: SELECT hypha_base_snapshot() throws a clear migration message pointing to the
	// TableFunction syntax. DuckDB routes SELECT * FROM hypha_base_snapshot() to the TableFunction
	// and SELECT hypha_base_snapshot() to this scalar — both names coexist in separate catalogs.
	ScalarFunction hypha_base_snapshot_compat("hypha_base_snapshot", {}, LogicalType::VARCHAR,
	                                          HyphaBaseSnapshotCompatFun);
	hypha_base_snapshot_compat.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_base_snapshot_compat);

	ScalarFunction hypha_sync_plan("hypha_sync_plan", {}, LogicalType::VARCHAR, HyphaSyncPlanFun);
	hypha_sync_plan.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_sync_plan);

	// hypha_sync() is a streaming TableFunction — yields one row per changed table.
	RegisterSyncTableFunction(loader);

	// Scalar shim: SELECT hypha_sync() throws a clear migration message pointing to the
	// TableFunction syntax. DuckDB routes SELECT * FROM hypha_sync() to the TableFunction
	// and SELECT hypha_sync() to this scalar — both names coexist in separate catalogs.
	ScalarFunction hypha_sync_compat("hypha_sync", {}, LogicalType::VARCHAR, HyphaSyncCompatFun);
	hypha_sync_compat.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_sync_compat);

	// hypha_drop() — two overloads:
	//   hypha_drop()        → drop all non-system, non-hypha schemas
	//   hypha_drop(true)    → same, plus drop the hypha meta schema
	ScalarFunctionSet hypha_drop_set("hypha_drop");

	ScalarFunction hypha_drop_0arg({}, LogicalType::VARCHAR, HyphaDropFun);
	hypha_drop_0arg.SetStability(FunctionStability::VOLATILE);
	hypha_drop_set.AddFunction(hypha_drop_0arg);

	ScalarFunction hypha_drop_1arg({LogicalType::BOOLEAN}, LogicalType::VARCHAR, HyphaDropWithMetaFun);
	hypha_drop_1arg.SetStability(FunctionStability::VOLATILE);
	hypha_drop_1arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	hypha_drop_set.AddFunction(hypha_drop_1arg);

	loader.RegisterFunction(hypha_drop_set);

	// hypha_status() — quick one-line summary of the last sync state (no Postgres required).
	ScalarFunction hypha_status("hypha_status", {}, LogicalType::VARCHAR, HyphaStatusFun);
	hypha_status.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_status);

	// hypha_help() — SQL-accessible function reference.
	// hypha_help()           → all function descriptions
	// hypha_help('fn_name')  → description for that specific function
	ScalarFunction hypha_help("hypha_help", {LogicalType::VARCHAR}, LogicalType::VARCHAR, HyphaHelpFun);
	hypha_help.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(hypha_help);
}

void HyphasyncExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string HyphasyncExtension::Name() {
	return "hyphasync";
}

std::string HyphasyncExtension::Version() const {
#ifdef EXT_VERSION_HYPHASYNC
	return EXT_VERSION_HYPHASYNC;
#else
	return HYPHASYNC_VERSION;
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(hyphasync, loader) {
	duckdb::LoadInternal(loader);
}
}
