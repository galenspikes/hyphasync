#define DUCKDB_EXTENSION_MAIN

#include "hyphasync_extension.hpp"
#include "hypha_metadata.hpp"
#include "hypha_postgres.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/connection.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

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

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t conn_string) {
		EnsureHyphaMetadata(con, conn_string.GetString());
		return StringVector::AddString(result, "hyphasync metadata initialized (target=default)");
	});
}

static void HyphaAttachCheckFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Connection con(*context.db);

	auto &input = args.data[0];
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		std::string conn_string;
		if (!input.GetValue(i).IsNull()) {
			conn_string = input.GetValue(i).ToString();
		}
		if (conn_string.empty()) {
			conn_string = GetDefaultTargetConnString(con);
		}
		result_data[i] = StringVector::AddString(result, RunHyphaAttachCheck(conn_string));
	}
}

// Phase 2+ roadmap (see docs/ROADMAP.md):
// TODO: remote hypha metadata on Postgres target
// TODO: hypha_base_snapshot_plan(), hypha_base_snapshot()
// TODO: object lineage comments and fingerprint/hashing capture
// TODO: hypha_sync_plan(), hypha_sync() — on-demand snapshot-diff, no CDC/WAL

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction hypha_hello("hypha_hello", {}, LogicalType::VARCHAR, HyphaHelloFun);
	loader.RegisterFunction(hypha_hello);

	ScalarFunction hypha_doctor("hypha_doctor", {}, LogicalType::VARCHAR, HyphaDoctorFun);
	hypha_doctor.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_doctor);

	ScalarFunction hypha_init("hypha_init", {LogicalType::VARCHAR}, LogicalType::VARCHAR, HyphaInitFun);
	hypha_init.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(hypha_init);

	ScalarFunction hypha_attach_check("hypha_attach_check", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                  HyphaAttachCheckFun);
	hypha_attach_check.SetStability(FunctionStability::VOLATILE);
	hypha_attach_check.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(hypha_attach_check);
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
