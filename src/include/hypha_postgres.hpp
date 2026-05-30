#pragma once

#include <cstdint>
#include <libpq-fe.h>
#include <string>

namespace duckdb {

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
};

//! Validates conninfo syntax and attempts a live connection. Read-only; gathers version/db/user on success.
HyphaConnectionProbe ProbeHyphaConnection(const std::string &conn_string);

//! Opens a validated, connected PGconn ready for use. Appends a 5-second connect timeout if not
//! already present. Throws ConnectionException / InvalidInputException / IOException on any failure.
//! The caller MUST call PQfinish() on the returned pointer when done.
PGconn *OpenHyphaConnection(const std::string &conn_string);

//! Masks the password in a Postgres connection string (URL userinfo and password= keyword/query forms)
//! so it is safe to print in reports, logs, and error messages.
std::string RedactConnString(const std::string &conn_string);

//! Read-only Postgres connectivity / capability probe (libpq). Does not mutate the remote database.
//! Throws on hard failures (empty/invalid conninfo, or no connection). On a successful connection it
//! returns a multi-line status report (status=ok when every probe succeeded, status=degraded when
//! connected but one or more catalog probes failed). Backs the hypha_target_status() SQL function.
//! `target_label` is reported verbatim as the report's target_name (e.g. "default" or "(explicit)").
std::string RunHyphaTargetStatus(const std::string &conn_string, const std::string &target_label);

} // namespace duckdb
