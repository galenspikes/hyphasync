#include "hypha_postgres.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <chrono>
#include <libpq-fe.h>
#include <sstream>
#include <thread>

namespace duckdb {

namespace {

bool StartsWith(const std::string &value, const char *prefix) {
	return value.rfind(prefix, 0) == 0;
}

std::string AppendConnectTimeout(const std::string &conn_string) {
	if (conn_string.find("connect_timeout") != std::string::npos) {
		return conn_string;
	}
	if (StartsWith(conn_string, "postgresql://") || StartsWith(conn_string, "postgres://")) {
		if (conn_string.find('?') != std::string::npos) {
			return conn_string + "&connect_timeout=5";
		}
		return conn_string + "?connect_timeout=5";
	}
	return conn_string + " connect_timeout=5";
}

std::string Clean(const char *value) {
	if (!value) {
		return "";
	}
	std::string out = value;
	// libpq messages end with a trailing newline; trim it for single-line reporting.
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

//! Validates conninfo syntax without opening a socket. Returns true when parseable.
bool ValidateConnInfo(const std::string &conn_string, std::string &error_out) {
	char *parse_error = nullptr;
	PQconninfoOption *options = PQconninfoParse(conn_string.c_str(), &parse_error);
	if (!options) {
		// Out-of-memory or a syntax error (parse_error populated on syntax errors).
		error_out = parse_error ? Clean(parse_error) : "could not parse connection string (out of memory)";
		if (parse_error) {
			PQfreemem(parse_error);
		}
		return false;
	}
	PQconninfoFree(options);
	if (parse_error) {
		PQfreemem(parse_error);
	}
	return true;
}

struct ScalarResult {
	bool ok = false;
	std::string value;
	std::string error;
	std::string error_code;
};

ScalarResult RunScalarQuery(PGconn *conn, const char *sql) {
	ScalarResult out;
	PGresult *result = PQexec(conn, sql);
	if (!result) {
		out.error = "query failed: " + Clean(PQerrorMessage(conn));
		return out;
	}
	if (PQresultStatus(result) != PGRES_TUPLES_OK) {
		out.error = Clean(PQresultErrorMessage(result));
		out.error_code = Clean(PQresultErrorField(result, PG_DIAG_SQLSTATE));
		PQclear(result);
		return out;
	}
	if (PQntuples(result) < 1 || PQnfields(result) < 1) {
		out.error = "query returned no rows";
		PQclear(result);
		return out;
	}
	out.ok = true;
	out.value = Clean(PQgetvalue(result, 0, 0));
	PQclear(result);
	return out;
}

struct CountResult {
	bool ok = false;
	int64_t value = 0;
	std::string error;
	std::string error_code;
};

CountResult RunCountQuery(PGconn *conn, const char *sql) {
	CountResult out;
	const auto scalar = RunScalarQuery(conn, sql);
	if (!scalar.ok) {
		out.error = scalar.error;
		out.error_code = scalar.error_code;
		return out;
	}
	try {
		out.value = std::stoll(scalar.value);
		out.ok = true;
	} catch (...) {
		out.error = "could not parse count value '" + scalar.value + "'";
	}
	return out;
}

std::string FormatAttachReport(const std::string &status, const std::string &target_name, int64_t latency_ms,
                               const std::string &postgres_version, const std::string &database,
                               const std::string &user, const std::string &remote_hypha_schema,
                               const std::string &remote_hypha_table_count, const std::string &remote_hypha_sync_log,
                               const std::string &remote_hypha_object_state, const std::string &error,
                               const std::string &error_code) {
	std::ostringstream report;
	report << "status=" << status << "\n";
	report << "target_name=" << target_name << "\n";
	report << "latency_ms=" << latency_ms << "\n";
	if (!postgres_version.empty()) {
		report << "postgres_version=" << postgres_version << "\n";
	}
	if (!database.empty()) {
		report << "database=" << database << "\n";
	}
	if (!user.empty()) {
		report << "user=" << user << "\n";
	}
	report << "remote_hypha_schema=" << remote_hypha_schema << "\n";
	report << "remote_hypha_table_count=" << remote_hypha_table_count << "\n";
	report << "remote_hypha_sync_log=" << remote_hypha_sync_log << "\n";
	report << "remote_hypha_object_state=" << remote_hypha_object_state << "\n";
	if (!error.empty()) {
		report << "error=" << error << "\n";
	}
	if (!error_code.empty()) {
		report << "error_code=" << error_code << "\n";
	}
	report << "note=read-only probe only; no remote writes performed";
	return report.str();
}

} // namespace

std::string RedactConnString(const std::string &conn_string) {
	std::string s = conn_string;

	// 1) URL userinfo form: scheme://user:password@host -> scheme://user:***@host
	const auto scheme = s.find("://");
	if (scheme != std::string::npos) {
		const auto userinfo_start = scheme + 3;
		const auto at = s.find('@', userinfo_start);
		if (at != std::string::npos) {
			const auto colon = s.find(':', userinfo_start);
			if (colon != std::string::npos && colon < at) {
				s = s.substr(0, colon + 1) + "***" + s.substr(at);
			}
		}
	}

	// 2) keyword/query "password=..." form (conninfo or URL query). Mask the value
	// up to the next space or '&'. Re-lower after each replacement since length changes.
	const std::string key = "password=";
	size_t search = 0;
	std::string lower = StringUtil::Lower(s);
	while (true) {
		const auto p = lower.find(key, search);
		if (p == std::string::npos) {
			break;
		}
		const auto val_start = p + key.size();
		auto end = val_start;
		while (end < s.size() && s[end] != ' ' && s[end] != '&') {
			end++;
		}
		s.replace(val_start, end - val_start, "***");
		lower = StringUtil::Lower(s);
		search = val_start + 3;
	}
	return s;
}

PGconn *OpenHyphaConnection(const std::string &conn_string, bool fast_mode) {
	if (conn_string.empty()) {
		throw InvalidInputException("no Postgres connection string provided; call hypha_init() first or pass a URL.");
	}
	std::string validation_error;
	if (!ValidateConnInfo(conn_string, validation_error)) {
		throw InvalidInputException("invalid connection string: %s.", validation_error);
	}
	const auto conninfo = AppendConnectTimeout(conn_string);
	PGconn *conn = PQconnectdb(conninfo.c_str());
	if (!conn) {
		throw IOException("libpq failed to allocate a connection (out of memory).");
	}
	if (PQstatus(conn) != CONNECTION_OK) {
		auto error = Clean(PQerrorMessage(conn));
		PQfinish(conn);
		if (error.empty()) {
			error = "could not connect to Postgres target";
		}
		throw ConnectionException(
		    "could not connect to the Postgres target: %s. "
		    "Verify the connection string, that the server is reachable, and that credentials are correct.",
		    error);
	}
	// Suppress NOTICE-level messages (e.g. "schema already exists, skipping") from DDL
	// statements that use IF NOT EXISTS / IF EXISTS. Warnings and errors still surface.
	PQclear(PQexec(conn, "SET client_min_messages = warning"));
	if (fast_mode) {
		// Disable synchronous WAL flush only when fast_mode=true.
		// This risks data loss on Postgres crash but is safe when hyphasync is a read replica
		// and can re-run the snapshot/sync after recovery.
		PQclear(PQexec(conn, "SET synchronous_commit = off"));
	}
	return conn;
}

HyphaConnectionProbe ProbeHyphaConnection(const std::string &conn_string) {
	HyphaConnectionProbe probe;
	const auto started = std::chrono::steady_clock::now();

	if (conn_string.empty()) {
		probe.error = "connection string is empty; call hypha_init() or pass a Postgres URL";
		return probe;
	}

	std::string validation_error;
	if (!ValidateConnInfo(conn_string, validation_error)) {
		probe.error = "invalid connection string: " + validation_error;
		return probe;
	}
	probe.conninfo_valid = true;

	const auto conninfo = AppendConnectTimeout(conn_string);
	PGconn *conn = PQconnectdb(conninfo.c_str());
	if (!conn) {
		probe.error = "libpq failed to allocate a connection (out of memory)";
		return probe;
	}

	if (PQstatus(conn) != CONNECTION_OK) {
		probe.latency_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		probe.error = Clean(PQerrorMessage(conn));
		if (probe.error.empty()) {
			probe.error = "could not connect to Postgres target";
		}
		PQfinish(conn);
		return probe;
	}

	probe.connected = true;
	if (const char *h = PQhost(conn)) {
		probe.host = h;
	}
	if (const char *p = PQport(conn)) {
		probe.port = p;
	}
	const auto version = RunScalarQuery(conn, "SELECT version()");
	if (version.ok) {
		probe.postgres_version = version.value;
	}
	const auto database = RunScalarQuery(conn, "SELECT current_database()");
	if (database.ok) {
		probe.database = database.value;
	}
	const auto user = RunScalarQuery(conn, "SELECT current_user");
	if (user.ok) {
		probe.user = user.value;
	}

	PQfinish(conn);
	probe.latency_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	return probe;
}

std::string RunHyphaTargetStatus(const std::string &conn_string, const std::string &target_label) {
	const auto target_name = target_label.empty() ? std::string("(explicit)") : target_label;
	const auto started = std::chrono::steady_clock::now();

	// Hard failures (no connection at all) throw, so a failed probe can never be
	// mistaken for success in a SQL pipeline. The structured report is reserved
	// for connections that actually succeeded (status=ok or status=degraded).
	if (conn_string.empty()) {
		throw InvalidInputException(
		    "hypha_target_status has no connection string to probe: pass a Postgres URL or call "
		    "hypha_init() first so a default target is stored.");
	}

	std::string validation_error;
	if (!ValidateConnInfo(conn_string, validation_error)) {
		throw InvalidInputException("hypha_target_status received an invalid connection string: %s.", validation_error);
	}

	const auto conninfo = AppendConnectTimeout(conn_string);
	PGconn *conn = PQconnectdb(conninfo.c_str());
	if (!conn) {
		throw IOException("hypha_target_status: libpq failed to allocate a connection (out of memory).");
	}

	if (PQstatus(conn) != CONNECTION_OK) {
		auto error = Clean(PQerrorMessage(conn));
		PQfinish(conn);
		if (error.empty()) {
			error = "could not connect to Postgres target";
		}
		throw ConnectionException(
		    "hypha_target_status could not connect to the Postgres target: %s. "
		    "Verify the connection string, that the server is reachable, and that credentials are correct.",
		    error);
	}

	const auto postgres_version = RunScalarQuery(conn, "SELECT version()");
	const auto database = RunScalarQuery(conn, "SELECT current_database()");
	const auto user = RunScalarQuery(conn, "SELECT current_user");
	const auto schema_count =
	    RunCountQuery(conn, "SELECT COUNT(*)::BIGINT FROM information_schema.schemata WHERE schema_name = 'hypha'");
	const auto table_count =
	    RunCountQuery(conn, "SELECT COUNT(*)::BIGINT FROM information_schema.tables WHERE table_schema = 'hypha'");
	const auto sync_log_count = RunCountQuery(conn, "SELECT COUNT(*)::BIGINT FROM information_schema.tables "
	                                                "WHERE table_schema = 'hypha' AND table_name = 'sync_log'");
	const auto object_state_count = RunCountQuery(conn, "SELECT COUNT(*)::BIGINT FROM information_schema.tables "
	                                                    "WHERE table_schema = 'hypha' AND table_name = 'object_state'");

	PQfinish(conn);

	const auto latency_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

	// Connected, but surface any probe that failed instead of silently reporting empty/false/0.
	std::string probe_errors;
	std::string error_code;
	auto note_failure = [&](const std::string &label, const std::string &err, const std::string &code) {
		if (!probe_errors.empty()) {
			probe_errors += "; ";
		}
		probe_errors += err.empty() ? (label + " probe failed") : (label + " probe failed: " + err);
		if (error_code.empty() && !code.empty()) {
			error_code = code;
		}
	};

	if (!postgres_version.ok) {
		note_failure("version()", postgres_version.error, postgres_version.error_code);
	}
	if (!database.ok) {
		note_failure("current_database()", database.error, database.error_code);
	}
	if (!user.ok) {
		note_failure("current_user", user.error, user.error_code);
	}

	std::string remote_hypha_schema;
	if (schema_count.ok) {
		remote_hypha_schema = schema_count.value > 0 ? "true" : "false";
	} else {
		remote_hypha_schema = "unknown";
		note_failure("hypha schema", schema_count.error, schema_count.error_code);
	}

	std::string remote_hypha_table_count;
	if (table_count.ok) {
		remote_hypha_table_count = std::to_string(table_count.value);
	} else {
		remote_hypha_table_count = "unknown";
		note_failure("hypha table count", table_count.error, table_count.error_code);
	}

	const std::string remote_hypha_sync_log =
	    sync_log_count.ok ? (sync_log_count.value > 0 ? "true" : "false") : "unknown";
	const std::string remote_hypha_object_state =
	    object_state_count.ok ? (object_state_count.value > 0 ? "true" : "false") : "unknown";

	const std::string status = probe_errors.empty() ? "ok" : "degraded";

	return FormatAttachReport(status, target_name, latency_ms, postgres_version.value, database.value, user.value,
	                          remote_hypha_schema, remote_hypha_table_count, remote_hypha_sync_log,
	                          remote_hypha_object_state, probe_errors, error_code);
}

void PGExecWithRetry(PGconn *&conn, const std::string &connstring, const std::string &sql, const char *context,
                     int max_retries, bool fast_mode) {
	for (int attempt = 0; attempt <= max_retries; attempt++) {
		// Reconnect if the connection is bad.
		if (PQstatus(conn) != CONNECTION_OK) {
			PQreset(conn);
			if (PQstatus(conn) != CONNECTION_OK) {
				PQfinish(conn);
				conn = PQconnectdb(connstring.c_str());
				if (!conn || PQstatus(conn) != CONNECTION_OK) {
					const std::string err = conn ? Clean(PQerrorMessage(conn)) : "out of memory";
					if (conn) {
						PQfinish(conn);
						conn = nullptr;
					}
					throw IOException(StringUtil::Format("%s: reconnect failed: %s", context, err));
				}
				// Re-apply session settings after a fresh connect.
				PQclear(PQexec(conn, "SET client_min_messages = warning"));
				if (fast_mode) {
					PQclear(PQexec(conn, "SET synchronous_commit = off"));
				}
			}
		}

		PGresult *res = PQexec(conn, sql.c_str());
		if (!res) {
			if (attempt < max_retries) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
				continue;
			}
			throw IOException(
			    StringUtil::Format("%s: PQexec returned null after %d attempts (out of memory)", context, max_retries));
		}

		const ExecStatusType status = PQresultStatus(res);
		if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
			PQclear(res);
			return;
		}

		// Check for transient error codes: 40001 (serialization), 40P01 (deadlock), 08* (connection).
		const std::string sqlstate = Clean(PQresultErrorField(res, PG_DIAG_SQLSTATE));
		const std::string errmsg = Clean(PQresultErrorMessage(res));
		PQclear(res);

		const bool is_transient = (sqlstate.size() >= 2 && sqlstate.substr(0, 2) == "40") ||
		                          (sqlstate.size() >= 2 && sqlstate.substr(0, 2) == "08") || sqlstate.empty();

		if (is_transient && attempt < max_retries) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
			continue;
		}

		throw IOException(StringUtil::Format("%s: %s", context, errmsg));
	}
}

std::string QuoteIdent(const std::string &name) {
	std::string out = "\"";
	for (const char c : name) {
		if (c == '"') {
			out += '"';
		}
		out += c;
	}
	out += '"';
	return out;
}

std::string QuoteLiteral(const std::string &val) {
	std::string out = "'";
	for (const char c : val) {
		if (c == '\'') {
			out += '\'';
		} else if (c == '\\') {
			out += '\\';
		}
		out += c;
	}
	out += '\'';
	return out;
}

void LogEventRemote(PGconn *pg, const std::string &db_name, const std::string &level, const std::string &operation,
                    const std::string &code, const std::string &message, const std::string &details_json) {
	if (!pg || PQstatus(pg) != CONNECTION_OK) {
		return;
	}
	// Parameterized to avoid quoting issues with arbitrary message text.
	const char *values[5] = {db_name.c_str(), level.c_str(), operation.c_str(), code.c_str(), message.c_str()};
	PGresult *res = PQexecParams(pg,
	                             "INSERT INTO hypha.event_log (db_name, level, operation, code, message) "
	                             "VALUES ($1, $2, $3, $4, $5)",
	                             5, nullptr, values, nullptr, nullptr, 0);
	if (res) {
		PQclear(res);
	}
}

} // namespace duckdb
