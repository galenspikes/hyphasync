#include "hypha_postgres.hpp"

#include "duckdb/common/string_util.hpp"

#include <chrono>
#include <libpq-fe.h>
#include <sstream>

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

std::string PQEscape(const char *value) {
	if (!value) {
		return "";
	}
	return value;
}

std::string FormatAttachReport(const std::string &status, const std::string &target_name, int64_t latency_ms,
                               const std::string &postgres_version, const std::string &database,
                               const std::string &user, bool remote_hypha_schema, int64_t remote_hypha_table_count,
                               const std::string &error) {
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
	report << "remote_hypha_schema=" << (remote_hypha_schema ? "true" : "false") << "\n";
	report << "remote_hypha_table_count=" << remote_hypha_table_count << "\n";
	if (!error.empty()) {
		report << "error=" << error << "\n";
	}
	report << "note=read-only probe only; no remote writes performed";
	return report.str();
}

std::string RunScalarQuery(PGconn *conn, const char *sql) {
	PGresult *result = PQexec(conn, sql);
	if (!result) {
		return "";
	}
	if (PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) < 1 || PQnfields(result) < 1) {
		PQclear(result);
		return "";
	}
	const auto value = PQgetvalue(result, 0, 0);
	std::string output = PQEscape(value);
	PQclear(result);
	return output;
}

int64_t RunCountQuery(PGconn *conn, const char *sql) {
	PGresult *result = PQexec(conn, sql);
	if (!result) {
		return 0;
	}
	if (PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) < 1 || PQnfields(result) < 1) {
		PQclear(result);
		return 0;
	}
	const auto value = PQgetvalue(result, 0, 0);
	int64_t count = 0;
	if (value) {
		try {
			count = std::stoll(value);
		} catch (...) {
			count = 0;
		}
	}
	PQclear(result);
	return count;
}

} // namespace

std::string RunHyphaAttachCheck(const std::string &conn_string) {
	const auto target_name = std::string("default");
	const auto started = std::chrono::steady_clock::now();

	if (conn_string.empty()) {
		return FormatAttachReport("error", target_name, 0, "", "", "", false, 0,
		                          "connection string is empty; call hypha_init() or pass a URL");
	}

	const auto conninfo = AppendConnectTimeout(conn_string);
	PGconn *conn = PQconnectdb(conninfo.c_str());
	if (!conn) {
		return FormatAttachReport("error", target_name, 0, "", "", "", false, 0, "libpq failed to allocate connection");
	}

	if (PQstatus(conn) != CONNECTION_OK) {
		const auto latency_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		const auto error = PQEscape(PQerrorMessage(conn));
		PQfinish(conn);
		return FormatAttachReport("error", target_name, latency_ms, "", "", "", false, 0, error);
	}

	const auto postgres_version = RunScalarQuery(conn, "SELECT version()");
	const auto database = RunScalarQuery(conn, "SELECT current_database()");
	const auto user = RunScalarQuery(conn, "SELECT current_user");
	const auto remote_hypha_table_count =
	    RunCountQuery(conn, "SELECT COUNT(*)::BIGINT FROM information_schema.tables WHERE table_schema = 'hypha'");
	const bool remote_hypha_schema =
	    RunCountQuery(conn, "SELECT COUNT(*)::BIGINT FROM information_schema.schemata WHERE schema_name = 'hypha'") > 0;

	PQfinish(conn);

	const auto latency_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	return FormatAttachReport("ok", target_name, latency_ms, postgres_version, database, user, remote_hypha_schema,
	                          remote_hypha_table_count, "");
}

} // namespace duckdb
