#!/usr/bin/env bash
# hyphasync integration test suite
# Runs against a native Postgres 16 instance (no Docker required).
# Usage: ./test/integration/run.sh [--no-build]
#
# Organized into numbered sections. Every assertion uses the `check` helper which
# prints PASS/FAIL and exits 1 on any failure.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

DUCKDB="${ROOT_DIR}/build/release/duckdb"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"
# Use a non-existent user rather than a wrong password: the local dev Postgres
# uses trust authentication, so any password is accepted for the 'hypha' user.
# A non-existent role is rejected even under trust auth.
PG_BAD_CRED="postgresql://no_such_hypha_user_xyz:wrongpassword@127.0.0.1:54329/hypha_test"
PG_BAD_PORT="postgresql://hypha:hypha@127.0.0.1:65432/hypha_test"
DB_FILE="${ROOT_DIR}/build/integration_test.duckdb"
PG_SCHEMA="integration_test_main"   # MakePostgresSchemaName(integration_test, main)
PASS=0
FAIL=0

# Native psql — prefer postgresql@16 from Homebrew, fall back to PATH
PSQL="$(command -v psql 2>/dev/null || echo "")"
for candidate in \
    /opt/homebrew/opt/postgresql@16/bin/psql \
    /usr/local/opt/postgresql@16/bin/psql \
    /opt/homebrew/bin/psql; do
    [[ -x "$candidate" ]] && { PSQL="$candidate"; break; }
done
[[ -n "$PSQL" ]] || { echo "psql not found — install postgresql@16 via Homebrew" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

section() { echo; echo "──────────────────────────────────────────────────────"; echo "  $1"; echo "──────────────────────────────────────────────────────"; }

check() {
    local desc="$1" sql="$2"
    local result
    result=$("$DUCKDB" "$DB_FILE" -noheader -list -c "$sql" 2>/dev/null || echo "ERROR")
    if echo "$result" | grep -qi true; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc"
        echo "       SQL: $sql"
        echo "       Got: $result"
        FAIL=$((FAIL + 1))
    fi
}

check_throws() {
    local desc="$1" sql="$2" pattern="$3"
    local result
    result=$("$DUCKDB" "$DB_FILE" -c "$sql" 2>&1 || true)
    if echo "$result" | grep -qiE "$pattern"; then
        echo -e "  ${GREEN}PASS${NC}: $desc (throws: $pattern)"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc — expected error matching '$pattern'"
        echo "       Got: $result"
        FAIL=$((FAIL + 1))
    fi
}

pg_check() {
    local desc="$1" sql="$2" expected="$3"
    local result
    result=$("$PSQL" "$PG_URL" -tAc "$sql" 2>/dev/null || echo "PG_ERROR")
    if [[ "$result" == "$expected" ]]; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc"
        echo "       Expected: $expected"
        echo "       Got:      $result"
        FAIL=$((FAIL + 1))
    fi
}

pg_check_contains() {
    local desc="$1" sql="$2" pattern="$3"
    local result
    result=$("$PSQL" "$PG_URL" -tAc "$sql" 2>/dev/null || echo "PG_ERROR")
    if echo "$result" | grep -qiE "$pattern"; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc"
        echo "       Expected pattern: $pattern"
        echo "       Got: $result"
        FAIL=$((FAIL + 1))
    fi
}

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
section "Setup"

# Try native Postgres first; fall back to Docker Compose
if ! pg_isready -h 127.0.0.1 -p 54329 -q 2>/dev/null; then
  echo "No native Postgres on 54329; attempting to start via docker compose..."
  if command -v docker &>/dev/null; then
    docker compose up -d postgres
    sleep 3
    STARTED_DOCKER=true
  else
    echo "ERROR: No Postgres on 54329 and Docker not available"
    exit 1
  fi
fi
trap '[ "${STARTED_DOCKER:-}" = true ] && docker compose down' EXIT

echo "Verifying native Postgres on port 54329..."
"$PSQL" "$PG_URL" -c "SELECT 1;" > /dev/null 2>&1 || {
    echo "ERROR: cannot connect to $PG_URL" >&2
    echo "Start with: brew services start postgresql@16" >&2
    exit 1
}
echo "Postgres OK ($("$PSQL" "$PG_URL" -tAc 'SELECT version()' | head -1))"

# Redirect CSV staging to external drive when available
if [[ -d /Volumes/alpha/tmp ]]; then
    export TMPDIR=/Volumes/alpha/tmp
fi

if [[ "${1:-}" != "--no-build" ]]; then
    echo "Building hyphasync extension..."
    CC=gcc CXX=g++ make release -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
fi

[[ -x "$DUCKDB" ]] || { echo "duckdb binary not found at $DUCKDB" >&2; exit 1; }

rm -f "$DB_FILE"
echo "Test DB: $DB_FILE"

# Clean up any prior test state on both DuckDB and Postgres
"$PSQL" "$PG_URL" -c "
DROP SCHEMA IF EXISTS integration_test_main CASCADE;
DROP SCHEMA IF EXISTS hypha CASCADE;
DROP TABLE IF EXISTS hypha_integration_marker;
" > /dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# 01 — Extension loading and doctor
# ---------------------------------------------------------------------------
section "01 — Extension loading and doctor"

"$DUCKDB" "$DB_FILE" -c "SELECT hypha_hello();" > /dev/null

check "doctor: metadata_initialized=false before init" \
    "SELECT hypha_doctor() LIKE '%metadata_initialized=false%'"
check "doctor: local_database present" \
    "SELECT hypha_doctor() LIKE '%local_database=%'"
check "doctor: fingerprint_algo=v3" \
    "SELECT hypha_doctor() LIKE '%fingerprint_algo=v3%'"
check "doctor: all capabilities available" \
    "SELECT hypha_doctor() LIKE '%capability_init=available%'
       AND hypha_doctor() LIKE '%capability_base_snapshot_plan=available%'
       AND hypha_doctor() LIKE '%capability_sync=available%'"
check "doctor: nested_types shipped" \
    "SELECT hypha_doctor() LIKE '%nested_types=%'"
check "doctor: schema_evolution shipped" \
    "SELECT hypha_doctor() LIKE '%schema_evolution=%'"
check "doctor: remote_metadata shipped" \
    "SELECT hypha_doctor() LIKE '%remote_metadata=%'"
check "doctor: row_level_diff shipped" \
    "SELECT hypha_doctor() LIKE '%row_level_diff=%'"

# ---------------------------------------------------------------------------
# 02 — hypha_init: validation and error handling
# ---------------------------------------------------------------------------
section "02 — hypha_init"

"$DUCKDB" "$DB_FILE" -c "SELECT hypha_init('${PG_URL}');" > /dev/null

check "init: metadata_initialized=true after init" \
    "SELECT hypha_doctor() LIKE '%metadata_initialized=true%'"
check "init: hypha schema created locally" \
    "SELECT COUNT(*)::BIGINT > 0 FROM information_schema.schemata WHERE schema_name='hypha'"
check "init: default target stored" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.target WHERE target_name='default'"
check "init: event_log has OK entry" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE operation='init' AND code='OK'"
check "init: no raw password in event_log" \
    "SELECT COUNT(*)::BIGINT = 0 FROM hypha.event_log WHERE details_json LIKE '%:hypha@%'"
check "init: redacted password in event_log" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE details_json LIKE '%:***@%'"

check_throws "init: NULL rejected" \
    "SELECT hypha_init(NULL);" "requires a Postgres connection string"
check_throws "init: unreachable host throws" \
    "SELECT hypha_init('${PG_BAD_PORT}');" "could not connect"
check_throws "init: bad credentials (non-existent user) throws" \
    "SELECT hypha_init('${PG_BAD_CRED}');" "could not connect"

# ---------------------------------------------------------------------------
# 03 — hypha_target_status
# ---------------------------------------------------------------------------
section "03 — hypha_target_status"

check "target_status: explicit URL → status=ok" \
    "SELECT hypha_target_status('${PG_URL}') LIKE '%status=ok%'"
check "target_status: explicit URL → target_name=(explicit)" \
    "SELECT hypha_target_status('${PG_URL}') LIKE '%target_name=(explicit)%'"
check "target_status: explicit URL → database=hypha_test" \
    "SELECT hypha_target_status('${PG_URL}') LIKE '%database=hypha_test%'"
check "target_status: explicit URL → user=hypha" \
    "SELECT hypha_target_status('${PG_URL}') LIKE '%user=hypha%'"
check "target_status: explicit URL → postgres_version present" \
    "SELECT hypha_target_status('${PG_URL}') LIKE '%postgres_version=PostgreSQL%'"
check "target_status: explicit URL → latency_ms present" \
    "SELECT hypha_target_status('${PG_URL}') LIKE '%latency_ms=%'"
check "target_status: default target (NULL) → status=ok" \
    "SELECT hypha_target_status(NULL) LIKE '%status=ok%'"
check "target_status: default target → target_name=default" \
    "SELECT hypha_target_status(NULL) LIKE '%target_name=default%'"
check "target_status: remote_hypha_schema=false before push" \
    "SELECT hypha_target_status(NULL) LIKE '%remote_hypha_schema=false%'"
check "target_status: remote_hypha_sync_log=false before push" \
    "SELECT hypha_target_status(NULL) LIKE '%remote_hypha_sync_log=false%'"

check_throws "target_status: bad port throws" \
    "SELECT hypha_target_status('${PG_BAD_PORT}');" "could not connect"
check_throws "target_status: malformed connstring throws" \
    "SELECT hypha_target_status('not valid = = =');" "invalid connection string"

# ---------------------------------------------------------------------------
# 04 — hypha_base_snapshot_plan: type mapping + fingerprinting
# ---------------------------------------------------------------------------
section "04 — hypha_base_snapshot_plan: type mapping and fingerprinting"

# json extension is auto-managed by hypha_base_snapshot_plan(); no manual INSTALL needed.
"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE scalars (
    id        INTEGER PRIMARY KEY,
    name      VARCHAR NOT NULL,
    score     DOUBLE,
    amount    DECIMAL(10,2),
    active    BOOLEAN,
    created   TIMESTAMP,
    stamp_tz  TIMESTAMPTZ,
    day       DATE,
    uid       UUID DEFAULT gen_random_uuid(),
    raw       BLOB,
    span      INTERVAL,
    small     SMALLINT,
    tiny      TINYINT,
    big       BIGINT
);
CREATE TABLE no_pk_table (x INTEGER, y VARCHAR);
CREATE TABLE nested_table (
    id     INTEGER PRIMARY KEY,
    tags   VARCHAR[],
    meta   STRUCT(source VARCHAR, ver INT),
    props  MAP(VARCHAR, VARCHAR)
);
INSERT INTO scalars VALUES (1,'alice',9.5,99.99,true,now(),now(),'2026-01-01'::DATE,gen_random_uuid(),'\xDEAD'::BLOB,'1 day',1,1,1000000);
INSERT INTO no_pk_table VALUES (1,'a'),(2,'b');
INSERT INTO nested_table VALUES
    (1, ['x','y'], {source:'api',ver:1}, map {'k':'v'}),
    (2, ['z'],     {source:'db',ver:2},  map {'a':'b','c':'d'});
SELECT hypha_base_snapshot_plan();
" > /dev/null

check "plan: status=completed" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.commit WHERE kind='base_snapshot_plan' AND status='completed'"
check "plan: object_snapshot populated" \
    "SELECT COUNT(*)::BIGINT >= 3 FROM hypha.object_snapshot"
check "plan: fingerprint_algo=v3 on commit" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.commit WHERE fingerprint_algo='v3'"
check "plan: all tables have table_hash (fingerprinting worked)" \
    "SELECT COUNT(*)::BIGINT = 3 FROM hypha.table_snapshot WHERE table_hash IS NOT NULL"

# Scalar type mapping
check "type: INTEGER → integer"          "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='INTEGER' AND postgres_type='integer'"
check "type: DOUBLE → double precision"  "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='DOUBLE' AND postgres_type='double precision'"
check "type: DECIMAL(10,2) → numeric"   "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='DECIMAL(10,2)' AND postgres_type='numeric(10,2)'"
check "type: BOOLEAN → boolean"         "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='BOOLEAN' AND postgres_type='boolean'"
check "type: TIMESTAMP → timestamp"     "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='TIMESTAMP' AND postgres_type LIKE '%timestamp%'"
check "type: UUID → uuid"               "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='UUID' AND postgres_type='uuid'"
check "type: BLOB → bytea"              "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='BLOB' AND postgres_type='bytea'"
check "type: INTERVAL → interval"       "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='INTERVAL' AND postgres_type='interval'"
check "type: SMALLINT → smallint"       "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='SMALLINT' AND postgres_type='smallint'"
check "type: BIGINT → bigint"           "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE duckdb_type='BIGINT' AND postgres_type='bigint'"
# NOT NULL preserved
check "type: PK column is NOT NULL"     "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE table_name='scalars' AND column_name='id' AND is_nullable=false"

# Nested type mapping
check "type: VARCHAR[] → jsonb"   "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE table_name='nested_table' AND column_name='tags' AND postgres_type='jsonb'"
check "type: STRUCT → jsonb"      "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE table_name='nested_table' AND column_name='meta' AND postgres_type='jsonb'"
check "type: MAP → jsonb"         "SELECT COUNT(*)::BIGINT > 0 FROM hypha.column_snapshot WHERE table_name='nested_table' AND column_name='props' AND postgres_type='jsonb'"

# Idempotency: second plan call creates new commit
"$DUCKDB" "$DB_FILE" -c "SELECT hypha_base_snapshot_plan();" > /dev/null
check "plan: idempotent (two distinct completed commits)" \
    "SELECT COUNT(DISTINCT commit_id)::BIGINT >= 2 FROM hypha.commit WHERE kind='base_snapshot_plan' AND status='completed'"

# ---------------------------------------------------------------------------
# 05 — hypha_base_snapshot: push to Postgres
# ---------------------------------------------------------------------------
section "05 — hypha_base_snapshot: full push"

"$DUCKDB" "$DB_FILE" -c "SELECT * FROM hypha_base_snapshot();" > /dev/null

check "push: commit marked applied" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.commit WHERE status IN ('applied', 'partial')"
check "push: event_log has base_snapshot OK" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE operation='base_snapshot' AND code='OK'"
check "push: tables_synced >= 3" \
    "SELECT (SELECT COUNT(*)::BIGINT FROM hypha.commit WHERE status='applied') > 0"

pg_check "Postgres: scalars table exists" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='integration_test_main' AND table_name='scalars'" "1"
pg_check "Postgres: scalars has expected columns" \
    "SELECT COUNT(*)::INT FROM information_schema.columns WHERE table_schema='integration_test_main' AND table_name='scalars'" "14"
pg_check "Postgres: scalars data landed (3 → wait, 1 row inserted)" \
    "SELECT COUNT(*)::INT FROM integration_test_main.scalars" "1"
pg_check "Postgres: nested_table exists" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='integration_test_main' AND table_name='nested_table'" "1"
pg_check "Postgres: nested_table data landed" \
    "SELECT COUNT(*)::INT FROM integration_test_main.nested_table" "2"
pg_check_contains "Postgres: nested tags is valid jsonb" \
    "SELECT tags FROM integration_test_main.nested_table WHERE id=1" '"x"'
pg_check_contains "Postgres: nested meta is valid jsonb" \
    "SELECT meta->>'source' FROM integration_test_main.nested_table WHERE id=1" "api"
pg_check_contains "Postgres: nested props is valid jsonb" \
    "SELECT props->>'k' FROM integration_test_main.nested_table WHERE id=1" "v"
pg_check "Postgres: no_pk_table exists and has data" \
    "SELECT COUNT(*)::INT FROM integration_test_main.no_pk_table" "2"

# Remote hypha metadata
pg_check "Postgres: hypha.sync_log populated" \
    "SELECT COUNT(*)::INT FROM hypha.sync_log WHERE operation_kind='base_snapshot'" "1"
pg_check "Postgres: hypha.object_state has all tables" \
    "SELECT COUNT(*)::INT FROM hypha.object_state WHERE pg_schema='integration_test_main'" "3"
pg_check "Postgres: hypha.object_state has fingerprints" \
    "SELECT COUNT(*)::INT FROM hypha.object_state WHERE definition_hash IS NOT NULL" "3"

check "target_status: remote_hypha_sync_log=true after push" \
    "SELECT hypha_target_status(NULL) LIKE '%remote_hypha_sync_log=true%'"
check "target_status: remote_hypha_object_state=true after push" \
    "SELECT hypha_target_status(NULL) LIKE '%remote_hypha_object_state=true%'"

# ---------------------------------------------------------------------------
# 06 — hypha_sync: incremental sync
# ---------------------------------------------------------------------------
section "06 — hypha_sync: incremental sync"

# 06a: likely-unchanged (no changes — expect 0 synced)
"$DUCKDB" "$DB_FILE" -c "SELECT * FROM hypha_sync();" > /dev/null
check "sync: no changes → tables_synced=0 on second run (all LIKELY_UNCHANGED)" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.commit WHERE kind='sync' AND status='applied'"

# 06b: data change on a PK table
"$DUCKDB" "$DB_FILE" -c "
INSERT INTO scalars VALUES (2,'bob',8.0,79.99,false,now(),now(),'2026-01-02'::DATE,gen_random_uuid(),'\xBEEF'::BLOB,'2 hours',2,2,2000000);
UPDATE scalars SET name='ALICE' WHERE id=1;
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "sync: data change — scalars has 2 rows" \
    "SELECT COUNT(*)::INT FROM integration_test_main.scalars" "2"
pg_check_contains "sync: update applied — alice → ALICE" \
    "SELECT name FROM integration_test_main.scalars WHERE id=1" "ALICE"

# 06c: new table
"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE new_table (id INTEGER PRIMARY KEY, val VARCHAR);
INSERT INTO new_table VALUES (1,'hello');
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "sync: new table created on Postgres" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='integration_test_main' AND table_name='new_table'" "1"
pg_check "sync: new table has data" \
    "SELECT COUNT(*)::INT FROM integration_test_main.new_table" "1"

# 06d: dropped table
"$DUCKDB" "$DB_FILE" -c "DROP TABLE new_table; SELECT * FROM hypha_sync();" > /dev/null
pg_check "sync: dropped table removed from Postgres" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='integration_test_main' AND table_name='new_table'" "0"

# 06e: no-PK table data change uses TRUNCATE+COPY (logged)
"$DUCKDB" "$DB_FILE" -c "
INSERT INTO no_pk_table VALUES (3,'c');
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "sync: no-PK table data synced" \
    "SELECT COUNT(*)::INT FROM integration_test_main.no_pk_table" "3"
check "sync: no-PK TRUNCATE_COPY logged to event_log" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE code='TRUNCATE_COPY'"
check "sync: tables_truncate_copy reported in sync note" \
    "SELECT (SELECT commit_id FROM hypha.commit WHERE kind='sync' ORDER BY created_at DESC LIMIT 1) IS NOT NULL"

# ---------------------------------------------------------------------------
# 07 — Schema evolution: ADD/DROP COLUMN
# ---------------------------------------------------------------------------
section "07 — Schema evolution via ALTER TABLE"

# 07a: drop-only (no COPY needed)
"$DUCKDB" "$DB_FILE" -c "ALTER TABLE scalars DROP COLUMN span; SELECT * FROM hypha_sync();" > /dev/null
pg_check "schema: DROP column — 'span' removed" \
    "SELECT COUNT(*)::INT FROM information_schema.columns WHERE table_schema='integration_test_main' AND table_name='scalars' AND column_name='span'" "0"
pg_check "schema: DROP column — other columns preserved" \
    "SELECT COUNT(*)::INT FROM information_schema.columns WHERE table_schema='integration_test_main' AND table_name='scalars' AND column_name='name'" "1"
pg_check "schema: DROP column — data preserved (2 rows)" \
    "SELECT COUNT(*)::INT FROM integration_test_main.scalars" "2"

# 07b: add-only (TRUNCATE+COPY to repopulate)
"$DUCKDB" "$DB_FILE" -c "ALTER TABLE scalars ADD COLUMN label VARCHAR DEFAULT 'default'; SELECT * FROM hypha_sync();" > /dev/null
pg_check "schema: ADD column — 'label' exists on Postgres" \
    "SELECT COUNT(*)::INT FROM information_schema.columns WHERE table_schema='integration_test_main' AND table_name='scalars' AND column_name='label'" "1"
pg_check "schema: ADD column — data repopulated (2 rows)" \
    "SELECT COUNT(*)::INT FROM integration_test_main.scalars" "2"

# 07c: type change forces DROP+CREATE (not ALTER)
"$DUCKDB" "$DB_FILE" -c "
DROP TABLE scalars;
CREATE TABLE scalars (id INTEGER PRIMARY KEY, name TEXT, score TEXT);
INSERT INTO scalars VALUES (1,'alice','A'),(2,'bob','B');
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "schema: type change — table recreated" \
    "SELECT COUNT(*)::INT FROM information_schema.columns WHERE table_schema='integration_test_main' AND table_name='scalars'" "3"

# ---------------------------------------------------------------------------
# 08 — Postgres not mutated by read-only operations
# ---------------------------------------------------------------------------
section "08 — Read-only probe leaves Postgres clean"

"$DUCKDB" "$DB_FILE" -c "SELECT hypha_target_status(NULL);" > /dev/null
"$DUCKDB" "$DB_FILE" -c "SELECT hypha_base_snapshot_plan();" > /dev/null
"$DUCKDB" "$DB_FILE" -c "SELECT hypha_sync_plan();" > /dev/null

pg_check "probe-only: extra snapshot_plan calls don't mutate Postgres tables" \
    "SELECT COUNT(*)::INT FROM integration_test_main.scalars" "2"

# ---------------------------------------------------------------------------
# 09 — Composite primary key snapshot + sync
# ---------------------------------------------------------------------------
section "09 — Composite primary key snapshot + sync"

"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE region_sales (region_id INT, store_id INT, revenue DOUBLE, PRIMARY KEY (region_id, store_id));
INSERT INTO region_sales VALUES (1, 101, 5000.0), (1, 102, 6000.0), (2, 201, 7000.0);
SELECT * FROM hypha_base_snapshot();
" > /dev/null
pg_check "composite PK: region_sales created on Postgres" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='${PG_SCHEMA}' AND table_name='region_sales'" "1"
pg_check "composite PK: 3 rows synced" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.region_sales" "3"

"$DUCKDB" "$DB_FILE" -c "
UPDATE region_sales SET revenue = 9999.0 WHERE region_id = 1 AND store_id = 101;
SELECT * FROM hypha_sync();
" > /dev/null
pg_check_contains "composite PK: updated revenue synced" \
    "SELECT revenue::TEXT FROM ${PG_SCHEMA}.region_sales WHERE region_id = 1 AND store_id = 101" "9999"

# ---------------------------------------------------------------------------
# 10 — Large table (150k rows): MUTABLE_ENTITY / APPEND_ONLY path
# ---------------------------------------------------------------------------
section "10 — Large table (150k rows): fingerprint strategy"

"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE big_table AS SELECT range AS id FROM range(150000);
SELECT * FROM hypha_base_snapshot();
" > /dev/null
pg_check "large table: 150 000 rows in Postgres" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.big_table" "150000"
check "large table: table_hash computed for big_table" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.table_snapshot WHERE table_name='big_table' AND table_hash IS NOT NULL"
check "large table: object_snapshot entry present" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.object_snapshot WHERE object_name='big_table'"

# ---------------------------------------------------------------------------
# 11 — max_rows_per_table guard
# ---------------------------------------------------------------------------
section "11 — max_rows_per_table guard"

DB_LIMIT="${ROOT_DIR}/build/integration_test_limit.duckdb"
PG_LIMIT_SCHEMA="integration_test_limit_main"
rm -f "$DB_LIMIT"
"$PSQL" "$PG_URL" -c "DROP SCHEMA IF EXISTS ${PG_LIMIT_SCHEMA} CASCADE;" > /dev/null 2>&1 || true

"$DUCKDB" "$DB_LIMIT" -c "
SELECT hypha_init('${PG_URL}', 100);
CREATE TABLE over_limit AS SELECT range AS id FROM range(200);
CREATE TABLE under_limit AS SELECT range AS id FROM range(50);
SELECT * FROM hypha_base_snapshot();
" > /dev/null

pg_check "max_rows: under_limit reaches Postgres" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='${PG_LIMIT_SCHEMA}' AND table_name='under_limit'" "1"
pg_check "max_rows: under_limit has 50 rows" \
    "SELECT COUNT(*)::INT FROM ${PG_LIMIT_SCHEMA}.under_limit" "50"
pg_check "max_rows: over_limit excluded from Postgres" \
    "SELECT COUNT(*)::INT FROM information_schema.tables WHERE table_schema='${PG_LIMIT_SCHEMA}' AND table_name='over_limit'" "0"

# Check skip was recorded in the limit DB's local event_log
_orig_db="$DB_FILE"
DB_FILE="$DB_LIMIT"
check "max_rows: skip or exclusion logged in event_log" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE code LIKE '%SKIP%' OR message LIKE '%max_row%' OR message LIKE '%over_limit%' OR details_json LIKE '%over_limit%'"
DB_FILE="$_orig_db"

# ---------------------------------------------------------------------------
# 12 — Network interruption simulation (resilience)
# ---------------------------------------------------------------------------
section "12 — Network interruption simulation (resilience)"

"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE interrupt_test AS SELECT range AS id, random()::VARCHAR AS v FROM range(10000);
SELECT * FROM hypha_base_snapshot();
" > /dev/null
pg_check "interruption: interrupt_test base snapshot ok (10 000 rows)" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.interrupt_test" "10000"

"$DUCKDB" "$DB_FILE" -c "
UPDATE interrupt_test SET v = 'changed' WHERE id < 1000;
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "interruption: 1000 changed rows synced" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.interrupt_test WHERE v = 'changed'" "1000"
check "interruption: sync completed without error" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE operation='sync' AND code='OK'"

# ---------------------------------------------------------------------------
# 13 — hypha_verify: blind-spot tripwire
# ---------------------------------------------------------------------------
section "13 — hypha_verify: blind-spot tripwire"

# A dedicated table for the tripwire so it is independent of earlier sections.
"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE verify_demo AS SELECT range AS n, ('v' || range)::VARCHAR AS label FROM range(100);
SELECT * FROM hypha_base_snapshot();
" > /dev/null

# First verify arms the baseline and writes hypha.verify_state.
check "verify: returns a one-line summary" \
    "SELECT hypha_verify() LIKE 'verify:%'"
check "verify: verify_state populated" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.verify_state WHERE object_name='verify_demo'"
check "verify: VERIFY_SUMMARY logged to event_log" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE code='VERIFY_SUMMARY'"

# With no intervening change, a re-run reports zero drift (single evaluation per call).
check "verify: clean re-run reports 0 drift" \
    "SELECT hypha_verify() LIKE '%0 drift%'"

# A change the fast fingerprint DOES catch is flagged (PENDING, not silently clean).
# NOTE: one hypha_verify() call per check — it advances the baseline each run.
"$DUCKDB" "$DB_FILE" -c "UPDATE verify_demo SET label='changed' WHERE n=1;" > /dev/null
check "verify: in-place change is flagged (not 0 drift · 0 pending)" \
    "SELECT hypha_verify() NOT LIKE '%0 drift \xc2\xb7 0 pending%'"

# ---------------------------------------------------------------------------
# 14 — exact_verify mode forces the EXACT strategy
# ---------------------------------------------------------------------------
section "14 — exact_verify mode forces EXACT fingerprinting"

DB_EXACT="${ROOT_DIR}/build/integration_test_exact.duckdb"
rm -f "$DB_EXACT"

# 50 000 rows × ~33 bytes ≈ 1.6 MB > 1 MB budget, and no id/timestamp column → this table
# would normally classify as MUTABLE_ENTITY. exact_verify must override that to EXACT.
"$DUCKDB" "$DB_EXACT" -c "
SELECT hypha_init('${PG_URL}', 0, false, true);
CREATE TABLE wide_blob AS SELECT range AS n, random()::VARCHAR AS payload FROM range(50000);
SELECT hypha_base_snapshot_plan();
" > /dev/null

_orig_db="$DB_FILE"
DB_FILE="$DB_EXACT"
check "exact_verify: exact_verify flag persisted in hypha.meta" \
    "SELECT value='true' FROM hypha.meta WHERE key='exact_verify'"
check "exact_verify: EXACT_VERIFY event logged" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE code='EXACT_VERIFY'"
check "exact_verify: large table classified EXACT (not MUTABLE_ENTITY)" \
    "SELECT fingerprint_strategy='EXACT' FROM hypha.table_snapshot WHERE table_name='wide_blob'"
DB_FILE="$_orig_db"

# ---------------------------------------------------------------------------
# 15 — keyless table: append-only fast path + delete/update fallback
# ---------------------------------------------------------------------------
section "15 — keyless table: append-only fast path"

# A table with no PRIMARY KEY → keyless.
"$DUCKDB" "$DB_FILE" -c "
CREATE TABLE keyless_log AS SELECT range AS seqno, ('e' || range)::VARCHAR AS payload FROM range(100);
SELECT * FROM hypha_base_snapshot();
" > /dev/null
pg_check "keyless: base snapshot pushes 100 rows" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.keyless_log" "100"

# Insert-only change → append-only fast path (KEYLESS_APPEND, no TRUNCATE, no duplicates).
"$DUCKDB" "$DB_FILE" -c "
INSERT INTO keyless_log SELECT range AS seqno, ('e' || range)::VARCHAR FROM range(100, 150);
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "keyless append: Postgres has exactly 150 rows (no duplicates)" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.keyless_log" "150"
check "keyless append: KEYLESS_APPEND logged for keyless_log" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE code='KEYLESS_APPEND' AND message LIKE '%keyless_log%'"
check "keyless append: no TRUNCATE_COPY used for the insert-only change" \
    "SELECT COUNT(*)::BIGINT = 0 FROM hypha.event_log WHERE code='TRUNCATE_COPY' AND message LIKE '%keyless_log%'"

# Delete + update → must fall back to TRUNCATE+COPY and reach the correct full state.
"$DUCKDB" "$DB_FILE" -c "
DELETE FROM keyless_log WHERE seqno < 10;
UPDATE keyless_log SET payload='changed' WHERE seqno = 50;
SELECT * FROM hypha_sync();
" > /dev/null
pg_check "keyless fallback: 140 rows after deleting 10" \
    "SELECT COUNT(*)::INT FROM ${PG_SCHEMA}.keyless_log" "140"
pg_check "keyless fallback: update propagated via full re-copy" \
    "SELECT payload FROM ${PG_SCHEMA}.keyless_log WHERE seqno=50" "changed"
check "keyless fallback: TRUNCATE_COPY logged for keyless_log" \
    "SELECT COUNT(*)::BIGINT > 0 FROM hypha.event_log WHERE code='TRUNCATE_COPY' AND message LIKE '%keyless_log%'"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "══════════════════════════════════════════════════════"
TOTAL=$((PASS + FAIL))
if [[ $FAIL -eq 0 ]]; then
    echo -e "  ${GREEN}ALL $TOTAL TESTS PASSED${NC}"
else
    echo -e "  ${RED}$FAIL / $TOTAL TESTS FAILED${NC} (${GREEN}$PASS passed${NC})"
fi
echo "══════════════════════════════════════════════════════"

[[ $FAIL -eq 0 ]]
