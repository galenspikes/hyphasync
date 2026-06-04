#pragma once

#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "hypha_postgres.hpp"

#include <libpq-fe.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace duckdb {

//! Column metadata used for DDL generation and chunk-size estimation.
struct ColumnDef {
	std::string name;
	std::string postgres_type;
	std::string duckdb_type;
	bool is_nullable;
	bool needs_json_cast = false; // true for STRUCT/MAP columns that require ::JSON cast in COPY
};

struct TableInfo {
	std::string schema_name;
	std::string table_name;
};

//! Target chunk sizes for memory-bounded COPY and row_hash loops.
static constexpr int64_t COPY_TARGET_BYTES = 64LL * 1024 * 1024; // 64 MB for COPY-to-Postgres
static constexpr int64_t HASH_TARGET_BYTES = 16LL * 1024 * 1024; // 16 MB for row_hash inserts

// Postgres max identifier length (NAMEDATALEN - 1).
static constexpr size_t PG_MAX_IDENT = 63;

unique_ptr<MaterializedQueryResult> Exec(Connection &con, const std::string &sql, const char *context);
int64_t CopyChunkViaPipe(Connection &con, PGconn *pg, const std::string &select_sql, const char *context);
std::string DuckTypeToPostgres(const std::string &raw);
bool IsIntegerDuckType(const std::string &duckdb_type);
int64_t EstimateCSVBytesPerRow(const std::vector<ColumnDef> &cols,
                               const std::unordered_map<std::string, int64_t> &varchar_widths);
std::unordered_map<std::string, int64_t> QueryVarcharWidths(Connection &con, const std::string &schema_name,
                                                            const std::string &table_name,
                                                            const std::vector<ColumnDef> &cols);
int64_t ComputeChunkRows(const std::vector<ColumnDef> &cols, int64_t target_chunk_bytes,
                         const std::unordered_map<std::string, int64_t> &varchar_widths);
std::string TruncPGIdent(const std::string &name);
std::string SafeTruncateIdent(const std::string &name, std::unordered_set<std::string> &used_names,
                              size_t max_len = PG_MAX_IDENT);

std::string SanitizeIdent(const std::string &name);
std::string MakePostgresSchemaName(const std::string &db_name, const std::string &duckdb_schema);
std::string TrimPQ(const char *value);
std::vector<std::string> ResolveColumnNames(const std::vector<ColumnDef> &cols);
bool NeedsExternalStorage(const std::string &pg_type);
int64_t EstimateFixedWidthRowBytes(const std::vector<ColumnDef> &cols);
bool NeedsJsonCastForCopy(const std::string &duckdb_type);
std::string BuildCopySelectList(const std::string &duckdb_schema, const std::string &table_name,
                                const std::vector<ColumnDef> &cols, const std::string &where_clause = "");
std::string BuildCreateTableDDL(const std::string &pg_schema, const std::string &pg_table_name,
                                const std::vector<ColumnDef> &cols, const std::vector<std::string> &pg_names,
                                bool suppress_not_null = false);
void PGExec(PGconn *pg, const std::string &sql, const char *context);

void EnsureRemoteHyphaMetadata(PGconn *pg);
void EnsureRemoteEventLog(PGconn *pg);
void LogAll(Connection &con, PGconn *pg_log, const std::string &db_name, const std::string &level,
            const std::string &operation, const std::string &code, const std::string &message,
            const std::string &details_json = "");
void WriteRemoteHyphaMetadata(Connection &con, PGconn *pg, const std::string &sync_id,
                              const std::string &source_database, const std::string &operation_kind,
                              const std::string &snapshot_commit_id, const std::string &parent_commit_id,
                              const std::string &fingerprint_algo, int64_t tables_synced, int64_t rows_synced,
                              int64_t tables_failed = 0, int64_t rows_failed = 0);
std::string JsonEscapePg(const std::string &s);
void StampHyphaTableComment(PGconn *pg, const std::string &pg_schema, const std::string &pg_table,
                            const std::string &hypha_object_id, const std::string &source_database,
                            const std::string &source_schema, const std::string &source_table,
                            const std::string &commit_id);
void StampHyphaSchemaComment(PGconn *pg, const std::string &pg_schema, const std::string &source_database,
                             const std::string &source_schema, const std::string &commit_id);
std::string GetHyphaObjectId(Connection &con, const std::string &schema_name, const std::string &table_name,
                             const std::string &commit_id);

//! RAII guard: saves the current DuckDB memory limit, applies a new one, and restores on destruction.
class MemoryLimitGuard {
	Connection &con_;
	idx_t original_limit_;

public:
	MemoryLimitGuard(Connection &con, idx_t new_limit);
	~MemoryLimitGuard();
};

idx_t ComputeSyncMemoryLimit(Connection &con, int64_t db_size_bytes);

struct ColumnDelta {
	std::vector<ColumnDef> added;
	std::vector<std::string> dropped;
	bool has_type_changes = false;
	bool has_reordering = false;
};

ColumnDelta ComputeColumnDelta(Connection &con, const std::string &old_commit_id, const std::string &new_commit_id,
                               const std::string &schema_name, const std::string &table_name);

struct TableAction {
	enum class Kind { NEW, DROPPED, SCHEMA_CHANGED, ROWS_CHANGED, LIKELY_UNCHANGED };
	Kind kind;
	std::string schema_name;
	std::string table_name;
	int64_t old_rows = 0;
	int64_t new_rows = 0;
	std::string detail;
};

std::string ActionLabel(TableAction::Kind k);
std::vector<TableAction> ComputeDiff(Connection &con, const std::string &old_commit_id,
                                     const std::string &new_commit_id);
std::vector<std::string> SplitPkKey(const std::string &pk_json);
std::string BuildPkFilter(const std::vector<std::string> &pk_cols, const std::vector<std::string> &pk_vals,
                          const std::string &cast_type);

struct RowDiff {
	int64_t inserts = 0;
	int64_t updates = 0;
	int64_t deletes = 0;
};

bool ApplyRowLevelDiff(Connection &con, PGconn *pg, const std::string &schema_name, const std::string &table_name,
                       const std::string &pg_schema, const std::string &pg_table, const std::string &old_commit_id,
                       const std::string &new_commit_id, const std::vector<ColumnDef> &cols, RowDiff &diff_out);

//! Append-only fast path for keyless (no-PK) tables. When the new row-hash multiset is a
//! superset of the old (rows were only inserted), COPY just the new rows to Postgres with no
//! TRUNCATE and no DELETE. Returns false — leaving the caller to TRUNCATE+COPY — when the table
//! has a PK, lacks row hashes for either commit, has any removed/updated rows, or fails an
//! internal consistency guard. On success diff_out.inserts is set. Never produces duplicates:
//! a mismatch between the expected and actual insert counts forces the safe fallback.
bool ApplyKeylessAppendDiff(Connection &con, PGconn *pg, const std::string &schema_name, const std::string &table_name,
                            const std::string &pg_schema, const std::string &pg_table, const std::string &old_commit_id,
                            const std::string &new_commit_id, const std::vector<ColumnDef> &cols, RowDiff &diff_out);

struct SyncTableResult {
	std::string table_name;
	std::string action;
	int64_t rows_synced;
	double duration_ms;
	std::string status;
};

std::vector<SyncTableResult> RunSync(Connection &con);

} // namespace duckdb
