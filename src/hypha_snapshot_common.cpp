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
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#include <fcntl.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

//! Run a query and throw with context on error.
unique_ptr<MaterializedQueryResult> Exec(Connection &con, const std::string &sql, const char *context) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw Exception(ExceptionType::EXECUTOR, StringUtil::Format("%s failed: %s", context, result->GetError()));
	}
	return result;
}

//! Stream a DuckDB COPY result directly to Postgres via an in-process pipe — no temp files.
//!
//! The caller must have already opened a COPY IN session on pg (via PQexec("COPY ... FROM STDIN...")).
//! This function runs DuckDB's CSV serializer into the write-end of a pipe while a background
//! thread drains the read-end with PQputCopyData. This eliminates the double I/O of the old
//! write-to-temp-file / read-back-and-send pattern.
//!
//! Returns the total bytes forwarded to Postgres (0 means DuckDB produced no rows for this
//! SELECT — the caller should break its pagination loop).
//!
//! On any failure this function calls PQputCopyEnd(pg, <reason>) to abort the open COPY session
//! and then throws an IOException; the caller should proceed directly to ROLLBACK.
int64_t CopyChunkViaPipe(Connection &con, PGconn *pg, const std::string &select_sql, const char *context) {
	int fds[2];
#ifdef _WIN32
	if (_pipe(fds, 65536, _O_BINARY) != 0) {
#else
	if (::pipe(fds) != 0) {
#endif
		PQputCopyEnd(pg, "pipe creation failed");
		throw IOException(std::string(context) + ": pipe() failed");
	}

	int64_t bytes_sent = 0;
	bool send_failed = false;

	// Reader thread: drains the pipe read-end and forwards each chunk to Postgres.
	// Runs concurrently with the DuckDB COPY write below.
	std::thread reader([&]() {
		char buf[65536];
#ifdef _WIN32
		int n;
		while ((n = _read(fds[0], buf, sizeof(buf))) > 0) {
#else
		ssize_t n;
		while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) {
#endif
			bytes_sent += n;
			if (!send_failed && PQputCopyData(pg, buf, static_cast<int>(n)) != 1) {
				send_failed = true;
				// Drain remaining data so DuckDB does not block on a full pipe buffer.
#ifdef _WIN32
				while (_read(fds[0], buf, sizeof(buf)) > 0) {
#else
				while (::read(fds[0], buf, sizeof(buf)) > 0) {
#endif
				}
				break;
			}
		}
#ifdef _WIN32
		_close(fds[0]);
#else
		::close(fds[0]);
#endif
	});

	const std::string dev_fd = "/dev/fd/" + std::to_string(fds[1]);
	const std::string full_sql =
	    StringUtil::Format("COPY (%s) TO %s (FORMAT CSV, HEADER FALSE, NULL '\\N')", select_sql, QuoteLiteral(dev_fd));
	std::exception_ptr duckdb_ex;
	try {
		Exec(con, full_sql, context);
	} catch (...) {
		duckdb_ex = std::current_exception();
	}
	// Closing the write-end signals EOF to the reader thread.
#ifdef _WIN32
	_close(fds[1]);
#else
	::close(fds[1]);
#endif
	reader.join();

	if (duckdb_ex) {
		PQputCopyEnd(pg, "duckdb serialization error");
		std::rethrow_exception(duckdb_ex);
	}
	if (send_failed) {
		PQputCopyEnd(pg, "data send error");
		throw IOException(std::string(context) + ": PQputCopyData failed");
	}
	return bytes_sent;
}

//! Map a DuckDB data_type string to its closest Postgres equivalent.
//! Returns "(unsupported: <type>)" for types with no safe mapping.
std::string DuckTypeToPostgres(const std::string &raw) {
	const auto t = StringUtil::Upper(raw);

	// Exact matches for common scalars
	if (t == "INTEGER" || t == "INT4" || t == "INT" || t == "SIGNED") {
		return "integer";
	}
	if (t == "BIGINT" || t == "INT8" || t == "LONG") {
		return "bigint";
	}
	if (t == "SMALLINT" || t == "INT2" || t == "SHORT") {
		return "smallint";
	}
	if (t == "TINYINT" || t == "INT1") {
		return "smallint"; // PG has no TINYINT; SMALLINT (int2) is the smallest
	}
	if (t == "HUGEINT") {
		return "numeric(39,0)";
	}
	if (t == "UBIGINT") {
		return "numeric(20,0)";
	}
	if (t == "UINTEGER") {
		return "bigint";
	}
	if (t == "USMALLINT") {
		return "integer";
	}
	if (t == "UTINYINT") {
		return "smallint";
	}
	if (t == "FLOAT" || t == "FLOAT4" || t == "REAL") {
		return "real";
	}
	if (t == "DOUBLE" || t == "FLOAT8" || t == "DOUBLE PRECISION") {
		return "double precision";
	}
	if (t == "BOOLEAN" || t == "BOOL" || t == "LOGICAL") {
		return "boolean";
	}
	if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || t == "CHAR VARYING" || t == "CHARACTER VARYING") {
		return "text";
	}
	if (t == "DATE") {
		return "date";
	}
	if (t == "TIME" || t == "TIME WITHOUT TIME ZONE") {
		return "time without time zone";
	}
	if (t == "TIMETZ" || t == "TIME WITH TIME ZONE") {
		return "time with time zone";
	}
	if (t == "TIMESTAMP" || t == "DATETIME" || t == "TIMESTAMP WITHOUT TIME ZONE") {
		return "timestamp without time zone";
	}
	if (t == "TIMESTAMP_S" || t == "TIMESTAMP_MS" || t == "TIMESTAMP_NS") {
		return "timestamp without time zone";
	}
	if (t == "TIMESTAMPTZ" || t == "TIMESTAMP WITH TIME ZONE") {
		return "timestamp with time zone";
	}
	if (t == "TIMESTAMPTZ_S" || t == "TIMESTAMPTZ_MS" || t == "TIMESTAMPTZ_NS") {
		return "timestamp with time zone";
	}
	if (t == "INTERVAL") {
		return "interval";
	}
	if (t == "BLOB" || t == "BYTEA" || t == "BINARY" || t == "VARBINARY") {
		return "bytea";
	}
	if (t == "UUID") {
		return "uuid";
	}
	if (t == "JSON") {
		return "jsonb";
	}
	if (t == "BIT" || t == "BITSTRING") {
		return "bit varying";
	}

	// Parameterized types: DECIMAL(p,s) / NUMERIC(p,s) / CHAR(n) / VARCHAR(n)
	if (StringUtil::StartsWith(t, "DECIMAL(") || StringUtil::StartsWith(t, "NUMERIC(")) {
		// Re-emit as lowercase numeric(p,s)
		const auto paren = raw.find('(');
		return "numeric" + raw.substr(paren);
	}
	if (StringUtil::StartsWith(t, "CHAR(") || StringUtil::StartsWith(t, "CHARACTER(")) {
		const auto paren = raw.find('(');
		return "char" + raw.substr(paren);
	}
	if (StringUtil::StartsWith(t, "VARCHAR(")) {
		const auto paren = raw.find('(');
		return "varchar" + raw.substr(paren);
	}
	// Plain DECIMAL / NUMERIC without precision
	if (t == "DECIMAL" || t == "NUMERIC") {
		return "numeric";
	}

	// All compound/nested types map to JSONB.
	// DuckDB's CSV export for arrays uses "[a, b]" bracket format which is NOT compatible
	// with Postgres's "{a,b}" array literal format in COPY FROM. Mapping to JSONB and using
	// a ::JSON cast in COPY avoids this mismatch for all compound types.
	if (StringUtil::EndsWith(t, "[]") || StringUtil::StartsWith(t, "LIST(")) {
		return "jsonb";
	}
	if (StringUtil::StartsWith(t, "STRUCT(") || StringUtil::StartsWith(t, "ROW(")) {
		return "jsonb";
	}
	if (StringUtil::StartsWith(t, "MAP(")) {
		return "jsonb";
	}

	return "(unsupported: " + raw + ")";
}

//! Returns true for DuckDB integer types usable with keyset (WHERE pk > last) pagination.
bool IsIntegerDuckType(const std::string &duckdb_type) {
	const auto t = StringUtil::Upper(duckdb_type);
	return t == "INTEGER" || t == "INT4" || t == "INT" || t == "SIGNED" || t == "BIGINT" || t == "INT8" ||
	       t == "LONG" || t == "SMALLINT" || t == "INT2" || t == "SHORT" || t == "TINYINT" || t == "INT1" ||
	       t == "UINTEGER" || t == "UBIGINT" || t == "USMALLINT" || t == "UTINYINT";
}

//! Estimate the average CSV byte width per row based on column types.
//! varchar_widths overrides the per-column heuristic for VARCHAR-like columns (name -> avg width).
int64_t EstimateCSVBytesPerRow(const std::vector<ColumnDef> &cols,
                               const std::unordered_map<std::string, int64_t> &varchar_widths) {
	int64_t bytes = 0;
	for (const auto &cd : cols) {
		const auto t = StringUtil::Upper(cd.duckdb_type);
		int64_t w;
		if (t == "INTEGER" || t == "INT" || t == "INT4" || t == "INT32") {
			w = 11;
		} else if (t == "BIGINT" || t == "INT8" || t == "INT64" || t == "HUGEINT") {
			w = 20;
		} else if (t == "DOUBLE" || t == "FLOAT" || t == "REAL") {
			w = 25;
		} else if (t == "TIMESTAMP" || t == "TIMESTAMPTZ") {
			w = 27;
		} else if (t == "DATE") {
			w = 10;
		} else if (t == "TIME") {
			w = 15;
		} else if (t == "UUID") {
			w = 36;
		} else if (t == "BOOLEAN") {
			w = 5;
		} else if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || StringUtil::StartsWith(t, "VARCHAR(") ||
		           StringUtil::StartsWith(t, "CHAR(") || StringUtil::StartsWith(t, "CHARACTER VARYING")) {
			const auto it = varchar_widths.find(cd.name);
			w = (it != varchar_widths.end()) ? it->second : 32;
		} else if (t == "BLOB" || t == "BYTEA" || t == "BINARY") {
			w = 128;
		} else if (t == "JSON") {
			w = 128;
		} else if (t == "STRUCT" || t == "LIST" || t == "MAP" || t == "UNION" ||
		           StringUtil::StartsWith(t, "STRUCT(") || StringUtil::StartsWith(t, "LIST(") ||
		           StringUtil::StartsWith(t, "MAP(") || StringUtil::EndsWith(t, "[]")) {
			w = 64;
		} else {
			w = 64;
		}
		bytes += w;
	}
	// Add CSV framing: n_cols commas + newline
	bytes += static_cast<int64_t>(cols.size()) + 1;
	return (bytes > 0) ? bytes : 1;
}

//! Query actual max string lengths for VARCHAR columns via DuckDB's stats() function.
//! Returns a map from column name -> estimated average width (max_len / 2, capped at 4096).
//! Falls back to empty map (32-byte heuristic) on any error or empty table.
std::unordered_map<std::string, int64_t> QueryVarcharWidths(Connection &con, const std::string &schema_name,
                                                            const std::string &table_name,
                                                            const std::vector<ColumnDef> &cols) {
	std::unordered_map<std::string, int64_t> result;
	std::vector<std::string> varchar_cols;
	for (const auto &cd : cols) {
		const auto t = StringUtil::Upper(cd.duckdb_type);
		if (t == "VARCHAR" || t == "TEXT" || t == "STRING" || StringUtil::StartsWith(t, "VARCHAR(") ||
		    StringUtil::StartsWith(t, "CHAR(") || StringUtil::StartsWith(t, "CHARACTER VARYING")) {
			varchar_cols.push_back(cd.name);
		}
	}
	if (varchar_cols.empty()) {
		return result;
	}
	std::ostringstream sql;
	sql << "SELECT ";
	for (size_t i = 0; i < varchar_cols.size(); i++) {
		if (i > 0) {
			sql << ", ";
		}
		// TRY_CAST so an empty regexp match (empty table or missing stats) returns NULL safely.
		sql << "TRY_CAST(regexp_extract(stats(" << QuoteIdent(varchar_cols[i])
		    << "), 'Max String Length: (\\d+)', 1) AS INTEGER)";
	}
	sql << " FROM " << QuoteIdent(schema_name) << "." << QuoteIdent(table_name) << " LIMIT 1";
	try {
		const auto res = con.Query(sql.str());
		if (!res || res->HasError() || res->RowCount() == 0) {
			return result;
		}
		for (size_t i = 0; i < varchar_cols.size(); i++) {
			const auto val = res->GetValue(static_cast<idx_t>(i), 0);
			if (!val.IsNull()) {
				try {
					const int64_t max_len = val.GetValue<int64_t>();
					if (max_len > 0) {
						const int64_t avg = max_len / 2;
						result[varchar_cols[i]] = (avg < 1) ? 1 : (avg > 4096 ? 4096 : avg);
					}
				} catch (...) {
				}
			}
		}
	} catch (...) {
		// Fall back to 32-byte heuristic for all VARCHAR columns
	}
	return result;
}

//! Compute chunk row count so each chunk is approximately target_chunk_bytes of CSV.
//! Clamped to [1000, 500000] rows.
int64_t ComputeChunkRows(const std::vector<ColumnDef> &cols, int64_t target_chunk_bytes,
                         const std::unordered_map<std::string, int64_t> &varchar_widths) {
	if (cols.empty() || target_chunk_bytes <= 0) {
		return 1000;
	}
	const int64_t bytes_per_row = EstimateCSVBytesPerRow(cols, varchar_widths);
	const int64_t chunk_rows = target_chunk_bytes / bytes_per_row;
	if (chunk_rows < 1000) {
		return 1000;
	}
	return (chunk_rows > 500000) ? 500000 : chunk_rows;
}

//! Truncate to PG_MAX_IDENT bytes (UTF-8 safe via simple byte truncation; identifiers are
//! typically ASCII). Returns the truncated string.
std::string TruncPGIdent(const std::string &name) {
	if (name.size() <= PG_MAX_IDENT) {
		return name;
	}
	return name.substr(0, PG_MAX_IDENT);
}

//! Truncate an identifier to fit within Postgres's 63-char limit, appending a numeric
//! suffix (_2, _3, ...) to guarantee uniqueness within a schema's used_names set.
//! The used_names set is updated in-place; callers must use one set per schema.
std::string SafeTruncateIdent(const std::string &name, std::unordered_set<std::string> &used_names, size_t max_len) {
	if (name.size() <= max_len && used_names.find(name) == used_names.end()) {
		used_names.insert(name);
		return name;
	}
	// Truncate to max_len first; if that's unique, use it directly.
	std::string base = name.substr(0, max_len);
	if (used_names.find(base) == used_names.end()) {
		used_names.insert(base);
		return base;
	}
	// Append _2, _3, ... until we find a free slot.
	for (int i = 2; i < 10000; i++) {
		const std::string suffix = "_" + std::to_string(i);
		const std::string candidate = name.substr(0, max_len - suffix.size()) + suffix;
		if (used_names.find(candidate) == used_names.end()) {
			used_names.insert(candidate);
			return candidate;
		}
	}
	throw std::runtime_error("Too many name collisions for: " + name);
}

} // namespace duckdb
