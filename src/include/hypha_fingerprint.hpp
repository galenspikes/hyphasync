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

//! Computes the table_hash for schema.table and returns it as a 64-char hex string.
//! table_hash = sha256( string_agg(row_hash, chr(10) ORDER BY row_hash) )
//! Also returns the exact row_count as a side-effect.
//! Throws for any column type that cannot be canonicalized.
struct TableFingerprint {
	std::string table_hash; //!< 64-char SHA-256 hex, or "" if table is empty
	int64_t row_count;
};
TableFingerprint ComputeTableFingerprint(Connection &con, const std::string &schema_name, const std::string &table_name,
                                         const std::vector<std::pair<std::string, std::string>> &cols);

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
