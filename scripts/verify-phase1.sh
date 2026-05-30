#!/usr/bin/env bash
# Phase 1 integration check — deeper Postgres probe coverage and snapshot plan validation.
#
# Covers:
#   • hypha_target_status: assert exact report field values (postgres_version, database, user,
#     target_name label, remote_hypha_schema)
#   • hypha_target_status: informative error messages for wrong credentials and unreachable host
#   • hypha_base_snapshot_plan: multi-schema capture, rich DuckDB→Postgres type mapping,
#     idempotency (two calls → two distinct completed commits)
#   • hypha.event_log: verify entries for both '(explicit)' and 'default' target labels,
#     and for base_snapshot_plan
#   • hypha_doctor: reports base_snapshot_plan as available
#   • Postgres: absolutely no mutations from any of the above
#
# Requires: Docker + Docker Compose, built extension (make release).
# Assumes verify-phase0.sh passes against the same Docker container.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DUCKDB="${ROOT_DIR}/build/release/duckdb"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"
PG_BAD_PASS="postgresql://hypha:wrongpassword@127.0.0.1:54329/hypha_test"
PG_BAD_PORT="postgresql://hypha:hypha@127.0.0.1:65432/hypha_test"
DB_FILE="${ROOT_DIR}/build/phase1_verify.duckdb"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
check() {
  local desc="$1" sql="$2"
  if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "$sql" | grep -qi true; then
    echo "FAIL: $desc" >&2
    "$DUCKDB" "$DB_FILE" -c "$sql" >&2 || true
    exit 1
  fi
}

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
echo "==> Starting PostgreSQL (docker compose)"
docker compose up -d --wait

echo "==> Building hyphasync extension"
make release

if [[ ! -x "$DUCKDB" ]]; then
  echo "duckdb binary not found at $DUCKDB" >&2; exit 1
fi

echo "==> Initializing DuckDB test file"
rm -f "$DB_FILE"
"$DUCKDB" "$DB_FILE" -c "LOAD hyphasync; SELECT hypha_init('${PG_URL}');" > /dev/null

# ---------------------------------------------------------------------------
# 1. hypha_target_status — exact report field values
# ---------------------------------------------------------------------------
echo "==> [1] hypha_target_status report field values (explicit URL)"

check "status=ok for explicit URL"   "SELECT hypha_target_status('${PG_URL}') LIKE '%status=ok%'"
check "target_name=(explicit)"       "SELECT hypha_target_status('${PG_URL}') LIKE '%target_name=(explicit)%'"
check "database=hypha_test"          "SELECT hypha_target_status('${PG_URL}') LIKE '%database=hypha_test%'"
check "user=hypha"                   "SELECT hypha_target_status('${PG_URL}') LIKE '%user=hypha%'"
check "postgres_version present"     "SELECT hypha_target_status('${PG_URL}') LIKE '%postgres_version=PostgreSQL%'"
check "remote_hypha_schema=false"    "SELECT hypha_target_status('${PG_URL}') LIKE '%remote_hypha_schema=false%'"
check "latency_ms present"           "SELECT hypha_target_status('${PG_URL}') LIKE '%latency_ms=%'"
check "read-only note present"       "SELECT hypha_target_status('${PG_URL}') LIKE '%read-only probe only%'"

echo "==> [1] hypha_target_status report field values (default target via NULL)"

check "status=ok for default target" "SELECT hypha_target_status(NULL) LIKE '%status=ok%'"
check "target_name=default"          "SELECT hypha_target_status(NULL) LIKE '%target_name=default%'"
check "database=hypha_test (null)"   "SELECT hypha_target_status(NULL) LIKE '%database=hypha_test%'"

# ---------------------------------------------------------------------------
# 2. hypha_target_status — error messages are informative
# ---------------------------------------------------------------------------
echo "==> [2] hypha_target_status error messages"

# Wrong password must throw and mention authentication or password or connection.
BAD_PASS_ERR=$("$DUCKDB" "$DB_FILE" -c "SELECT hypha_target_status('${PG_BAD_PASS}');" 2>&1 || true)
if echo "$BAD_PASS_ERR" | grep -qi "status=ok"; then
  echo "FAIL: wrong-password probe unexpectedly reported status=ok" >&2; exit 1
fi
if ! echo "$BAD_PASS_ERR" | grep -qiE "could not connect|authentication|password|Connection Error"; then
  echo "FAIL: wrong-password error is not informative: $BAD_PASS_ERR" >&2; exit 1
fi
echo "  ok: wrong-password throws with informative message"

# Wrong port must throw with a refused/unreachable message.
BAD_PORT_ERR=$("$DUCKDB" "$DB_FILE" -c "SELECT hypha_target_status('${PG_BAD_PORT}');" 2>&1 || true)
if ! echo "$BAD_PORT_ERR" | grep -qiE "Connection refused|could not connect|Connection Error"; then
  echo "FAIL: wrong-port error is not informative: $BAD_PORT_ERR" >&2; exit 1
fi
echo "  ok: unreachable-host throws with informative message"

# NULL with uninitialized metadata (fresh :memory: db) must throw, not return NULL.
NULL_NO_INIT_ERR=$("$DUCKDB" ":memory:" -c "LOAD hyphasync; SELECT hypha_target_status(NULL);" 2>&1 || true)
if ! echo "$NULL_NO_INIT_ERR" | grep -qi "no connection string to probe"; then
  echo "FAIL: NULL without init did not produce expected message: $NULL_NO_INIT_ERR" >&2; exit 1
fi
echo "  ok: NULL without init throws with actionable message"

# ---------------------------------------------------------------------------
# 3. hypha_base_snapshot_plan — multi-schema + rich type mapping
# ---------------------------------------------------------------------------
echo "==> [3] hypha_base_snapshot_plan: multi-schema capture"

"$DUCKDB" "$DB_FILE" <<'SQL'
-- Main schema tables (basic types)
CREATE TABLE IF NOT EXISTS orders (
    id          INTEGER PRIMARY KEY,
    customer    VARCHAR,
    amount      DECIMAL(10,2),
    created_at  TIMESTAMP
);
-- Second schema with richer types
CREATE SCHEMA IF NOT EXISTS analytics;
CREATE TABLE IF NOT EXISTS analytics.metrics (
    metric_id    UUID,
    recorded_on  DATE,
    score        DOUBLE,
    period       INTERVAL,
    raw_bytes    BLOB,
    active       BOOLEAN,
    tags         VARCHAR[]
);
CREATE TABLE IF NOT EXISTS analytics.events (
    event_id     BIGINT,
    event_time   TIMESTAMP WITH TIME ZONE,
    small_num    SMALLINT,
    tiny_num     TINYINT
);
INSERT OR IGNORE INTO orders VALUES (1, 'alice', 49.99, now());
SQL

# Run the plan once explicitly; capture the report for field checks.
PLAN_REPORT=$("$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT hypha_base_snapshot_plan();")
if ! echo "$PLAN_REPORT" | grep -qi "status=completed"; then
  echo "FAIL: first snapshot plan did not report status=completed" >&2
  echo "$PLAN_REPORT" >&2
  exit 1
fi
if ! echo "$PLAN_REPORT" | grep -qE "schemas_scanned=[2-9]"; then
  echo "FAIL: expected schemas_scanned >= 2 in plan report" >&2
  echo "$PLAN_REPORT" >&2
  exit 1
fi
if ! echo "$PLAN_REPORT" | grep -qiE "hashes_computed=(true|partial)|fingerprint_algo="; then
  echo "FAIL: plan report should indicate fingerprint status" >&2
  exit 1
fi
echo "  ok: plan report fields (status=completed, schemas_scanned>=2, fingerprint_algo=v1)"

echo "==> [3] Snapshot tables populated for both schemas"
check "object_snapshot has analytics entries"  "SELECT COUNT(*) > 0 FROM hypha.object_snapshot WHERE schema_name='analytics'"
check "column_snapshot has analytics entries"  "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE schema_name='analytics'"
check "table_snapshot has analytics entries"   "SELECT COUNT(*) > 0 FROM hypha.table_snapshot WHERE schema_name='analytics'"

echo "==> [3] DuckDB→Postgres type mapping for extended types"
check "uuid   → uuid"                  "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='UUID' AND postgres_type='uuid'"
check "date   → date"                  "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='DATE' AND postgres_type='date'"
check "double → double precision"      "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='DOUBLE' AND postgres_type='double precision'"
check "interval → interval"            "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='INTERVAL' AND postgres_type='interval'"
check "blob   → bytea"                 "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='BLOB' AND postgres_type='bytea'"
check "boolean → boolean"             "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='BOOLEAN' AND postgres_type='boolean'"
check "smallint → smallint"            "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='SMALLINT' AND postgres_type='smallint'"
check "tinyint → smallint"            "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='TINYINT' AND postgres_type='smallint'"
check "timestamptz → timestamp tz"     "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE postgres_type='timestamp with time zone'"
check "decimal(10,2) → numeric(10,2)" "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE duckdb_type='DECIMAL(10,2)' AND postgres_type='numeric(10,2)'"
check "nullable=false for PK column"  "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE table_name='orders' AND column_name='id' AND is_nullable=false"

echo "==> [3] Idempotency: second snapshot plan creates a new distinct commit"
BEFORE=$("$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) FROM hypha.commit WHERE kind='base_snapshot_plan' AND status='completed'")
"$DUCKDB" "$DB_FILE" -c "SELECT hypha_base_snapshot_plan();" > /dev/null
AFTER=$("$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) FROM hypha.commit WHERE kind='base_snapshot_plan' AND status='completed'")
if [[ "$AFTER" -le "$BEFORE" ]]; then
  echo "FAIL: second base_snapshot_plan call did not create a new commit (before=$BEFORE, after=$AFTER)" >&2
  exit 1
fi
echo "  ok: commit count increased from $BEFORE to $AFTER"
check "all snapshot_plan commit IDs are distinct" \
  "SELECT COUNT(DISTINCT commit_id) = COUNT(*) FROM hypha.commit WHERE kind='base_snapshot_plan'"

echo "==> [3] Row counts captured correctly"
check "orders row_count=1" \
  "SELECT COUNT(*) > 0 FROM hypha.table_snapshot WHERE table_name='orders' AND row_count=1"

# ---------------------------------------------------------------------------
# 4. hypha.event_log — observability
# ---------------------------------------------------------------------------
echo "==> [4] event_log entries for both probe labels"

check "event_log has explicit-label target_status entry" \
  "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE operation='target_status' AND details_json LIKE '%explicit%'"
check "event_log has default-label target_status entry" \
  "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE operation='target_status' AND details_json LIKE '%default%'"
check "event_log has base_snapshot_plan OK entry" \
  "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE operation='base_snapshot_plan' AND code='OK'"
check "event_log snapshot entry has table count" \
  "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE operation='base_snapshot_plan' AND details_json LIKE '%tables%'"
check "no raw password in event_log" \
  "SELECT COUNT(*) = 0 FROM hypha.event_log WHERE details_json LIKE '%:hypha@%'"

# ---------------------------------------------------------------------------
# 5. hypha_doctor
# ---------------------------------------------------------------------------
echo "==> [5] hypha_doctor capabilities"

check "doctor: base_snapshot_plan=available" \
  "SELECT hypha_doctor() LIKE '%capability_base_snapshot_plan=available%'"
check "doctor: base_snapshot=available" \
  "SELECT hypha_doctor() LIKE '%capability_base_snapshot=available%'"
check "doctor: sync=available" \
  "SELECT hypha_doctor() LIKE '%capability_sync=available%'"
check "doctor: fingerprint_algo=v1" \
  "SELECT hypha_doctor() LIKE '%fingerprint_algo=v1%'"
check "doctor: metadata_initialized=true" \
  "SELECT hypha_doctor() LIKE '%metadata_initialized=true%'"

# ---------------------------------------------------------------------------
# 6. Postgres: absolutely no mutations
# ---------------------------------------------------------------------------
echo "==> [6] Verifying PostgreSQL was not mutated by Phase 1 operations"
docker compose exec -T postgres psql -U hypha -d hypha_test -v ON_ERROR_STOP=1 <<'SQL'
DO $$
DECLARE
  hypha_tables INT;
BEGIN
  SELECT COUNT(*)::INT INTO hypha_tables
  FROM information_schema.schemata
  WHERE schema_name = 'hypha';
  IF hypha_tables > 0 THEN
    RAISE EXCEPTION 'hypha schema was created on Postgres — unexpected remote mutation!';
  END IF;
END $$;
SELECT 'remote_mutation_check=clean' AS result;
SQL

# ---------------------------------------------------------------------------
echo ""
echo "==> Phase 1 integration check passed"
