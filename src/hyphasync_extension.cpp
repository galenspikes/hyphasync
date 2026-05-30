#define DUCKDB_EXTENSION_MAIN

#include "hyphasync_extension.hpp"
#include "hypha_metadata.hpp"
#include "hypha_postgres.hpp"
#include "hypha_snapshot.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
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

		const auto redacted = RedactConnString(conn_string);
		const std::string details = "{\"target\":\"default\",\"database\":\"" + JsonEscape(probe.database) +
		                            "\",\"user\":\"" + JsonEscape(probe.user) +
		                            "\",\"latency_ms\":" + std::to_string(probe.latency_ms) + ",\"conn\":\"" +
		                            JsonEscape(redacted) + "\"}";
		LogEvent(con, "info", "init", "OK", "metadata initialized for target 'default'", details);

		std::string summary = "hyphasync metadata initialized (target=default)";
		if (!probe.database.empty()) {
			summary += "; connected to database '" + probe.database + "'";
			if (!probe.user.empty()) {
				summary += " as '" + probe.user + "'";
			}
			summary += " in " + std::to_string(probe.latency_ms) + "ms";
		}
		result_data[i] = StringVector::AddString(result, summary);
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

// Phase 2+ roadmap (see docs/ROADMAP.md). The functions below are scaffolded as
// loud placeholders: calling one raises a NotImplementedException explaining that
// it is not implemented and that nothing was changed. This guarantees no silent
// failures and no accidental no-ops while the pipeline is being built out.

static void ThrowWorkflowPlaceholder(const char *signature, const char *roadmap_step) {
	throw NotImplementedException(
	    "%s is a scaffolded placeholder and is NOT implemented yet (roadmap: %s). "
	    "It performed no action: nothing was captured locally and nothing was written to the Postgres target. "
	    "See docs/ROADMAP.md for status.",
	    signature, roadmap_step);
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

static void HyphaBaseSnapshotFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = RunBaseSnapshot(con);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
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

static void HyphaSyncFun(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &context = state.GetContext();
	Connection con(*context.db);
	const auto report = RunSync(con);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, report);
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction hypha_hello("hypha_hello", {}, LogicalType::VARCHAR, HyphaHelloFun);
	loader.RegisterFunction(hypha_hello);

	ScalarFunction hypha_doctor("hypha_doctor", {}, LogicalType::VARCHAR, HyphaDoctorFun);
	hypha_doctor.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_doctor);

	ScalarFunction hypha_init("hypha_init", {LogicalType::VARCHAR}, LogicalType::VARCHAR, HyphaInitFun);
	hypha_init.SetStability(FunctionStability::VOLATILE);
	hypha_init.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(hypha_init);

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

	ScalarFunction hypha_base_snapshot("hypha_base_snapshot", {}, LogicalType::VARCHAR, HyphaBaseSnapshotFun);
	hypha_base_snapshot.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_base_snapshot);

	ScalarFunction hypha_sync_plan("hypha_sync_plan", {}, LogicalType::VARCHAR, HyphaSyncPlanFun);
	hypha_sync_plan.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_sync_plan);

	ScalarFunction hypha_sync("hypha_sync", {}, LogicalType::VARCHAR, HyphaSyncFun);
	hypha_sync.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_sync);
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
