#include "hypha_fingerprint.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <sstream>

namespace duckdb {

namespace {

//! Compute sha256 of an arbitrary binary blob by hex-encoding it and using
//! DuckDB's sha256(unhex(...)). This avoids embedding binary chars in SQL literals.
std::string Sha256HexViaSql(Connection &con, const std::string &binary_payload) {
	std::string hex;
	hex.reserve(binary_payload.size() * 2);
	static const char digits[] = "0123456789abcdef";
	for (unsigned char c : binary_payload) {
		hex += digits[c >> 4];
		hex += digits[c & 0xf];
	}
	const auto sql = "SELECT sha256(unhex('" + hex + "'))";
	auto result = con.Query(sql);
	if (result->HasError() || result->RowCount() == 0 || result->GetValue(0, 0).IsNull()) {
		throw Exception(ExceptionType::EXECUTOR, "sha256 computation failed: " + result->GetError());
	}
	return result->GetValue(0, 0).ToString();
}

} // namespace

// ---------------------------------------------------------------------------
// field_encoding_expr(): SQL expression builder
// Returns a SQL fragment that evaluates to the canonical field encoding.
// Format: "tag(byte_length):payload" or "n():" for NULL (spec §4).
// ---------------------------------------------------------------------------

std::string FieldEncodingExpr(const std::string &col_expr, const std::string &duckdb_type) {
	const auto t = StringUtil::Upper(duckdb_type);

	// NULL-guarded wrapper: wraps any payload expression.
	// All v1 non-text payloads (integers, decimals, dates, timestamps, hex) are ASCII,
	// so length() == byte count. The VARCHAR branch uses a BLOB cast for true byte count.
	auto nullable = [&](const std::string &tag, const std::string &payload_sql) -> std::string {
		return "CASE WHEN (" + col_expr +
		       ") IS NULL THEN 'n():' ELSE "
		       "'" +
		       tag + "(' || length(CAST((" + payload_sql + ") AS VARCHAR)) || '):' || (" + payload_sql + ") END";
	};

	// BOOLEAN: tag 'b', payload '0' or '1'. Written inline to avoid the
	// double-evaluation that nullable() would cause for the boolean expression.
	if (t == "BOOLEAN" || t == "BOOL" || t == "LOGICAL") {
		return "CASE WHEN (" + col_expr +
		       ") IS NULL THEN 'n():'"
		       " WHEN (" +
		       col_expr +
		       ") THEN 'b(1):1'"
		       " ELSE 'b(1):0' END";
	}

	// Integer types: tag 'i', payload = decimal string (no leading zeros, sign if negative).
	// DuckDB CAST of any integer to VARCHAR gives the minimal decimal representation.
	if (t == "TINYINT" || t == "INT1" || t == "SMALLINT" || t == "INT2" || t == "SHORT" || t == "INTEGER" ||
	    t == "INT" || t == "INT4" || t == "SIGNED" || t == "BIGINT" || t == "INT8" || t == "LONG" || t == "UTINYINT" ||
	    t == "USMALLINT" || t == "UINTEGER" || t == "UBIGINT" || t == "HUGEINT") {
		return nullable("i", "CAST((" + col_expr + ") AS VARCHAR)");
	}

	// DECIMAL / NUMERIC: tag 'd', normalize to strip trailing fractional zeros.
	// DuckDB preserves declared scale in VARCHAR (e.g. DECIMAL(10,2) 1.50 → '1.50'),
	// so we must normalize: '1.50' → '1.5', '0.00' → '0', '-0' → '0'.
	// Bug guard: only strip trailing zeros when a decimal point is present — integer-scale
	// DECIMAL values have no decimal point and their trailing zeros are significant digits
	// (e.g. DECIMAL(5,0) 100 → '100', NOT '1').
	if (t == "DECIMAL" || t == "NUMERIC" || StringUtil::StartsWith(t, "DECIMAL(") ||
	    StringUtil::StartsWith(t, "NUMERIC(")) {
		const auto raw = "CAST((" + col_expr + ") AS VARCHAR)";
		// Step 1: strip trailing fractional zeros only when a '.' is present.
		// Step 2: strip trailing '.' if all fractional digits were stripped.
		// Step 3: normalize '-0' to '0'.
		const auto stripped = "CASE WHEN " + raw +
		                      " LIKE '%.%'"
		                      " THEN regexp_replace(regexp_replace(" +
		                      raw +
		                      ", '0+$', ''), '\\.$', '')"
		                      " ELSE " +
		                      raw + " END";
		const auto normalized = "CASE WHEN (" + stripped + ") = '-0' THEN '0' ELSE (" + stripped + ") END";
		return nullable("d", normalized);
	}

	// FLOAT / DOUBLE / REAL: tag 'f', shortest round-trippable representation.
	// Special values: NaN → 'nan', +Inf → 'inf', -Inf → '-inf', ±0.0 → '0'.
	// DuckDB's CAST of DOUBLE to VARCHAR gives a short decimal for normal values ('1.5').
	if (t == "FLOAT" || t == "FLOAT4" || t == "REAL" || t == "DOUBLE" || t == "FLOAT8" || t == "DOUBLE PRECISION") {
		const auto e = col_expr;
		const auto payload = "CASE WHEN isnan(" + e +
		                     ") THEN 'nan'"
		                     " WHEN isinf(" +
		                     e + ") AND (" + e +
		                     ") > 0 THEN 'inf'"
		                     " WHEN isinf(" +
		                     e + ") AND (" + e +
		                     ") < 0 THEN '-inf'"
		                     " WHEN (" +
		                     e +
		                     ") = 0 THEN '0'" // IEEE 754: -0.0 = +0.0, both → '0'
		                     " ELSE CAST((" +
		                     e + ") AS VARCHAR) END";
		return nullable("f", payload);
	}

	// VARCHAR / TEXT: tag 's', raw UTF-8 bytes.
	// NFC normalization deferred to v2 (see docs/fingerprinting.md §4.2).
	// Byte count: octet_length(CAST(x AS BLOB)) — DuckDB's octet_length() accepts BLOB
	// but not VARCHAR directly; casting to BLOB gives the UTF-8 byte representation.
	if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || t == "CHAR VARYING" || t == "CHARACTER VARYING" ||
	    StringUtil::StartsWith(t, "VARCHAR(") || StringUtil::StartsWith(t, "CHAR(") ||
	    StringUtil::StartsWith(t, "CHARACTER(")) {
		return "CASE WHEN (" + col_expr +
		       ") IS NULL THEN 'n():'"
		       " ELSE 's(' || octet_length(CAST((" +
		       col_expr + ") AS BLOB)) || '):' || (" + col_expr + ") END";
	}

	// BLOB / BYTEA: tag 'x', lowercase hex.
	if (t == "BLOB" || t == "BYTEA" || t == "BINARY" || t == "VARBINARY") {
		const auto payload = "lower(hex((" + col_expr + ")))";
		return nullable("x", payload);
	}

	// DATE: tag 'D', YYYY-MM-DD (ISO 8601).
	if (t == "DATE") {
		return nullable("D", "strftime((" + col_expr + "), '%Y-%m-%d')");
	}

	// TIME (no tz): tag 'T', HH:MM:SS.ffffff (microsecond precision, zero-padded).
	// epoch_us(TIME) gives microseconds since midnight; % 1000000 extracts the sub-second part.
	if (t == "TIME" || t == "TIME WITHOUT TIME ZONE") {
		const auto e = col_expr;
		const auto us_frac = "(epoch_us((" + e + ")) % 1000000)";
		const auto payload =
		    "strftime((" + e + "), '%H:%M:%S') || '.' || lpad(CAST(" + us_frac + " AS VARCHAR), 6, '0')";
		return nullable("T", payload);
	}

	// TIMETZ: tag 'T', normalize to UTC +00:00.
	if (t == "TIMETZ" || t == "TIME WITH TIME ZONE") {
		const auto utc = "(" + col_expr + " AT TIME ZONE 'UTC')";
		const auto us_frac = "(epoch_us(" + utc + ") % 1000000)";
		const auto payload =
		    "strftime(" + utc + ", '%H:%M:%S') || '.' || lpad(CAST(" + us_frac + " AS VARCHAR), 6, '0') || '+00:00'";
		return nullable("T", payload);
	}

	// TIMESTAMP (no tz): tag 't', YYYY-MM-DDThh:mm:ss.ffffff.
	if (t == "TIMESTAMP" || t == "DATETIME" || t == "TIMESTAMP WITHOUT TIME ZONE") {
		const auto e = col_expr;
		const auto us_frac = "(epoch_us((" + e + ")) % 1000000)";
		const auto payload =
		    "strftime((" + e + "), '%Y-%m-%dT%H:%M:%S') || '.' || lpad(CAST(" + us_frac + " AS VARCHAR), 6, '0')";
		return nullable("t", payload);
	}

	// TIMESTAMPTZ: tag 'z', normalize to UTC, append 'Z'.
	// DuckDB stores TIMESTAMPTZ internally as UTC epoch; epoch_us() gives the UTC moment
	// and is identical for the same instant expressed in any timezone.
	if (t == "TIMESTAMPTZ" || t == "TIMESTAMP WITH TIME ZONE") {
		const auto e = col_expr;
		// epoch_us gives the UTC epoch; reconstruct ISO 8601 from epoch microseconds.
		const auto us_total = "epoch_us((" + e + "))";
		const auto us_frac = "(" + us_total + " % 1000000)";
		// make_timestamp converts epoch microseconds back to a naive TIMESTAMP for strftime.
		const auto utc_ts = "make_timestamp(" + us_total + ")";
		const auto payload = "strftime(" + utc_ts +
		                     ", '%Y-%m-%dT%H:%M:%S') || '.' || "
		                     "lpad(CAST(" +
		                     us_frac + " AS VARCHAR), 6, '0') || 'Z'";
		return nullable("z", payload);
	}

	// UUID: tag 'u', canonical lowercase 8-4-4-4-12.
	if (t == "UUID") {
		return nullable("u", "lower(CAST((" + col_expr + ") AS VARCHAR))");
	}

	// ENUM: tag 'e', label text.
	if (StringUtil::StartsWith(t, "ENUM")) {
		return nullable("e", "CAST((" + col_expr + ") AS VARCHAR)");
	}

	// INTERVAL: tag 'v', canonical component form "MmDdUs" to avoid ambiguity
	// between representations like '1 month' vs '30 days' (they are NOT equal in SQL).
	if (t == "INTERVAL") {
		const auto e = col_expr;
		const auto payload = "CAST(datepart('month',       (" + e +
		                     ")) AS VARCHAR) || 'm' ||"
		                     " CAST(datepart('day',         (" +
		                     e +
		                     ")) AS VARCHAR) || 'd' ||"
		                     " CAST(datepart('microsecond', (" +
		                     e + ")) AS VARCHAR) || 'us'";
		return nullable("v", payload);
	}

	// BIT / BITSTRING: tag 'B', '0101...' string.
	if (t == "BIT" || t == "BITSTRING") {
		return nullable("B", "CAST((" + col_expr + ") AS VARCHAR)");
	}

	// Nested / unsupported: throw rather than silently skip (spec §9, §4.3).
	throw NotImplementedException(
	    "fingerprinting of DuckDB type '%s' is not yet supported in v1 (column expression: %s). "
	    "Nested types (LIST, STRUCT, MAP) are reserved for v2. See docs/fingerprinting.md §4.3.",
	    duckdb_type, col_expr);
}

// ---------------------------------------------------------------------------
// RowHashExpr(): assemble all column encodings into a row hash SQL expression
// ---------------------------------------------------------------------------

std::string RowHashExpr(const std::vector<std::pair<std::string, std::string>> &cols) {
	if (cols.empty()) {
		throw InvalidInputException("cannot compute row hash for a table with no hashable columns");
	}

	// Join field encodings with chr(31) (ASCII unit separator, unlikely in data).
	// Length prefixes already make concatenation injective; the separator aids readability.
	std::string concat;
	for (size_t i = 0; i < cols.size(); i++) {
		if (i > 0) {
			concat += " || chr(31) || ";
		}
		concat += "(" + FieldEncodingExpr(cols[i].first, cols[i].second) + ")";
	}

	// sha256() returns VARCHAR lowercase hex in DuckDB.
	return "sha256(" + concat + ")";
}

// ---------------------------------------------------------------------------
// ComputeTableFingerprint(): full table scan → table_hash + row_count
// ---------------------------------------------------------------------------

TableFingerprint ComputeTableFingerprint(Connection &con, const std::string &schema_name, const std::string &table_name,
                                         const std::vector<std::pair<std::string, std::string>> &cols) {
	TableFingerprint fp;

	if (cols.empty()) {
		return fp;
	}

	const auto row_hash_expr = RowHashExpr(cols);

	// Order-independent aggregate: sort row hashes before combining (spec §6.1).
	// Sort (not XOR) preserves duplicate rows correctly.
	// sha256 of empty table → NULL string_agg → empty table_hash.
	const auto sql = StringUtil::Format(R"(
SELECT
    sha256(string_agg(rh, chr(10) ORDER BY rh)) AS tbl_hash,
    COUNT(*) AS row_count
FROM (
    SELECT %s AS rh
    FROM "%s"."%s"
) __rows
)",
	                                    row_hash_expr, schema_name, table_name);

	auto result = con.Query(sql);
	if (result->HasError()) {
		throw Exception(ExceptionType::EXECUTOR, StringUtil::Format("table fingerprint failed for \"%s\".\"%s\": %s",
		                                                            schema_name, table_name, result->GetError()));
	}

	fp.row_count = result->GetValue(1, 0).GetValue<int64_t>();
	if (!result->GetValue(0, 0).IsNull()) {
		fp.table_hash = result->GetValue(0, 0).ToString();
	}
	return fp;
}

// ---------------------------------------------------------------------------
// ComputeDefinitionHash(): sha256 of normalized column metadata (no row scan)
// Columns tuple: (column_name, ordinal, duckdb_type, postgres_type, is_nullable, default_expr)
// ---------------------------------------------------------------------------

std::string ComputeDefinitionHash(
    Connection &con, const std::string &schema_name, const std::string &object_name, const std::string &object_type,
    const std::vector<std::tuple<std::string, int, std::string, std::string, bool, std::string>> &columns) {
	// Build the raw canonical payload with 0x1F separators (spec §6.2).
	std::ostringstream buf;
	const char sep = '\x1f';
	buf << schema_name << sep << object_name << sep << object_type << sep;
	for (const auto &col : columns) {
		buf << std::get<0>(col) << sep << std::to_string(std::get<1>(col)) << sep << std::get<2>(col) << sep
		    << std::get<3>(col) << sep << (std::get<4>(col) ? "1" : "0") << sep << std::get<5>(col) << sep;
	}
	// Hash the binary payload via DuckDB's sha256(unhex(...)).
	return Sha256HexViaSql(con, buf.str());
}

// ---------------------------------------------------------------------------
// ComputeObjectFingerprint(): sha256( definition_hash ":" content_hash )
// ---------------------------------------------------------------------------

std::string ComputeObjectFingerprint(Connection &con, const std::string &definition_hash,
                                     const std::string &content_hash) {
	if (definition_hash.empty() || content_hash.empty()) {
		return "";
	}
	// Both are already 64-char hex strings; their concatenation is ASCII-safe for SQL.
	return Sha256HexViaSql(con, definition_hash + ":" + content_hash);
}

} // namespace duckdb
