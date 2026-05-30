#pragma once

#include <string>

namespace duckdb {

//! Read-only Postgres connectivity / capability probe (libpq). Does not mutate the remote database.
std::string RunHyphaAttachCheck(const std::string &conn_string);

} // namespace duckdb
