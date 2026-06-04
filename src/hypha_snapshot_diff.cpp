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

// ---------------------------------------------------------------------------
// ColumnDelta: describes the structural difference between two column snapshots
// ---------------------------------------------------------------------------

//! Computes a structural column diff between two commits for a single table.
//! Returns a ColumnDelta describing additions, drops, type changes, and reorderings.
ColumnDelta ComputeColumnDelta(Connection &con, const std::string &old_commit_id, const std::string &new_commit_id,
                               const std::string &schema_name, const std::string &table_name) {
	ColumnDelta delta;

	const auto old_result =
	    Exec(con,
	         StringUtil::Format("SELECT column_name, postgres_type, is_nullable, duckdb_type "
	                            "FROM hypha.column_snapshot "
	                            "WHERE commit_id = %s AND schema_name = %s AND table_name = %s "
	                            "ORDER BY ordinal_position",
	                            QuoteLiteral(old_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)),
	         "get old columns for delta");

	const auto new_result =
	    Exec(con,
	         StringUtil::Format("SELECT column_name, postgres_type, is_nullable, duckdb_type "
	                            "FROM hypha.column_snapshot "
	                            "WHERE commit_id = %s AND schema_name = %s AND table_name = %s "
	                            "ORDER BY ordinal_position",
	                            QuoteLiteral(new_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)),
	         "get new columns for delta");

	// Build lookup maps (exclude unsupported columns from both sides).
	std::unordered_map<std::string, std::pair<std::string, bool>> old_map; // name -> (pg_type, nullable)
	for (idx_t i = 0; i < old_result->RowCount(); i++) {
		const auto name = old_result->GetValue(0, i).ToString();
		const auto pg_type = old_result->GetValue(1, i).ToString();
		if (StringUtil::StartsWith(pg_type, "(unsupported")) {
			continue;
		}
		old_map[name] = {pg_type, old_result->GetValue(2, i).GetValue<bool>()};
	}
	std::unordered_map<std::string, std::pair<std::string, bool>> new_map; // name -> (pg_type, nullable)
	for (idx_t i = 0; i < new_result->RowCount(); i++) {
		const auto name = new_result->GetValue(0, i).ToString();
		const auto pg_type = new_result->GetValue(1, i).ToString();
		if (StringUtil::StartsWith(pg_type, "(unsupported")) {
			continue;
		}
		new_map[name] = {pg_type, new_result->GetValue(2, i).GetValue<bool>()};
	}

	// Added columns: in new but not old.
	for (idx_t i = 0; i < new_result->RowCount(); i++) {
		const auto name = new_result->GetValue(0, i).ToString();
		const auto pg_type = new_result->GetValue(1, i).ToString();
		if (StringUtil::StartsWith(pg_type, "(unsupported")) {
			continue;
		}
		if (old_map.count(name) == 0) {
			ColumnDef cd;
			cd.name = name;
			cd.postgres_type = pg_type;
			cd.is_nullable = new_result->GetValue(2, i).GetValue<bool>();
			cd.duckdb_type = new_result->GetValue(3, i).ToString();
			cd.needs_json_cast = NeedsJsonCastForCopy(cd.duckdb_type);
			delta.added.push_back(cd);
		}
	}

	// Dropped columns: in old but not new.
	for (idx_t i = 0; i < old_result->RowCount(); i++) {
		const auto name = old_result->GetValue(0, i).ToString();
		if (StringUtil::StartsWith(old_result->GetValue(1, i).ToString(), "(unsupported")) {
			continue;
		}
		if (new_map.count(name) == 0) {
			delta.dropped.push_back(name);
		}
	}

	// Type changes: surviving columns with changed postgres_type or nullability.
	for (const auto &kv : new_map) {
		auto it = old_map.find(kv.first);
		if (it == old_map.end()) {
			continue; // added column
		}
		if (kv.second.first != it->second.first || kv.second.second != it->second.second) {
			delta.has_type_changes = true;
			break;
		}
	}

	// Relative reordering: compare the ordered sequence of surviving column names.
	std::vector<std::string> old_order, new_order;
	for (idx_t i = 0; i < old_result->RowCount(); i++) {
		const auto name = old_result->GetValue(0, i).ToString();
		if (StringUtil::StartsWith(old_result->GetValue(1, i).ToString(), "(unsupported")) {
			continue;
		}
		if (new_map.count(name) > 0) {
			old_order.push_back(name);
		}
	}
	for (idx_t i = 0; i < new_result->RowCount(); i++) {
		const auto name = new_result->GetValue(0, i).ToString();
		if (StringUtil::StartsWith(new_result->GetValue(1, i).ToString(), "(unsupported")) {
			continue;
		}
		if (old_map.count(name) > 0) {
			new_order.push_back(name);
		}
	}
	if (old_order != new_order) {
		delta.has_reordering = true;
	}

	return delta;
}

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

// ---------------------------------------------------------------------------
// ApplyRowLevelDiff(): targeted INSERT/UPDATE/DELETE for ROWS_CHANGED tables.
// Works for any PK (single-column or composite). Falls back to TRUNCATE+COPY
// for no-PK tables or tables without row hashes from a prior snapshot.
// ---------------------------------------------------------------------------

//! Split a chr(31)-separated compound pk_json key into individual column values.
std::vector<std::string> SplitPkKey(const std::string &pk_json) {
	std::vector<std::string> parts;
	std::string cur;
	for (char c : pk_json) {
		if (c == '\x1f') {
			parts.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	parts.push_back(cur);
	return parts;
}

//! Build a WHERE filter expression for a set of pk_json compound keys.
//! For single-column PKs: col IN ('v1','v2')
//! For composite PKs:     (col1 = 'v1a' AND col2 = 'v1b') OR (col1 = 'v2a' AND col2 = 'v2b')
//! cast_type: "TEXT" for Postgres, "VARCHAR" for DuckDB.
std::string BuildPkFilter(const std::vector<std::string> &pk_cols, const std::vector<std::string> &pk_vals,
                          const std::string &cast_type) {
	if (pk_vals.empty()) {
		return "FALSE";
	}
	if (pk_cols.size() == 1) {
		// Single PK: compact IN clause.
		std::string list;
		for (const auto &v : pk_vals) {
			if (!list.empty())
				list += ",";
			list += "'" + StringUtil::Replace(v, "'", "''") + "'";
		}
		return "CAST(" + QuoteIdent(pk_cols[0]) + " AS " + cast_type + ") IN (" + list + ")";
	}
	// Composite PK: OR of AND conditions.
	std::string conds;
	for (const auto &pk_val : pk_vals) {
		const auto parts = SplitPkKey(pk_val);
		if (parts.size() != pk_cols.size()) {
			continue; // malformed — skip; safety net handles this
		}
		if (!conds.empty())
			conds += " OR ";
		conds += "(";
		for (size_t i = 0; i < pk_cols.size(); i++) {
			if (i > 0)
				conds += " AND ";
			conds += "CAST(" + QuoteIdent(pk_cols[i]) + " AS " + cast_type + ") = '" +
			         StringUtil::Replace(parts[i], "'", "''") + "'";
		}
		conds += ")";
	}
	return conds.empty() ? "FALSE" : conds;
}

bool ApplyRowLevelDiff(Connection &con, PGconn *pg, const std::string &schema_name, const std::string &table_name,
                       const std::string &pg_schema, const std::string &pg_table, const std::string &old_commit_id,
                       const std::string &new_commit_id, const std::vector<ColumnDef> &cols, RowDiff &diff_out) {
	// Get PK columns from the new snapshot's object_snapshot.
	const auto pk_result =
	    Exec(con,
	         StringUtil::Format(R"(
SELECT COALESCE(pk_columns, '') FROM hypha.object_snapshot
WHERE commit_id = %s AND schema_name = %s AND object_name = %s LIMIT 1
)",
	                            QuoteLiteral(new_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)),
	         "get pk_columns");
	if (pk_result->RowCount() == 0 || pk_result->GetValue(0, 0).IsNull()) {
		return false;
	}
	const auto pk_columns = pk_result->GetValue(0, 0).ToString();
	if (pk_columns.empty()) {
		return false; // No PK — fall back to TRUNCATE+COPY
	}

	// Parse and sort PK column names (must match the order used when building pk_json).
	std::vector<std::string> pk_cols;
	{
		std::string rem = pk_columns;
		while (!rem.empty()) {
			const auto c = rem.find(',');
			pk_cols.push_back(c == std::string::npos ? rem : rem.substr(0, c));
			rem = c == std::string::npos ? "" : rem.substr(c + 1);
		}
	}
	std::sort(pk_cols.begin(), pk_cols.end());

	// Require row hashes for both commits.
	const auto has_old =
	    Exec(con,
	         StringUtil::Format(
	             "SELECT 1 FROM hypha.row_hash WHERE commit_id=%s AND schema_name=%s AND table_name=%s LIMIT 1",
	             QuoteLiteral(old_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)),
	         "check old hashes");
	const auto has_new =
	    Exec(con,
	         StringUtil::Format(
	             "SELECT 1 FROM hypha.row_hash WHERE commit_id=%s AND schema_name=%s AND table_name=%s LIMIT 1",
	             QuoteLiteral(new_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)),
	         "check new hashes");
	if (has_old->RowCount() == 0 || has_new->RowCount() == 0) {
		return false;
	}

	// Build a DuckDB temp table with the full delta — avoids loading all PKs into C++ RAM.
	// The temp table holds op ∈ {'insert','update','delete'} and pk_json for each changed row.
	// We then paginate over it in chunks of PK_CHUNK_SIZE rather than materializing vectors.
	static std::atomic<int64_t> s_delta_seq {0};
	const std::string delta_id = std::to_string(++s_delta_seq);
	const std::string tmp_tbl = "__hypha_delta_" + delta_id;

	const std::string new_h_sql = StringUtil::Format(
	    "(SELECT pk_json, row_hash FROM hypha.row_hash WHERE commit_id=%s AND schema_name=%s AND table_name=%s)",
	    QuoteLiteral(new_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name));
	const std::string old_h_sql = StringUtil::Format(
	    "(SELECT pk_json, row_hash FROM hypha.row_hash WHERE commit_id=%s AND schema_name=%s AND table_name=%s)",
	    QuoteLiteral(old_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name));

	const std::string create_delta = StringUtil::Format(
	    "CREATE TEMP TABLE %s AS "
	    "SELECT 'insert' AS op, pk_json FROM %s AS new_h WHERE pk_json NOT IN (SELECT pk_json FROM %s) "
	    "UNION ALL "
	    "SELECT 'delete',        pk_json FROM %s AS old_h WHERE pk_json NOT IN (SELECT pk_json FROM %s) "
	    "UNION ALL "
	    "SELECT 'update',        n.pk_json FROM %s AS n JOIN %s AS o USING (pk_json) WHERE n.row_hash != o.row_hash",
	    QuoteIdent(tmp_tbl), new_h_sql, old_h_sql, old_h_sql, new_h_sql, new_h_sql, old_h_sql);

	const auto cr_res = con.Query(create_delta);
	if (!cr_res || cr_res->HasError()) {
		return false; // fall back to TRUNCATE+COPY
	}

	// RAII: drop the temp table when we leave this scope (error or success).
	struct TempTableDrop {
		Connection &con;
		std::string name;
		~TempTableDrop() {
			con.Query("DROP TABLE IF EXISTS " + name);
		}
	} drop_guard {con, QuoteIdent(tmp_tbl)};

	// Get per-op counts for diff_out and detect empty delta.
	const auto counts_res =
	    Exec(con, StringUtil::Format("SELECT op, COUNT(*)::BIGINT FROM %s GROUP BY op", tmp_tbl), "delta op counts");
	for (idx_t r = 0; r < counts_res->RowCount(); r++) {
		const auto op = counts_res->GetValue(0, r).ToString();
		const auto cnt = counts_res->GetValue(1, r).GetValue<int64_t>();
		if (op == "insert")
			diff_out.inserts = cnt;
		else if (op == "update")
			diff_out.updates = cnt;
		else if (op == "delete")
			diff_out.deletes = cnt;
	}
	if (diff_out.inserts == 0 && diff_out.updates == 0 && diff_out.deletes == 0) {
		return true; // No rows changed
	}

	// Safety: if any non-empty rows have empty pk_json, the key is malformed — fall back.
	{
		const auto bad_res =
		    con.Query(StringUtil::Format("SELECT 1 FROM %s WHERE pk_json IS NULL OR pk_json = '' LIMIT 1", tmp_tbl));
		if (bad_res && !bad_res->HasError() && bad_res->RowCount() > 0) {
			return false;
		}
	}

	static constexpr int64_t PK_CHUNK_SIZE = 1000;

	// DELETE pass: page through 'delete' + 'update' rows from the temp table.
	{
		int64_t del_offset = 0;
		const int64_t del_total = diff_out.deletes + diff_out.updates;
		while (del_offset < del_total) {
			const auto chunk_res =
			    Exec(con,
			         StringUtil::Format("SELECT pk_json FROM %s WHERE op IN ('delete','update') "
			                            "LIMIT %s OFFSET %s",
			                            tmp_tbl, std::to_string(PK_CHUNK_SIZE), std::to_string(del_offset)),
			         "stream delete PKs");
			if (chunk_res->RowCount() == 0) {
				break;
			}
			std::vector<std::string> del_chunk;
			del_chunk.reserve(static_cast<size_t>(chunk_res->RowCount()));
			for (idx_t i = 0; i < chunk_res->RowCount(); i++) {
				del_chunk.push_back(chunk_res->GetValue(0, i).IsNull() ? "" : chunk_res->GetValue(0, i).ToString());
			}
			PGExec(pg,
			       "DELETE FROM " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " WHERE " +
			           BuildPkFilter(pk_cols, del_chunk, "TEXT"),
			       "row-level DELETE");
			del_offset += static_cast<int64_t>(del_chunk.size());
		}
	}

	// INSERT pass: page through 'insert' + 'update' rows from the temp table.
	{
		const std::string copy_sql =
		    "COPY " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " FROM STDIN CSV NULL '\\N'";
		int64_t ins_offset = 0;
		const int64_t ins_total = diff_out.inserts + diff_out.updates;
		while (ins_offset < ins_total) {
			const auto chunk_res =
			    Exec(con,
			         StringUtil::Format("SELECT pk_json FROM %s WHERE op IN ('insert','update') "
			                            "LIMIT %s OFFSET %s",
			                            tmp_tbl, std::to_string(PK_CHUNK_SIZE), std::to_string(ins_offset)),
			         "stream insert PKs");
			if (chunk_res->RowCount() == 0) {
				break;
			}
			std::vector<std::string> ins_chunk;
			ins_chunk.reserve(static_cast<size_t>(chunk_res->RowCount()));
			for (idx_t i = 0; i < chunk_res->RowCount(); i++) {
				ins_chunk.push_back(chunk_res->GetValue(0, i).IsNull() ? "" : chunk_res->GetValue(0, i).ToString());
			}
			const auto filter = BuildPkFilter(pk_cols, ins_chunk, "VARCHAR");

			PGresult *cr = PQexec(pg, copy_sql.c_str());
			if (!cr) {
				throw IOException("PQexec returned null starting row-level COPY (out of memory): " +
				                  TrimPQ(PQerrorMessage(pg)));
			}
			if (PQresultStatus(cr) != PGRES_COPY_IN) {
				const auto err = TrimPQ(PQresultErrorMessage(cr));
				PQclear(cr);
				throw IOException("row-level COPY IN failed: " + err);
			}
			PQclear(cr);

			CopyChunkViaPipe(con, pg, BuildCopySelectList(schema_name, table_name, cols, filter), "COPY delta chunk");

			if (PQputCopyEnd(pg, nullptr) != 1) {
				throw IOException("PQputCopyEnd failed");
			}
			PGresult *done = PQgetResult(pg);
			if (!done) {
				throw IOException("PQgetResult returned null after row-level COPY (out of memory): " +
				                  TrimPQ(PQerrorMessage(pg)));
			}
			if (PQresultStatus(done) != PGRES_COMMAND_OK) {
				const auto err = TrimPQ(PQresultErrorMessage(done));
				PQclear(done);
				throw IOException("row-level COPY failed: " + err);
			}
			PQclear(done);

			ins_offset += static_cast<int64_t>(ins_chunk.size());
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// ApplyKeylessAppendDiff(): append-only fast path for no-PK tables.
// When the new row-hash multiset is a superset of the old (rows were only inserted),
// COPY just the new rows — no TRUNCATE, no DELETE. Any removal/update, or any condition
// that risks duplicates, returns false so the caller does a full TRUNCATE+COPY.
// ---------------------------------------------------------------------------

bool ApplyKeylessAppendDiff(Connection &con, PGconn *pg, const std::string &schema_name,
                            const std::string &table_name, const std::string &pg_schema, const std::string &pg_table,
                            const std::string &old_commit_id, const std::string &new_commit_id,
                            const std::vector<ColumnDef> &cols, RowDiff &diff_out) {
	// Only keyless tables — keyed tables go through ApplyRowLevelDiff.
	const auto pk_result =
	    Exec(con,
	         StringUtil::Format("SELECT COALESCE(pk_columns, '') FROM hypha.object_snapshot "
	                            "WHERE commit_id = %s AND schema_name = %s AND object_name = %s LIMIT 1",
	                            QuoteLiteral(new_commit_id), QuoteLiteral(schema_name), QuoteLiteral(table_name)),
	         "keyless: get pk_columns");
	if (pk_result->RowCount() > 0 && !pk_result->GetValue(0, 0).IsNull() &&
	    !pk_result->GetValue(0, 0).ToString().empty()) {
		return false; // table has a PK
	}

	// Count stored row hashes for both commits. A baseline taken before keyless row_hash was
	// populated has none for the old commit → fall back.
	auto count_hashes = [&](const std::string &commit) -> int64_t {
		auto r = con.Query(StringUtil::Format(
		    "SELECT COUNT(*)::BIGINT FROM hypha.row_hash WHERE commit_id=%s AND schema_name=%s AND table_name=%s",
		    QuoteLiteral(commit), QuoteLiteral(schema_name), QuoteLiteral(table_name)));
		if (!r || r->HasError() || r->RowCount() == 0 || r->GetValue(0, 0).IsNull()) {
			return -1;
		}
		return r->GetValue(0, 0).GetValue<int64_t>();
	};
	const int64_t old_n = count_hashes(old_commit_id);
	const int64_t new_n = count_hashes(new_commit_id);
	if (old_n <= 0 || new_n < 0) {
		return false; // no old hashes (or empty old table) → simplest correct path is full re-copy
	}

	const std::string old_grp = StringUtil::Format("(SELECT row_hash, COUNT(*) AS n FROM hypha.row_hash "
	                                               "WHERE commit_id=%s AND schema_name=%s AND table_name=%s "
	                                               "GROUP BY row_hash)",
	                                               QuoteLiteral(old_commit_id), QuoteLiteral(schema_name),
	                                               QuoteLiteral(table_name));
	const std::string new_grp = StringUtil::Format("(SELECT row_hash, COUNT(*) AS n FROM hypha.row_hash "
	                                               "WHERE commit_id=%s AND schema_name=%s AND table_name=%s "
	                                               "GROUP BY row_hash)",
	                                               QuoteLiteral(new_commit_id), QuoteLiteral(schema_name),
	                                               QuoteLiteral(table_name));

	// Multiset subset check: if any hash occurs more in old than new, rows were removed or
	// updated. We cannot reconstruct removed content, so fall back to TRUNCATE+COPY.
	{
		auto rem = con.Query(
		    StringUtil::Format("SELECT EXISTS(SELECT 1 FROM %s o LEFT JOIN %s n USING(row_hash) "
		                       "WHERE o.n > COALESCE(n.n, 0))",
		                       old_grp, new_grp));
		if (!rem || rem->HasError() || rem->RowCount() == 0 || rem->GetValue(0, 0).IsNull() ||
		    rem->GetValue(0, 0).GetValue<bool>()) {
			return false; // removal/update detected (or check failed) → fallback
		}
	}

	// No removals → new ⊇ old; the multiset surplus is the number of rows to insert.
	const int64_t surplus_total = new_n - old_n;
	if (surplus_total <= 0) {
		diff_out.inserts = 0; // multisets identical — nothing to copy (defensive; unchanged tables
		return true;          // do not reach this path)
	}

	// New-rows filter: live rows whose content hash is absent from the old snapshot.
	std::vector<std::pair<std::string, std::string>> fp_cols;
	fp_cols.reserve(cols.size());
	for (const auto &cd : cols) {
		fp_cols.emplace_back(cd.name, cd.duckdb_type);
	}
	std::string row_hash_expr;
	try {
		row_hash_expr = RowHashExpr(fp_cols);
	} catch (...) {
		return false; // unhashable column types → fallback
	}
	const std::string where =
	    row_hash_expr + " NOT IN (SELECT row_hash FROM hypha.row_hash WHERE commit_id=" +
	    QuoteLiteral(old_commit_id) + " AND schema_name=" + QuoteLiteral(schema_name) +
	    " AND table_name=" + QuoteLiteral(table_name) + ")";

	// Consistency guard: the live NOT-IN count must equal the multiset surplus. A mismatch means
	// a recomputed hash disagrees with the stored one, or a duplicate row gained copies (which the
	// NOT IN filter cannot express). Either way, fall back to avoid inserting duplicates or
	// missing rows.
	{
		auto actual = con.Query("SELECT COUNT(*)::BIGINT FROM " + QuoteIdent(schema_name) + "." +
		                        QuoteIdent(table_name) + " WHERE " + where);
		if (!actual || actual->HasError() || actual->RowCount() == 0 || actual->GetValue(0, 0).IsNull() ||
		    actual->GetValue(0, 0).GetValue<int64_t>() != surplus_total) {
			return false;
		}
	}

	// Stream just the new rows to Postgres — no TRUNCATE, no DELETE.
	const std::string copy_sql =
	    "COPY " + QuoteIdent(pg_schema) + "." + QuoteIdent(pg_table) + " FROM STDIN CSV NULL '\\N'";
	PGresult *cr = PQexec(pg, copy_sql.c_str());
	if (!cr || PQresultStatus(cr) != PGRES_COPY_IN) {
		const auto err = cr ? TrimPQ(PQresultErrorMessage(cr)) : TrimPQ(PQerrorMessage(pg));
		if (cr) {
			PQclear(cr);
		}
		throw IOException("keyless append COPY IN failed: " + err);
	}
	PQclear(cr);

	CopyChunkViaPipe(con, pg, BuildCopySelectList(schema_name, table_name, cols, where), "keyless append COPY");

	if (PQputCopyEnd(pg, nullptr) != 1) {
		throw IOException("keyless append PQputCopyEnd failed");
	}
	PGresult *done = PQgetResult(pg);
	if (!done || PQresultStatus(done) != PGRES_COMMAND_OK) {
		const auto err = done ? TrimPQ(PQresultErrorMessage(done)) : TrimPQ(PQerrorMessage(pg));
		if (done) {
			PQclear(done);
		}
		throw IOException("keyless append COPY failed: " + err);
	}
	PQclear(done);

	diff_out.inserts = surplus_total;
	return true;
}

} // namespace duckdb
