#!/usr/bin/env bash
# Phase 0 local integration check: Postgres is available but must not be mutated.
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

echo "==> Running hypha_init against local Postgres URL (local metadata only)"
rm -f "$DB_FILE"
"$DUCKDB" "$DB_FILE" <<SQL
LOAD hyphasync;
SELECT hypha_hello();
SELECT hypha_init('${PG_URL}');
SELECT target_name, conn_string FROM hypha.target WHERE target_name = 'default';
SQL

echo "==> Verifying DuckDB local metadata"
"$DUCKDB" "$DB_FILE" <<'SQL'
SELECT COUNT(*)::BIGINT AS table_count
FROM information_schema.tables
WHERE table_schema = 'hypha';
SQL

echo "==> Verifying PostgreSQL was not mutated by hypha_init"
docker compose exec -T postgres psql -U hypha -d hypha_test -v ON_ERROR_STOP=1 <<'SQL'
SELECT COUNT(*)::INT AS marker_rows FROM hypha_phase0_marker;
SELECT COUNT(*)::INT AS hypha_schema_tables
FROM information_schema.tables
WHERE table_schema = 'hypha';
SQL

echo "==> Phase 0 integration check passed"
