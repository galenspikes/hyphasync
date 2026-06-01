#pragma once

#include "duckdb/main/connection.hpp"
#include <string>
#include <vector>

namespace duckdb {

//! Canonical field encoding spec: docs/fingerprinting.md §4
//! Given a SQL column expression and its DuckDB type string, returns a SQL expression
//! that produces the canonical field encoding: "tag(len):payload" or "n():" for NULL.
//! Throws NotImplementedException for unsupported nested types (LIST/STRUCT/MAP).
std::string FieldEncodingExpr(const std::string &col_expr, const std::string &duckdb_type);

//! Returns a SQL expression that computes the row hash for one row of a table.
//! col_exprs[i] pairs the SQL column expression with its DuckDB type.
//! Row hash = sha256( join_0x1F( field_encoding(col) for each col in order ) )
std::string RowHashExpr(const std::vector<std::pair<std::string, std::string>> &cols);

// ---------------------------------------------------------------------------
// Pluggable fingerprint strategy (v3 two-layer classifier)
//
// Layer 1 — Structural classifier (fast, schema-driven):
//   APPEND_ONLY       integer PK / monotonic timestamp column → COUNT + MAX(signal_col)
//   EXACT             estimated table bytes < 1 MB → full per-row sha256 (cheap, most precise)
//   MUTABLE_ENTITY    default for large tables → COUNT + MIN/MAX/SUM of internal rowid
//   WIDE_ANALYTICAL   >50 columns → COUNT + MIN/MAX of first 3 simple scalar columns
//
// Layer 2 — Domain-semantic strategies (pluggable, highest priority):
//   CHEMINFORMATICS_COMPOUNDS  smiles/inchi/mol column → COUNT + MIN/MAX(mol_col)
//   CHEMINFORMATICS_ASSAY      ic50/ec50/ki/activity column → COUNT + MIN/MAX/AVG(assay_col)
// ---------------------------------------------------------------------------

//! A strategy for computing the fingerprint of a specific table instance.
//! Encapsulates the algorithm name, ready-to-run SQL, and human-readable rationale
//! (logged to event_log and stored in hypha.table_snapshot.fingerprint_strategy).
struct FingerprintStrategy {
	std::string name;      //!< "APPEND_ONLY" | "EXACT" | "MUTABLE_ENTITY" | "WIDE_ANALYTICAL"
	                       //!< "CHEMINFORMATICS_COMPOUNDS" | "CHEMINFORMATICS_ASSAY"
	std::string sql;       //!< Ready-to-run SQL returning (table_hash VARCHAR, row_count BIGINT)
	std::string rationale; //!< Why this strategy was chosen (for logging and observability)
};

//! Classify a table and return the fingerprint strategy to use.
//! Classification priority (highest first):
//!   1. Domain signals  — cheminformatics column name patterns (Layer 2)
//!   2. Small cost      — estimated bytes < 1 MB → EXACT (full per-row hash, cheap)
//!   3. Append signal   — integer PK / monotonic timestamp → APPEND_ONLY
//!   4. Wide table      — >50 columns → WIDE_ANALYTICAL
//!   5. Default         — MUTABLE_ENTITY (rowid statistics)
//!
//! is_base_snapshot is accepted for API compatibility but does not alter classification;
//! EXACT and APPEND_ONLY are both O(1) or O(small) and are safe at snapshot time.
FingerprintStrategy ClassifyTable(Connection &con, const std::string &schema_name, const std::string &table_name,
                                  const std::vector<std::pair<std::string, std::string>> &cols,
                                  bool is_base_snapshot = false);

//! Result of ComputeTableFingerprint: hash, row count, and the chosen strategy.
struct TableFingerprint {
	std::string table_hash; //!< 64-char SHA-256 hex, or "" if table is empty
	int64_t row_count = 0;
	std::string strategy_name;      //!< Strategy used (stored in hypha.table_snapshot)
	std::string strategy_rationale; //!< Why this strategy was chosen (logged to event_log)
};

//! Classify this table, execute the chosen strategy SQL, and return the fingerprint.
//! Falls back to MUTABLE_ENTITY if the chosen strategy SQL fails at runtime.
//! Throws only for unrecoverable failures (MUTABLE_ENTITY fallback also fails).
//! is_base_snapshot is accepted for API compatibility; classification is identical for both modes.
TableFingerprint ComputeTableFingerprint(Connection &con, const std::string &schema_name, const std::string &table_name,
                                         const std::vector<std::pair<std::string, std::string>> &cols,
                                         bool is_base_snapshot = false);

//! Computes the definition_hash for a table from its column_snapshot rows.
//! definition_hash = sha256( join_0x1F( schema, object, type, col metadata... ) )
//! Uses DuckDB's sha256(unhex()) to hash the binary payload (which contains 0x1F separators).
std::string ComputeDefinitionHash(
    Connection &con, const std::string &schema_name, const std::string &object_name, const std::string &object_type,
    const std::vector<std::tuple<std::string, int, std::string, std::string, bool, std::string>> &columns);

//! Rolls up definition_hash and content_hash into the object fingerprint.
//! object_fingerprint = sha256( definition_hash + ":" + content_hash )
std::string ComputeObjectFingerprint(Connection &con, const std::string &definition_hash,
                                     const std::string &content_hash);

} // namespace duckdb
