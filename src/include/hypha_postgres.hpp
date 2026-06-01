#pragma once

#include <cstdint>
#include <libpq-fe.h>
#include <string>

namespace duckdb {

//! RAII owner of a PGconn*. Non-copyable; movable.
//! Automatically calls PQfinish on destruction.
struct PgConn {
	PGconn *conn = nullptr;
	PgConn() = default;
	explicit PgConn(PGconn *c) : conn(c) {
	}
	PgConn(const PgConn &) = delete;
	PgConn &operator=(const PgConn &) = delete;
	PgConn(PgConn &&o) noexcept : conn(o.conn) {
		o.conn = nullptr;
	}
	PgConn &operator=(PgConn &&o) noexcept {
		if (conn) {
			PQfinish(conn);
		}
		conn = o.conn;
		o.conn = nullptr;
		return *this;
	}
	~PgConn() {
		if (conn) {
			PQfinish(conn);
		}
	}
	//! Implicit conversion so existing PQexec/PQputCopyData call sites still compile.
	operator PGconn *() const {
		return conn;
	}
	PGconn *get() const {
		return conn;
	}
	//! Replace the managed pointer, finishing the old one if present.
	void reset(PGconn *c = nullptr) {
		if (conn) {
			PQfinish(conn);
		}
		conn = c;
	}
	//! True when a non-null connection is held.
	explicit operator bool() const {
		return conn != nullptr;
	}
};

//! Outcome of a read-only Postgres connectivity probe (libpq). Never mutates the remote database.
struct HyphaConnectionProbe {
	//! True only when libpq reported CONNECTION_OK.
	bool connected = false;
	//! True when the connection string parsed as a valid libpq conninfo / URL.
	bool conninfo_valid = false;
	//! Human-readable failure reason (libpq message or validation detail). Empty on success.
	std::string error;
	//! SQLSTATE or libpq error code when available. Empty otherwise.
	std::string error_code;
	//! Wall-clock time spent establishing the connection, in milliseconds.
	int64_t latency_ms = 0;
	std::string postgres_version;
	std::string database;
	std::string user;
	std::string host;
	std::string port;
};

//! Validates conninfo syntax and attempts a live connection. Read-only; gathers version/db/user on success.
HyphaConnectionProbe ProbeHyphaConnection(const std::string &conn_string);

//! Opens a validated, connected PGconn ready for use. Appends a 5-second connect timeout if not
//! already present. Throws ConnectionException / InvalidInputException / IOException on any failure.
//! The caller MUST call PQfinish() on the returned pointer when done.
//! When fast_mode=true, applies SET synchronous_commit=off on the new connection.
PGconn *OpenHyphaConnection(const std::string &conn_string, bool fast_mode = false);

//! Masks the password in a Postgres connection string (URL userinfo and password= keyword/query forms)
//! so it is safe to print in reports, logs, and error messages.
std::string RedactConnString(const std::string &conn_string);

//! Fire-and-forget INSERT into hypha.event_log on Postgres.
//! Uses a parameterized query; silently ignores all errors (network drop, missing table, etc.).
//! This must only be called on a dedicated auto-commit connection (no open BEGIN block),
//! so that each INSERT is immediately visible without waiting for the main transaction.
void LogEventRemote(PGconn *pg, const std::string &db_name, const std::string &level, const std::string &operation,
                    const std::string &code, const std::string &message, const std::string &details_json = "");

//! Read-only Postgres connectivity / capability probe (libpq). Does not mutate the remote database.
//! Throws on hard failures (empty/invalid conninfo, or no connection). On a successful connection it
//! returns a multi-line status report (status=ok when every probe succeeded, status=degraded when
//! connected but one or more catalog probes failed). Backs the hypha_target_status() SQL function.
//! `target_label` is reported verbatim as the report's target_name (e.g. "default" or "(explicit)").
std::string RunHyphaTargetStatus(const std::string &conn_string, const std::string &target_label);

//! Execute `sql` on `conn`, retrying up to `max_retries` times on transient SQLSTATEs
//! (40001 serialization failure, 40P01 deadlock, 08* connection errors).
//! Reconnects automatically when PQstatus(conn) == CONNECTION_BAD.
//! `conn` is passed by reference so a successful reconnect is visible to the caller.
//! When fast_mode=true, applies SET synchronous_commit=off after each successful reconnect.
//! Throws IOException on permanent errors or after exhausting retries.
void PGExecWithRetry(PGconn *&conn, const std::string &connstring, const std::string &sql, const char *context,
                     int max_retries = 3, bool fast_mode = false);

//! Wraps name in double-quotes, escaping any embedded double-quotes by doubling.
std::string QuoteIdent(const std::string &name);

//! Wraps val in single-quotes, escaping embedded single-quotes by doubling and backslashes by doubling.
std::string QuoteLiteral(const std::string &val);

} // namespace duckdb
