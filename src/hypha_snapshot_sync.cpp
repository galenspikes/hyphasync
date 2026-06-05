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

std::string RunSyncPlan(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException("hypha metadata is not initialized — call hypha_init() first.");
	}

	// Require a prior applied or partial snapshot to diff against.
	const auto base_result = Exec(con, R"(
SELECT
    COALESCE(c.parent_commit_id, c.commit_id) AS commit_id,
    COALESCE(p.fingerprint_algo, c.fingerprint_algo, 'none') AS old_algo
FROM hypha.commit c
LEFT JOIN hypha.commit p ON p.commit_id = c.parent_commit_id
WHERE c.status IN ('applied', 'partial')
ORDER BY c.applied_at DESC NULLS LAST, c.created_at DESC
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

	// Refuse to diff if the old snapshot used a different (real) fingerprint version.
	// Comparing v1 hashes against v2 hashes would silently produce wrong diffs.
	if (old_fp_algo != "none" && old_fp_algo != std::string(HYPHA_FINGERPRINT_ALGO)) {
		throw InvalidInputException("fingerprint_algo mismatch: the last applied snapshot used '%s' but the current "
		                            "code uses '%s'. Run hypha_base_snapshot() to establish a fresh baseline before "
		                            "syncing — diffing across fingerprint versions would produce wrong results.",
		                            old_fp_algo, HYPHA_FINGERPRINT_ALGO);
	}
	// Warn (don't block) if the old snapshot has no fingerprints: diff falls back to row-count.
	if (old_fp_algo == "none") {
		LogEvent(con, "warn", "sync_plan", "OLD_SNAPSHOT_UNFINGERPRINTED",
		         "last applied snapshot has no fingerprints (pre-v1); diff falls back to "
		         "column-signature + row-count only. Run hypha_base_snapshot() for a full "
		         "fingerprint baseline.",
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

	// Print elegant summary to stderr; return empty VARCHAR.
	// U+00B7 middle dot = \xc2\xb7 (UTF-8); U+2192 right arrow = \xe2\x86\x92
	const std::string short_old_id = old_commit_id.size() >= 8 ? old_commit_id.substr(0, 8) : old_commit_id;

	if (n_to_sync == 0) {
		Printer::Print(
		    OutputStream::STREAM_STDERR,
		    StringUtil::Format("[hyphasync] sync plan \xc2\xb7 database=%s \xc2\xb7 all %lld table%s unchanged "
		                       "\xc2\xb7 nothing to sync",
		                       db_name.c_str(), (long long)n_skip, n_skip == 1 ? "" : "s"));
	} else {
		Printer::Print(OutputStream::STREAM_STDERR,
		               StringUtil::Format("[hyphasync] sync plan \xc2\xb7 database=%s \xc2\xb7 %lld table%s changed "
		                                  "\xc2\xb7 %lld unchanged \xc2\xb7 based on commit: %s",
		                                  db_name.c_str(), (long long)n_to_sync, n_to_sync == 1 ? "" : "s",
		                                  (long long)n_skip, short_old_id.c_str()));
		Printer::Print(
		    OutputStream::STREAM_STDERR,
		    StringUtil::Format("[hyphasync]   new: %lld   dropped: %lld   schema changed: %lld   rows changed: %lld",
		                       (long long)n_new, (long long)n_dropped, (long long)n_schema, (long long)n_rows));
		for (const auto &a : actions) {
			if (a.kind == TableAction::Kind::LIKELY_UNCHANGED) {
				continue;
			}
			const std::string qualified = a.schema_name + "." + a.table_name;
			const std::string action_str = ActionLabel(a.kind);
			std::string row_info;
			if (a.kind == TableAction::Kind::SCHEMA_CHANGED) {
				row_info = a.detail;
			} else if (a.kind != TableAction::Kind::DROPPED) {
				if (a.old_rows == a.new_rows) {
					row_info = std::to_string(a.new_rows) + " rows (content changed)";
				} else {
					row_info = std::to_string(a.old_rows) + " \xe2\x86\x92 " + std::to_string(a.new_rows) + " rows";
				}
			}
			char line[256];
			snprintf(line, sizeof(line), "[hyphasync]   %-22s %-16s %s", qualified.c_str(), action_str.c_str(),
			         row_info.c_str());
			Printer::Print(OutputStream::STREAM_STDERR, line);
		}
		Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync]   run hypha_sync() to apply");
	}
	return "";
}

// ---------------------------------------------------------------------------
// hypha_sync(): apply the plan
// ---------------------------------------------------------------------------

static std::string FormatDurMs(double ms) {
	char buf[32];
	if (ms < 10.0) {
		snprintf(buf, sizeof(buf), "%.1fms", ms < 0 ? 0.0 : ms);
	} else {
		snprintf(buf, sizeof(buf), "%.0fms", ms);
	}
	return std::string(buf);
}

static std::string FormatRows(int64_t n) {
	return std::to_string(n) + (n == 1 ? " row" : " rows");
}

std::vector<SyncTableResult> RunSync(Connection &con) {
	if (!IsHyphaMetadataInitialized(con)) {
		throw InvalidInputException("hypha metadata is not initialized — call hypha_init() first.");
	}

	// Require a prior applied or partial snapshot.
	const auto base_result = Exec(con, R"(
SELECT
    COALESCE(c.parent_commit_id, c.commit_id) AS commit_id,
    COALESCE(p.fingerprint_algo, c.fingerprint_algo, 'none') AS old_algo
FROM hypha.commit c
LEFT JOIN hypha.commit p ON p.commit_id = c.parent_commit_id
WHERE c.status IN ('applied', 'partial')
ORDER BY c.applied_at DESC NULLS LAST, c.created_at DESC
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
	const bool fast_mode = GetFastMode(con);

	// Refuse to sync across fingerprint versions — wrong diffs would be worse than no sync.
	if (old_fp_algo != "none" && old_fp_algo != std::string(HYPHA_FINGERPRINT_ALGO)) {
		throw InvalidInputException("fingerprint_algo mismatch: the last applied snapshot used '%s' but the current "
		                            "code uses '%s'. Run hypha_base_snapshot() to re-establish a baseline first.",
		                            old_fp_algo, HYPHA_FINGERPRINT_ALGO);
	}

	// Fresh catalog walk.
	RunBaseSnapshotPlan(con);
	const auto new_commit_result = Exec(con, R"(
SELECT commit_id FROM hypha.commit
WHERE kind = 'base_snapshot_plan' AND status = 'completed'
ORDER BY created_at DESC LIMIT 1)",
	                                    "get new plan commit");
	const std::string new_commit_id = new_commit_result->GetValue(0, 0).ToString();

	const auto actions = ComputeDiff(con, old_commit_id, new_commit_id);

	// Pre-load collision-safe Postgres table names assigned during RunBaseSnapshotPlan.
	// Key: "schema_name\x00table_name" to avoid ambiguity.
	// Load from both commits: new commit for new/changed tables, old commit for dropped tables.
	std::unordered_map<std::string, std::string> sync_pg_name_map;
	{
		// Load old commit names first (for dropped tables); new commit names will overwrite.
		for (const auto &cid : {old_commit_id, new_commit_id}) {
			const auto pn_res = Exec(con,
			                         StringUtil::Format("SELECT schema_name, object_name, "
			                                            "COALESCE(pg_table_name, substr(object_name, 1, 63)) "
			                                            "FROM hypha.object_snapshot "
			                                            "WHERE commit_id = %s AND object_type = 'table'",
			                                            QuoteLiteral(cid)),
			                         "load pg_table_names for sync");
			for (idx_t i = 0; i < pn_res->RowCount(); i++) {
				const auto key = pn_res->GetValue(0, i).ToString() + '\0' + pn_res->GetValue(1, i).ToString();
				sync_pg_name_map[key] = pn_res->GetValue(2, i).ToString();
			}
		}
	}

	// Generate the sync commit ID early so it can be used in remote metadata writes
	// (which happen between the Postgres COMMIT and PQfinish).
	const std::string sync_commit_id =
	    Exec(con, "SELECT uuid()::VARCHAR", "gen sync commit id")->GetValue(0, 0).ToString();

	// Connect to Postgres.
	PGconn *pg = OpenHyphaConnection(conn_string, fast_mode);

	// Dedicated auto-commit log connection for real-time event mirroring (best-effort).
	PGconn *pg_log = nullptr;
	try {
		pg_log = OpenHyphaConnection(conn_string, fast_mode);
		EnsureRemoteEventLog(pg_log);
	} catch (...) {
		if (pg_log) {
			PQfinish(pg_log);
			pg_log = nullptr;
		}
	}

	int64_t tables_synced = 0, tables_unchanged = 0, tables_new = 0, tables_dropped = 0, rows_synced = 0,
	        tables_truncate_copy = 0;
	std::vector<SyncTableResult> results;
	const auto sync_overall_start = std::chrono::steady_clock::now();

	try {
		PGExec(pg, "BEGIN", "BEGIN transaction");
		Printer::Print(OutputStream::STREAM_STDERR,
		               StringUtil::Format("[hyphasync] sync: checking %lld tables...", (long long)actions.size()));

		for (const auto &a : actions) {
			const auto pg_schema = MakePostgresSchemaName(db_name, a.schema_name);
			const auto pg_name_key = a.schema_name + '\0' + a.table_name;
			const auto pg_name_it = sync_pg_name_map.find(pg_name_key);
			const auto pg_table =
			    (pg_name_it != sync_pg_name_map.end()) ? pg_name_it->second : TruncPGIdent(a.table_name);

			if (a.kind == TableAction::Kind::LIKELY_UNCHANGED) {
				tables_unchanged++;
				continue;
			}

			// Use a savepoint so a per-table failure skips that table.
			const auto sp = "sp_" + SanitizeIdent(a.table_name).substr(0, 50);
			PGExec(pg, "SAVEPOINT " + sp, "SAVEPOINT");

			const auto table_t0 = std::chrono::steady_clock::now();
			const std::string tbl_label = a.schema_name + "." + a.table_name;
			bool tbl_is_new = false;
			bool tbl_is_tc = false;

			bool ok = true;
			std::string err_msg;
			int64_t table_rows = 0;

			try {
				if (a.kind == TableAction::Kind::DROPPED) {
					PGExec(pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table),
					       "DROP TABLE");
					tables_dropped++;
					{
						const double dur_ms =
						    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - table_t0)
						        .count();
						Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] - " + tbl_label + "  DROPPED");
						results.push_back({tbl_label, "dropped", 0, dur_ms, "ok"});
					}

				} else {
					// NEW, SCHEMA_CHANGED, ROWS_CHANGED all need the column list.
					const auto cols_result =
					    Exec(con,
					         StringUtil::Format(R"(
SELECT column_name, postgres_type, is_nullable, duckdb_type
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
						cd.duckdb_type = cols_result->GetValue(3, c).ToString();
						cd.needs_json_cast = NeedsJsonCastForCopy(cd.duckdb_type);
						cols.push_back(cd);
					}
					if (cols.empty()) {
						tables_unchanged++;
						PGExec(pg, "RELEASE SAVEPOINT " + sp, "RELEASE SAVEPOINT");
						continue;
					}

					const auto pg_names = ResolveColumnNames(cols);

					// Fetch hypha_object_id once for all DDL paths below.
					const auto hypha_oid = GetHyphaObjectId(con, a.schema_name, a.table_name, new_commit_id);

					if (a.kind == TableAction::Kind::NEW) {
						tbl_is_new = true;
						PGExec(pg, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdent(pg_schema),
						       "CREATE SCHEMA IF NOT EXISTS");
						StampHyphaSchemaComment(pg, pg_schema, db_name, a.schema_name, new_commit_id);
						PGExec(pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table),
						       "DROP TABLE IF EXISTS");
						PGExec(pg, BuildCreateTableDDL(pg_schema, pg_table, cols, pg_names), "CREATE TABLE");
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
						{
							const int64_t fw_bytes = EstimateFixedWidthRowBytes(cols);
							if (fw_bytes > 7000) {
								const std::string tbl_qn = a.schema_name + "." + a.table_name;
								LogAll(con, pg_log, db_name, "warn", "sync", "TABLE_TOO_WIDE",
								       "TABLE_TOO_WIDE: table '" + tbl_qn +
								           "' has an estimated fixed-width row size of ~" + std::to_string(fw_bytes) +
								           " bytes, which exceeds Postgres's 8 KB heap row limit even after "
								           "TOAST-ing varlena columns. Consider splitting the table or converting "
								           "fixed-width columns to text/numeric/jsonb.");
							}
						}
						if (!hypha_oid.empty()) {
							StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name,
							                       a.table_name, new_commit_id);
						}
					} else if (a.kind == TableAction::Kind::SCHEMA_CHANGED) {
						// Compute what actually changed to choose the cheapest DDL path.
						const auto cdelta =
						    ComputeColumnDelta(con, old_commit_id, new_commit_id, a.schema_name, a.table_name);

						if (cdelta.has_type_changes || cdelta.has_reordering) {
							// Full recreate: type change or column reordering requires DROP+CREATE
							// since Postgres cannot cheaply retype or reorder columns in-place.
							PGExec(pg, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdent(pg_schema),
							       "CREATE SCHEMA IF NOT EXISTS");
							StampHyphaSchemaComment(pg, pg_schema, db_name, a.schema_name, new_commit_id);
							PGExec(pg, "DROP TABLE IF EXISTS " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table),
							       "DROP TABLE IF EXISTS");
							PGExec(pg, BuildCreateTableDDL(pg_schema, pg_table, cols, pg_names), "CREATE TABLE");
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
								       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " " +
								           alter,
								       "SET STORAGE EXTERNAL");
							}
							{
								const int64_t fw_bytes = EstimateFixedWidthRowBytes(cols);
								if (fw_bytes > 7000) {
									const std::string tbl_qn = a.schema_name + "." + a.table_name;
									LogAll(con, pg_log, db_name, "warn", "sync", "TABLE_TOO_WIDE",
									       "TABLE_TOO_WIDE: table '" + tbl_qn +
									           "' has an estimated fixed-width row size of ~" +
									           std::to_string(fw_bytes) +
									           " bytes, which exceeds Postgres's 8 KB heap row limit even after "
									           "TOAST-ing varlena columns. Consider splitting the table or converting "
									           "fixed-width columns to text/numeric/jsonb.");
								}
							}
							if (!hypha_oid.empty()) {
								StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name,
								                       a.table_name, new_commit_id);
							}
							// Fall through to COPY block below.
						} else if (cdelta.added.empty() && !cdelta.dropped.empty()) {
							// Drop-only: ALTER TABLE DROP COLUMN — no data movement needed.
							// Preserves all Postgres-side indexes, constraints, and grants.
							std::string drop_clause;
							for (const auto &col_name : cdelta.dropped) {
								if (!drop_clause.empty()) {
									drop_clause += ", ";
								}
								drop_clause += "DROP COLUMN IF EXISTS " + QuoteIdent(TruncPGIdent(col_name));
							}
							PGExec(pg,
							       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " " +
							           drop_clause,
							       "ALTER TABLE DROP COLUMN");
							if (!hypha_oid.empty()) {
								StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name,
								                       a.table_name, new_commit_id);
							}
							tables_synced++;
							{
								const double dur_ms = std::chrono::duration<double, std::milli>(
								                          std::chrono::steady_clock::now() - table_t0)
								                          .count();
								Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] ~ " + tbl_label +
								                                                "  SCHEMA_CHANGED  " +
								                                                FormatDurMs(dur_ms));
								results.push_back({tbl_label, "schema_changed", 0, dur_ms, "ok"});
							}
							PGExec(pg, "RELEASE SAVEPOINT " + sp, "RELEASE SAVEPOINT");
							continue; // No COPY needed — existing row data is correct after drops.
						} else {
							// Add-only or mixed add+drop: ALTER TABLE then TRUNCATE+COPY.
							// Preserves Postgres-side indexes and constraints while refreshing data.
							for (const auto &col_name : cdelta.dropped) {
								PGExec(pg,
								       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) +
								           " DROP COLUMN IF EXISTS " + QuoteIdent(TruncPGIdent(col_name)),
								       "ALTER TABLE DROP COLUMN");
							}
							for (const auto &add_col : cdelta.added) {
								const std::string add_clause = "ADD COLUMN " + QuoteIdent(TruncPGIdent(add_col.name)) +
								                               " " + add_col.postgres_type;
								PGExec(pg,
								       "ALTER TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " " +
								           add_clause,
								       "ALTER TABLE ADD COLUMN");
							}
							if (!hypha_oid.empty()) {
								StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name,
								                       a.table_name, new_commit_id);
							}
							PGExec(pg, "TRUNCATE TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table),
							       "TRUNCATE");
							// Fall through to COPY block to repopulate all rows.
						}
					} else {
						// ROWS_CHANGED: try row-level diff (INSERT/UPDATE/DELETE) first.
						// Supports single and composite PKs. Falls back to TRUNCATE+COPY for
						// no-PK tables or tables without row hashes from a prior snapshot.
						RowDiff rd;
						const bool used_row_level = ApplyRowLevelDiff(con, pg, a.schema_name, a.table_name, pg_schema,
						                                              pg_table, old_commit_id, new_commit_id, cols, rd);
						if (used_row_level) {
							if (!hypha_oid.empty()) {
								StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name,
								                       a.table_name, new_commit_id);
							}
							tables_synced++;
							rows_synced += rd.inserts + rd.updates;
							{
								const double dur_ms = std::chrono::duration<double, std::milli>(
								                          std::chrono::steady_clock::now() - table_t0)
								                          .count();
								const int64_t added = rd.inserts + rd.updates;
								const int64_t removed = rd.deletes;
								std::string row_part = "+" + std::to_string(added);
								if (removed > 0) {
									row_part += " -" + std::to_string(removed);
								}
								row_part += (added == 1 && removed == 0) ? " row" : " rows";
								Printer::Print(OutputStream::STREAM_STDERR, "[hyphasync] ~ " + tbl_label +
								                                                "  UPDATED  " + row_part + "  " +
								                                                FormatDurMs(dur_ms));
								results.push_back({tbl_label, "updated", added, dur_ms, "ok"});
							}
							continue; // Skip the COPY block below — already applied
						}

						// Keyless append-only fast path: when a no-PK table only gained rows, COPY just
						// the new rows (no TRUNCATE, no DELETE). Returns false for any removal/update or
						// risk of duplicates, leaving the TRUNCATE+COPY fallback below.
						RowDiff krd;
						const bool used_keyless =
						    ApplyKeylessAppendDiff(con, pg, a.schema_name, a.table_name, pg_schema, pg_table,
						                           old_commit_id, new_commit_id, cols, krd);
						if (used_keyless) {
							if (!hypha_oid.empty()) {
								StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name,
								                       a.table_name, new_commit_id);
							}
							tables_synced++;
							rows_synced += krd.inserts;
							LogAll(con, pg_log, db_name, "info", "sync", "KEYLESS_APPEND",
							       a.schema_name + "." + a.table_name + ": keyless append-only fast path, +" +
							           std::to_string(krd.inserts) + " rows (no TRUNCATE)");
							const double dur_ms =
							    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - table_t0)
							        .count();
							Printer::Print(OutputStream::STREAM_STDERR,
							               "[hyphasync] + " + tbl_label + "  APPEND  +" + std::to_string(krd.inserts) +
							                   (krd.inserts == 1 ? " row  " : " rows  ") + FormatDurMs(dur_ms));
							results.push_back({tbl_label, "appended", krd.inserts, dur_ms, "ok"});
							continue; // Skip the COPY block below — already applied
						}

						// Fall back to TRUNCATE+COPY (no PK or missing row hashes).
						tbl_is_tc = true;
						tables_truncate_copy++;
						LogAll(con, pg_log, db_name, "info", "sync", "TRUNCATE_COPY",
						       a.schema_name + "." + a.table_name +
						           ": row-level diff unavailable (no PK or missing row hashes); using TRUNCATE+COPY");
						PGExec(pg, "TRUNCATE TABLE " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table), "TRUNCATE");
					}

					// COPY data in chunks (full table — NEW, SCHEMA_CHANGED, and ROWS_CHANGED fallback).
					// Use an explicit column list so unsupported columns are excluded and
					// STRUCT/MAP columns receive a ::JSON cast for Postgres JSONB import.
					{
						// Fetch PK columns for keyset pagination.
						const auto sc_pk_res = con.Query(StringUtil::Format(
						    "SELECT COALESCE(pk_columns, '') FROM hypha.object_snapshot "
						    "WHERE commit_id = %s AND schema_name = %s AND object_name = %s LIMIT 1",
						    QuoteLiteral(new_commit_id), QuoteLiteral(a.schema_name), QuoteLiteral(a.table_name)));
						const std::string sc_pk_cols =
						    (sc_pk_res && !sc_pk_res->HasError() && sc_pk_res->RowCount() > 0 &&
						     !sc_pk_res->GetValue(0, 0).IsNull())
						        ? sc_pk_res->GetValue(0, 0).ToString()
						        : "";

						// Detect single-integer PK for keyset pagination.
						std::string sc_int_pk;
						if (sc_pk_cols.find(',') == std::string::npos && !sc_pk_cols.empty()) {
							for (const auto &cd : cols) {
								if (cd.name == sc_pk_cols && IsIntegerDuckType(cd.duckdb_type)) {
									sc_int_pk = cd.name;
									break;
								}
							}
						}

						const auto sc_varchar_widths = QueryVarcharWidths(con, a.schema_name, a.table_name, cols);
						const int64_t sc_chunk_rows = ComputeChunkRows(cols, COPY_TARGET_BYTES, sc_varchar_widths);

						const std::string sync_copy_sql =
						    "COPY " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " FROM STDIN CSV NULL '\\N'";
						PGresult *cr = PQexec(pg, sync_copy_sql.c_str());
						if (PQresultStatus(cr) != PGRES_COPY_IN) {
							const auto e = TrimPQ(PQresultErrorMessage(cr));
							PQclear(cr);
							throw IOException(e);
						}
						PQclear(cr);

						int64_t sc_offset = 0;
						int64_t sc_last_pk = 0;
						bool sc_have_last_pk = false;
						while (true) {
							std::string sc_where;
							std::string sc_suffix;
							if (!sc_int_pk.empty()) {
								if (sc_have_last_pk) {
									sc_where = QuoteIdent(sc_int_pk) + " > " + std::to_string(sc_last_pk);
								}
								sc_suffix =
								    " ORDER BY " + QuoteIdent(sc_int_pk) + " LIMIT " + std::to_string(sc_chunk_rows);
							} else {
								sc_suffix =
								    " LIMIT " + std::to_string(sc_chunk_rows) + " OFFSET " + std::to_string(sc_offset);
							}

							const std::string sc_select =
							    BuildCopySelectList(a.schema_name, a.table_name, cols, sc_where) + sc_suffix;
							if (CopyChunkViaPipe(con, pg, sc_select, "sync COPY chunk") == 0) {
								break;
							}

							if (!sc_int_pk.empty()) {
								const auto max_r =
								    Exec(con,
								         "SELECT MAX(t." + QuoteIdent(sc_int_pk) + ") FROM (SELECT " +
								             QuoteIdent(sc_int_pk) + " FROM " + QuoteIdent(a.schema_name) + "." +
								             QuoteIdent(a.table_name) + (sc_where.empty() ? "" : " WHERE " + sc_where) +
								             " ORDER BY " + QuoteIdent(sc_int_pk) + " LIMIT " +
								             std::to_string(sc_chunk_rows) + ") t",
								         "sync copy chunk max pk");
								if (max_r->RowCount() == 0 || max_r->GetValue(0, 0).IsNull()) {
									break;
								}
								sc_last_pk = max_r->GetValue(0, 0).GetValue<int64_t>();
								sc_have_last_pk = true;
							} else {
								sc_offset += sc_chunk_rows;
							}
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
					}
					// Stamp after every successful COPY (covers NEW, SCHEMA_CHANGED recreate/add,
					// and ROWS_CHANGED TRUNCATE+COPY fallback).
					if (!hypha_oid.empty()) {
						StampHyphaTableComment(pg, pg_schema, pg_table, hypha_oid, db_name, a.schema_name, a.table_name,
						                       new_commit_id);
					}
					tables_synced++;
					rows_synced += table_rows;
					{
						const double dur_ms =
						    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - table_t0)
						        .count();
						std::string action, sym;
						if (tbl_is_new) {
							action = "new";
							sym = "+";
							tables_new++;
						} else if (tbl_is_tc) {
							action = "truncate_copy";
							sym = "\xe2\x86\xba"; // ↺
						} else {
							action = "schema_changed";
							sym = "~";
						}
						const std::string msg = "[hyphasync] " + sym + " " + tbl_label + "  " +
						                        StringUtil::Upper(action) + "  " + FormatRows(table_rows) + "  " +
						                        FormatDurMs(dur_ms);
						Printer::Print(OutputStream::STREAM_STDERR, msg);
						results.push_back({tbl_label, action, table_rows, dur_ms, "ok"});
					}
				}

			} catch (const std::exception &ex) {
				ok = false;
				err_msg = ex.what();
				PGresult *rb = PQexec(pg, ("ROLLBACK TO SAVEPOINT " + sp).c_str());
				if (rb) {
					PQclear(rb);
				}
				// Check whether the connection survived the COPY/DDL failure.
				if (PQstatus(pg) == CONNECTION_BAD) {
					PQreset(pg);
					if (PQstatus(pg) != CONNECTION_OK) {
						// Propagate to the outer catch — whole transaction is lost.
						throw IOException("connection lost during sync and could not be re-established: " + err_msg);
					}
					// Re-apply session settings and re-issue BEGIN so the remaining tables can proceed.
					if (fast_mode) {
						PQclear(PQexec(pg, "SET synchronous_commit = off"));
					}
					PQclear(PQexec(pg, "BEGIN"));
				}
				tables_unchanged++;
				LogAll(con, pg_log, db_name, "warn", "sync", "TABLE_SKIP",
				       "skipped " + a.schema_name + "." + a.table_name + ": " + err_msg);
				results.push_back(
				    {tbl_label, "error", 0,
				     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - table_t0).count(),
				     "error: " + err_msg});
			}
			(void)ok;
			// Only release the savepoint if the connection is still alive.
			if (PQstatus(pg) == CONNECTION_OK) {
				PGExec(pg, "RELEASE SAVEPOINT " + sp, "RELEASE SAVEPOINT");
			}
		}

		PGExec(pg, "COMMIT", "COMMIT transaction");

	} catch (...) {
		PGresult *rb = PQexec(pg, "ROLLBACK");
		if (rb)
			PQclear(rb);
		PQfinish(pg);
		if (pg_log) {
			PQfinish(pg_log);
		}
		throw;
	}

	// Remote hypha metadata — best-effort bookkeeping on the Postgres target.
	// Failures here must never abort an already-committed sync.
	try {
		WriteRemoteHyphaMetadata(con, pg, sync_commit_id, db_name, "sync", new_commit_id, old_commit_id,
		                         HYPHA_FINGERPRINT_ALGO, tables_synced, rows_synced);
	} catch (const std::exception &meta_ex) {
		LogAll(con, pg_log, db_name, "warn", "sync", "REMOTE_META_FAIL",
		       std::string("remote hypha metadata write failed: ") + meta_ex.what());
	}

	PQfinish(pg);

	// Mark as applied.
	Exec(con,
	     StringUtil::Format(R"(
INSERT INTO hypha.commit (commit_id, parent_commit_id, target_name, kind, message, applied_at, status, fingerprint_algo)
VALUES (%s, %s, 'default', 'sync',
        'sync: %s table(s) synced, %s dropped, %s unchanged',
        now(), 'applied', %s)
)",
	                        QuoteLiteral(sync_commit_id), QuoteLiteral(new_commit_id),
	                        std::to_string(tables_synced).c_str(), std::to_string(tables_dropped).c_str(),
	                        std::to_string(tables_unchanged).c_str(), QuoteLiteral(HYPHA_FINGERPRINT_ALGO)),
	     "INSERT sync commit");

	const std::string details = "{\"commit_id\":\"" + sync_commit_id +
	                            "\",\"tables_synced\":" + std::to_string(tables_synced) +
	                            ",\"tables_dropped\":" + std::to_string(tables_dropped) +
	                            ",\"rows_synced\":" + std::to_string(rows_synced) + "}";
	LogAll(con, pg_log, db_name, "info", "sync", "OK",
	       "sync complete: " + std::to_string(tables_synced) + " table(s) synced, " + std::to_string(tables_dropped) +
	           " dropped, " + std::to_string(tables_unchanged) + " unchanged",
	       details);

	if (pg_log) {
		PQfinish(pg_log);
	}

	// Progress summary and structured results for the streaming TableFunction.
	const double total_ms =
	    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sync_overall_start).count();
	if (tables_synced == 0 && tables_dropped == 0) {
		Printer::Print(OutputStream::STREAM_STDERR,
		               StringUtil::Format("[hyphasync] sync: no changes detected (%lld tables unchanged)",
		                                  (long long)tables_unchanged));
		results.push_back({"(all)", "unchanged", 0, total_ms,
		                   "no changes detected (" + std::to_string(tables_unchanged) + " tables unchanged)"});
	} else {
		int64_t cnt_new = 0, cnt_updated = 0, cnt_dropped = 0;
		for (const auto &r : results) {
			if (r.action == "new") {
				cnt_new++;
			} else if (r.action == "updated" || r.action == "schema_changed" || r.action == "truncate_copy" ||
			           r.action == "appended") {
				cnt_updated++;
			} else if (r.action == "dropped") {
				cnt_dropped++;
			}
		}
		Printer::Print(OutputStream::STREAM_STDERR,
		               StringUtil::Format("[hyphasync] sync: %lld new, %lld updated, %lld dropped — %lld rows in %s",
		                                  (long long)cnt_new, (long long)cnt_updated, (long long)cnt_dropped,
		                                  (long long)rows_synced, FormatDurMs(total_ms).c_str()));
	}
	return results;
}

} // namespace duckdb
