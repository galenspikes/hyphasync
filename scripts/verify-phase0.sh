#!/usr/bin/env bash
# Local integration check: Postgres available; hypha_init + hypha_target_status are read-only on the remote.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DUCKDB="${ROOT_DIR}/build/release/duckdb"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"
DB_FILE="${ROOT_DIR}/build/phase0_verify.duckdb"
MARKER_TABLE="hypha_phase0_marker"

echo "==> Starting local PostgreSQL (docker compose)"
docker compose up -d --wait

echo "==> Building hyphasync extension"
make release

if [[ ! -x "$DUCKDB" ]]; then
  echo "duckdb binary not found at $DUCKDB" >&2
  exit 1
fi

echo "==> Creating marker table in PostgreSQL (baseline remote state)"
docker compose exec -T postgres psql -U hypha -d hypha_test -v ON_ERROR_STOP=1 <<SQL
CREATE TABLE IF NOT EXISTS ${MARKER_TABLE} (id INTEGER PRIMARY KEY, note VARCHAR);
DELETE FROM ${MARKER_TABLE};
INSERT INTO ${MARKER_TABLE} (id, note) VALUES (1, 'phase0-baseline');
SQL

echo "==> Running hypha_init against local Postgres URL (verifies live connection, then writes local metadata)"
rm -f "$DB_FILE"
"$DUCKDB" "$DB_FILE" <<SQL
LOAD hyphasync;
SELECT hypha_hello();
SELECT hypha_init('${PG_URL}');
SELECT target_name, conn_string FROM hypha.target WHERE target_name = 'default';
SQL

echo "==> Verifying hypha_init reports the verified connection"
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "LOAD hyphasync; SELECT hypha_init('${PG_URL}') LIKE '%connected to database%'" | grep -qi true; then
  echo "hypha_init did not report a verified connection summary" >&2
  "$DUCKDB" "$DB_FILE" -c "LOAD hyphasync; SELECT hypha_init('${PG_URL}');"
  exit 1
fi

echo "==> Verifying hypha_init fails loudly against an unreachable target (and writes nothing)"
if "$DUCKDB" ":memory:" -c "LOAD hyphasync; SELECT hypha_init('postgresql://127.0.0.1:65432/hyphasync_unreachable');" 2>/dev/null; then
  echo "hypha_init unexpectedly succeeded against an unreachable target" >&2
  exit 1
fi

echo "==> Verifying DuckDB local metadata"
"$DUCKDB" "$DB_FILE" <<'SQL'
SELECT COUNT(*)::BIGINT AS table_count
FROM information_schema.tables
WHERE table_schema = 'hypha';
SQL

echo "==> Running hypha_target_status (read-only Postgres probe)"
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT hypha_target_status('${PG_URL}') LIKE '%status=ok%'" | grep -qi true; then
  echo "hypha_target_status explicit URL did not report status=ok" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT hypha_target_status('${PG_URL}');"
  exit 1
fi
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT hypha_target_status(NULL) LIKE '%status=ok%'" | grep -qi true; then
  echo "hypha_target_status(NULL) did not report status=ok" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT hypha_target_status(NULL);"
  exit 1
fi
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT hypha_target_status(NULL) LIKE '%remote_hypha_schema=false%'" | grep -qi true; then
  echo "expected no remote hypha schema in Phase 1" >&2
  exit 1
fi

echo "==> Running hypha_base_snapshot_plan (local catalog walk — no Postgres writes)"
"$DUCKDB" "$DB_FILE" <<'SQL'
-- Create a couple of test tables to give the snapshot plan something to capture.
CREATE TABLE IF NOT EXISTS test_orders (
    id INTEGER PRIMARY KEY,
    customer VARCHAR,
    amount DECIMAL(10,2),
    created_at TIMESTAMP
);
CREATE TABLE IF NOT EXISTS test_items (
    id BIGINT,
    order_id INTEGER,
    qty SMALLINT,
    active BOOLEAN
);
INSERT OR IGNORE INTO test_orders VALUES (1, 'alice', 99.50, now());
SQL

if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT hypha_base_snapshot_plan() LIKE '%status=completed%'" | grep -qi true; then
  echo "hypha_base_snapshot_plan() did not report status=completed" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT hypha_base_snapshot_plan();"
  exit 1
fi

echo "==> Verifying snapshot tables were populated"
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.object_snapshot" | grep -qi true; then
  echo "hypha.object_snapshot is empty after base_snapshot_plan" >&2
  exit 1
fi
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.column_snapshot" | grep -qi true; then
  echo "hypha.column_snapshot is empty after base_snapshot_plan" >&2
  exit 1
fi
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.table_snapshot" | grep -qi true; then
  echo "hypha.table_snapshot is empty after base_snapshot_plan" >&2
  exit 1
fi
# Verify the commit was recorded
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.commit WHERE kind='base_snapshot_plan' AND status='completed'" | grep -qi true; then
  echo "hypha.commit missing completed base_snapshot_plan record" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT * FROM hypha.commit;"
  exit 1
fi
# Verify DuckDB→Postgres type mapping produced sensible output
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.column_snapshot WHERE postgres_type = 'integer'" | grep -qi true; then
  echo "expected at least one column mapped to postgres type 'integer'" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT table_name, column_name, duckdb_type, postgres_type FROM hypha.column_snapshot ORDER BY table_name, ordinal_position;"
  exit 1
fi

echo "==> Verifying hypha.meta was seeded (schema version + fingerprint algo)"
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT value FROM hypha.meta WHERE key='fingerprint_algo'" | grep -qx v1; then
  echo "hypha.meta missing fingerprint_algo=v1" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT * FROM hypha.meta;"
  exit 1
fi

echo "==> Verifying hypha.event_log captured init + target_status events"
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE operation='init' AND code='OK'" | grep -qi true; then
  echo "event_log missing successful init event" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT * FROM hypha.event_log;"
  exit 1
fi
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE operation='target_status'" | grep -qi true; then
  echo "event_log missing target_status event" >&2
  exit 1
fi

echo "==> Verifying the connection password was redacted in event_log"
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) > 0 FROM hypha.event_log WHERE details_json LIKE '%:***@%'" | grep -qi true; then
  echo "expected redacted password (:***@) in event_log details" >&2
  exit 1
fi
if ! "$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT COUNT(*) = 0 FROM hypha.event_log WHERE details_json LIKE '%:hypha@%'" | grep -qi true; then
  echo "raw password leaked into event_log details" >&2
  "$DUCKDB" "$DB_FILE" -c "SELECT details_json FROM hypha.event_log;"
  exit 1
fi

echo "==> Verifying PostgreSQL was not mutated by hyphasync"
docker compose exec -T postgres psql -U hypha -d hypha_test -v ON_ERROR_STOP=1 <<'SQL'
SELECT COUNT(*)::INT AS marker_rows FROM hypha_phase0_marker;
SELECT COUNT(*)::INT AS hypha_schema_tables
FROM information_schema.tables
WHERE table_schema = 'hypha';
SQL

echo "==> Phase 0/1 integration check passed"
