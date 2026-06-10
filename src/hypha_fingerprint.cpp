#include "hypha_fingerprint.hpp"

#include "hypha_postgres.hpp"

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

//! Shared SQL emission for the nested/document tags (L, R, M, J).
//! All four use DuckDB's ::JSON::VARCHAR serialization as a length-prefixed,
//! NULL-guarded payload. Factored out so the four branches stay byte-identical.
//!
//! Byte count: strlen() returns the UTF-8 byte length of the serialized JSON text,
//! correctly handling multibyte characters (e.g. 'é' = 2 bytes, '—' = 3 bytes). We must
//! NOT use CAST(... AS BLOB) for the byte count: DuckDB rejects the VARCHAR→BLOB cast for
//! any string containing a non-ASCII byte ("Invalid byte encountered in STRING -> BLOB
//! conversion"), so JSON holding accented/Unicode text would fail to hash. strlen() and
//! the old octet_length(CAST(... AS BLOB)) agree exactly on ASCII payloads, so existing
//! ASCII fingerprints are unchanged; non-ASCII JSON simply hashes instead of erroring.
std::string NestedJsonEncoding(const std::string &col_expr, char tag) {
	const auto jp = "CAST((" + col_expr + ")::JSON AS VARCHAR)";
	return std::string("CASE WHEN (") + col_expr +
	       ") IS NULL THEN 'n():'"
	       " ELSE '" +
	       tag + "(' || strlen((" + jp + ")) || '):' || (" + jp + ") END";
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
	// NFC normalization deferred to a future version.
	// Byte count: strlen(col) returns the UTF-8 byte length for VARCHAR strings, correctly
	// handling multi-byte characters (e.g. em dash = 3 bytes). We avoid CAST(col AS BLOB)
	// which DuckDB rejects for strings containing non-ASCII bytes.
	if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || t == "CHAR VARYING" || t == "CHARACTER VARYING" ||
	    StringUtil::StartsWith(t, "VARCHAR(") || StringUtil::StartsWith(t, "CHAR(") ||
	    StringUtil::StartsWith(t, "CHARACTER(")) {
		return "CASE WHEN (" + col_expr +
		       ") IS NULL THEN 'n():'"
		       " ELSE 's(' || strlen((" +
		       col_expr + ")) || '):' || (" + col_expr + ") END";
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
	// TIMESTAMP_S/MS/NS: DuckDB sub-second-precision variants — epoch_us() works on all of them;
	// sub-second parts are zero for TIMESTAMP_S, microsecond-aligned for TIMESTAMP_MS.
	if (t == "TIMESTAMP" || t == "DATETIME" || t == "TIMESTAMP WITHOUT TIME ZONE" || t == "TIMESTAMP_S" ||
	    t == "TIMESTAMP_MS" || t == "TIMESTAMP_NS") {
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

	// ---------------------------------------------------------------------------
	// v2: Nested/document types — LIST, STRUCT/ROW, MAP, JSON (spec §4.2 tags L, R, M, J).
	// Payload: DuckDB's ::JSON::VARCHAR canonical serialization, which is
	// deterministic within a DuckDB version and satisfies the same-engine
	// comparison guarantee (spec §2).  A future v3 may implement the full
	// recursive sub-field encoding from the spec.
	//
	// All four share an identical emission shape, so they route through the shared
	// NestedJsonEncoding() helper to guarantee no copy-paste drift. NULL-guarded; the
	// byte-length prefix uses strlen() so multibyte JSON text is counted in bytes
	// without the VARCHAR->BLOB cast that DuckDB rejects on non-ASCII input.
	// ---------------------------------------------------------------------------

	// LIST / ARRAY: tag 'L', payload = JSON array string.
	if (StringUtil::EndsWith(t, "[]") || StringUtil::StartsWith(t, "LIST(")) {
		return NestedJsonEncoding(col_expr, 'L');
	}

	// STRUCT / ROW: tag 'R', payload = JSON object string.
	if (StringUtil::StartsWith(t, "STRUCT(") || StringUtil::StartsWith(t, "ROW(")) {
		return NestedJsonEncoding(col_expr, 'R');
	}

	// MAP: tag 'M', payload = JSON object string (DuckDB sorts map keys in JSON output).
	if (StringUtil::StartsWith(t, "MAP(")) {
		return NestedJsonEncoding(col_expr, 'M');
	}

	// JSON: tag 'J', payload = canonical JSON text (::JSON cast is an identity on an
	// already-JSON column but guarantees the engine's canonical rendering). A SQL NULL
	// encodes as 'n():'; a JSON `null` literal encodes as 'J(4):null' — kept distinct.
	if (t == "JSON") {
		return NestedJsonEncoding(col_expr, 'J');
	}

	// Unsupported: throw rather than silently skip (spec §9).
	throw NotImplementedException(
	    "fingerprinting of DuckDB type '%s' is not yet supported in v2 (column expression: %s). "
	    "See docs/fingerprinting.md for supported types.",
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
	// cols[i].first is a bare column name from the catalog (it may contain spaces, reserved
	// keywords, or other characters needing quoting, e.g. "update notes"), so QuoteIdent() it
	// before it is inlined into SQL. We keep the bare name in `cols` because ClassifyTable()
	// matches on it by lowercased text (append-only PK detection).
	std::string concat;
	for (size_t i = 0; i < cols.size(); i++) {
		if (i > 0) {
			concat += " || chr(31) || ";
		}
		concat += "(" + FieldEncodingExpr(QuoteIdent(cols[i].first), cols[i].second) + ")";
	}

	// sha256() returns VARCHAR lowercase hex in DuckDB.
	return "sha256(" + concat + ")";
}

// ---------------------------------------------------------------------------
// v3 pluggable fingerprint strategy engine
//
//   EXACT           estimated table bytes < 1 MB → full per-row sha256 (exact, cheap for small tables)
//   APPEND_ONLY     integer PK / monotonic timestamp → COUNT + MAX(signal_col) (O(1))
//   MUTABLE_ENTITY  everything else → COUNT + MIN/MAX(rowid) via zone maps (O(1))
//
// See docs/fingerprinting.md §6.4 for the full specification and trade-offs.
// ---------------------------------------------------------------------------

namespace {

// ---- Type classification helpers ----

bool IsIntegerType(const std::string &t) {
	const auto u = StringUtil::Upper(t);
	return u == "BIGINT" || u == "INT8" || u == "LONG" || u == "INTEGER" || u == "INT4" || u == "INT" ||
	       u == "SIGNED" || u == "SMALLINT" || u == "INT2" || u == "SHORT" || u == "TINYINT" || u == "INT1" ||
	       u == "UBIGINT" || u == "UINTEGER" || u == "USMALLINT" || u == "UTINYINT" || u == "HUGEINT";
}

bool IsTimestampType(const std::string &t) {
	const auto u = StringUtil::Upper(t);
	return u == "TIMESTAMP" || u == "DATETIME" || u == "TIMESTAMPTZ" || u == "TIMESTAMP WITH TIME ZONE" ||
	       u == "TIMESTAMP WITHOUT TIME ZONE" || u == "TIMESTAMP_S" || u == "TIMESTAMP_MS" || u == "TIMESTAMP_NS";
}

// Estimate average CSV bytes per row from the (name, duckdb_type) column list.
// Mirrors EstimateCSVBytesPerRow() in hypha_snapshot.cpp but operates on the
// classifier's column-pair type instead of ColumnDef.
int64_t EstimateBytesPerRow(const std::vector<std::pair<std::string, std::string>> &cols) {
	int64_t bytes = 0;
	for (const auto &col : cols) {
		const auto t = StringUtil::Upper(col.second);
		int64_t w;
		if (t == "INTEGER" || t == "INT4" || t == "INT" || t == "SIGNED") {
			w = 11;
		} else if (t == "BIGINT" || t == "INT8" || t == "LONG" || t == "HUGEINT") {
			w = 20;
		} else if (t == "SMALLINT" || t == "INT2" || t == "SHORT") {
			w = 6;
		} else if (t == "TINYINT" || t == "INT1") {
			w = 4;
		} else if (t == "UINTEGER" || t == "UBIGINT") {
			w = 20;
		} else if (t == "USMALLINT" || t == "UTINYINT") {
			w = 5;
		} else if (t == "DOUBLE" || t == "FLOAT8" || t == "DOUBLE PRECISION") {
			w = 25;
		} else if (t == "FLOAT" || t == "FLOAT4" || t == "REAL") {
			w = 15;
		} else if (t == "TIMESTAMP" || t == "DATETIME" || t == "TIMESTAMP WITHOUT TIME ZONE" || t == "TIMESTAMPTZ" ||
		           t == "TIMESTAMP WITH TIME ZONE" || t == "TIMESTAMP_S" || t == "TIMESTAMP_MS" ||
		           t == "TIMESTAMP_NS") {
			w = 27;
		} else if (t == "DATE") {
			w = 10;
		} else if (t == "TIME" || t == "TIME WITHOUT TIME ZONE") {
			w = 15;
		} else if (t == "TIMETZ" || t == "TIME WITH TIME ZONE") {
			w = 21;
		} else if (t == "UUID") {
			w = 36;
		} else if (t == "BOOLEAN" || t == "BOOL" || t == "LOGICAL") {
			w = 5;
		} else if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || t == "CHAR VARYING" || t == "CHARACTER VARYING" ||
		           StringUtil::StartsWith(t, "VARCHAR(") || StringUtil::StartsWith(t, "CHAR(") ||
		           StringUtil::StartsWith(t, "CHARACTER(")) {
			w = 32;
		} else if (t == "BLOB" || t == "BYTEA" || t == "BINARY" || t == "VARBINARY") {
			w = 128;
		} else if (t == "JSON") {
			w = 128;
		} else {
			w = 64; // DECIMAL, NUMERIC, INTERVAL, BIT, ENUM, LIST, STRUCT, MAP, …
		}
		bytes += w;
	}
	// CSV framing: one comma per column + newline per row.
	bytes += static_cast<int64_t>(cols.size()) + 1;
	return (bytes > 0) ? bytes : 1;
}

// Returns the exact row count if ≤ limit, otherwise -1.
// Scans at most (limit+1) rows so large tables are never fully counted.
int64_t QuickRowEstimate(Connection &con, const std::string &schema, const std::string &table, int64_t limit = 50000) {
	const auto sql = "SELECT COUNT(*) FROM (SELECT 1 FROM " + QuoteIdent(schema) + "." + QuoteIdent(table) + " LIMIT " +
	                 std::to_string(limit + 1) + ") __t";
	auto result = con.Query(sql);
	if (result->HasError() || result->RowCount() == 0 || result->GetValue(0, 0).IsNull()) {
		return -1;
	}
	const int64_t n = result->GetValue(0, 0).GetValue<int64_t>();
	return (n <= limit) ? n : -1;
}

// Build the MUTABLE_ENTITY rowid-statistics SQL (fallback / default).
// Uses only COUNT + MIN + MAX — both answered from DuckDB zone-map statistics in ~59ms.
std::string BuildMutableEntitySQL(const std::string &schema, const std::string &table) {
	return "SELECT sha256(CAST(COUNT(*) AS VARCHAR) || '|' || "
	       "COALESCE(CAST(MIN(rowid) AS VARCHAR), '') || '|' || "
	       "COALESCE(CAST(MAX(rowid) AS VARCHAR), '')) AS table_hash, "
	       "COUNT(*) AS row_count FROM " +
	       QuoteIdent(schema) + "." + QuoteIdent(table);
}

} // namespace

// Build the EXACT full-per-row sha256 table_hash SQL. Shared by the EXACT strategy, by
// exact_verify mode (force_exact), and by hypha_verify(). Throws (via RowHashExpr) when a
// column type is unsupported for hashing — callers fall back to a structural strategy.
std::string BuildExactTableHashSQL(const std::string &schema_name, const std::string &table_name,
                                   const std::vector<std::pair<std::string, std::string>> &cols) {
	const auto rhe = RowHashExpr(cols);
	return "SELECT sha256(string_agg(rh, chr(10) ORDER BY rh)) AS table_hash, "
	       "COUNT(*) AS row_count FROM "
	       "(SELECT " +
	       rhe + " AS rh FROM " + QuoteIdent(schema_name) + "." + QuoteIdent(table_name) + ") __rows";
}

// ---------------------------------------------------------------------------
// ClassifyTable(): assign a FingerprintStrategy to a specific table.
// See docs/fingerprinting.md §6.4 for the full classification hierarchy.
// ---------------------------------------------------------------------------

FingerprintStrategy ClassifyTable(Connection &con, const std::string &schema_name, const std::string &table_name,
                                  const std::vector<std::pair<std::string, std::string>> &cols, bool is_base_snapshot,
                                  bool force_exact) {
	(void)is_base_snapshot; // classification is now identical for base and incremental snapshots

	// exact_verify mode: force the full per-row EXACT hash for every table regardless of size.
	// This closes the MUTABLE_ENTITY in-place-update blind spot (docs/fingerprinting.md §6.4) at
	// the cost of an O(n) scan. Tables whose column types cannot be hashed fall through to the
	// normal structural classification below rather than failing.
	if (force_exact) {
		try {
			auto sql = BuildExactTableHashSQL(schema_name, table_name, cols);
			return FingerprintStrategy {"EXACT", std::move(sql),
			                            "exact_verify mode: forced full per-row hash (blind-spot-free)"};
		} catch (...) {
			// Unsupported column types — fall through to structural classification.
		}
	}

	// ---- Structural classification ----

	// EXACT: use full per-row sha256 when the estimated table size is below the 1 MB cost
	// threshold. Threshold = estimated_bytes_per_row × row_count < 1 MB (1,048,576 bytes).
	// This correctly promotes wide tables with few rows and demotes narrow tables with many rows.
	static constexpr int64_t EXACT_BYTE_BUDGET = 1048576LL;
	const int64_t bytes_per_row = EstimateBytesPerRow(cols);
	const int64_t max_rows_for_budget = EXACT_BYTE_BUDGET / bytes_per_row;
	const int64_t quick_count = QuickRowEstimate(con, schema_name, table_name, max_rows_for_budget);
	const bool within_budget = (quick_count >= 0);

	if (within_budget) {
		try {
			auto sql = BuildExactTableHashSQL(schema_name, table_name, cols);
			return FingerprintStrategy {"EXACT", std::move(sql),
			                            "estimated ~" + std::to_string(quick_count * bytes_per_row) +
			                                " bytes (<1 MB, " + std::to_string(quick_count) + " rows × " +
			                                std::to_string(bytes_per_row) + " bytes/row), full per-row hash is cheap"};
		} catch (...) {
			// Unsupported column types in RowHashExpr — fall through to structural strategies.
		}
	}

	// APPEND_ONLY: detect a monotonic integer PK column → COUNT + MAX(pk) (O(1)).
	std::string append_col;
	std::string append_reason;
	for (const auto &col : cols) {
		const auto lower = StringUtil::Lower(col.first);
		if (IsIntegerType(col.second)) {
			if (lower == "id" || lower == "pk" || lower == "seq" || lower == "rowno" || lower == "row_no" ||
			    lower == "record_id" || StringUtil::EndsWith(lower, "_id") || StringUtil::StartsWith(lower, "pk_") ||
			    StringUtil::EndsWith(lower, "_seq") || StringUtil::StartsWith(lower, "seq_")) {
				append_col = col.first;
				append_reason = "integer column '" + col.first + "' signals monotonic PK";
				break;
			}
		}
		if (IsTimestampType(col.second)) {
			if (lower == "created_at" || lower == "inserted_at" || lower == "created" || lower == "inserted" ||
			    StringUtil::EndsWith(lower, "_at") || StringUtil::EndsWith(lower, "_ts") ||
			    StringUtil::EndsWith(lower, "_timestamp") || StringUtil::StartsWith(lower, "created_") ||
			    StringUtil::StartsWith(lower, "inserted_")) {
				append_col = col.first;
				append_reason = "timestamp column '" + col.first + "' signals monotonic ordering";
				break;
			}
		}
	}

	if (!append_col.empty()) {
		const auto qc = QuoteIdent(append_col);
		return FingerprintStrategy {"APPEND_ONLY",
		                            "SELECT sha256(CAST(COUNT(*) AS VARCHAR) || '|' || "
		                            "COALESCE(CAST(MAX(" +
		                                qc +
		                                ") AS VARCHAR), '')) AS table_hash, "
		                                "COUNT(*) AS row_count FROM " +
		                                QuoteIdent(schema_name) + "." + QuoteIdent(table_name),
		                            append_reason};
	}

	// MUTABLE_ENTITY: default — COUNT + MIN/MAX(rowid) via DuckDB zone maps (O(1)).
	return FingerprintStrategy {"MUTABLE_ENTITY", BuildMutableEntitySQL(schema_name, table_name),
	                            "default strategy: COUNT + MIN/MAX(rowid) via zone maps (O(1))"};
}

// ---------------------------------------------------------------------------
// ComputeTableFingerprint(): classify, execute strategy, return hash + metadata
// ---------------------------------------------------------------------------

TableFingerprint ComputeTableFingerprint(Connection &con, const std::string &schema_name, const std::string &table_name,
                                         const std::vector<std::pair<std::string, std::string>> &cols,
                                         bool is_base_snapshot, bool force_exact) {
	TableFingerprint fp;

	if (cols.empty()) {
		fp.strategy_name = "EMPTY";
		fp.strategy_rationale = "table has no columns";
		return fp;
	}

	auto strategy = ClassifyTable(con, schema_name, table_name, cols, is_base_snapshot, force_exact);
	fp.strategy_name = strategy.name;
	fp.strategy_rationale = strategy.rationale;

	auto result = con.Query(strategy.sql);
	if (result->HasError()) {
		// Chosen strategy failed at runtime — fall back to MUTABLE_ENTITY.
		fp.strategy_name = "MUTABLE_ENTITY_FALLBACK";
		fp.strategy_rationale = "fallback after " + strategy.name + " failed: " + result->GetError();
		result = con.Query(BuildMutableEntitySQL(schema_name, table_name));
	}

	if (result->HasError()) {
		throw Exception(ExceptionType::EXECUTOR, StringUtil::Format("table fingerprint failed for \"%s\".\"%s\": %s",
		                                                            schema_name, table_name, result->GetError()));
	}

	if (result->RowCount() == 0 || result->GetValue(1, 0).IsNull()) {
		return fp;
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
