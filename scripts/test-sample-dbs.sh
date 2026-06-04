#!/opt/homebrew/bin/bash
# Full end-to-end test harness: runs hypha_init + hypha_base_snapshot against
# every DuckDB database in testdata/ and verifies what landed on Postgres.
# Reports per-DB timing, row counts, skipped tables, type coverage, and event_log.
# Results are persisted to testdata/results/ as JSONL + a summary JSON.
#
# Usage:
#   ./scripts/test-sample-dbs.sh                  # test all DBs in testdata/
#   ./scripts/test-sample-dbs.sh --no-teardown     # (no-op; native Postgres stays up)
#   ./scripts/test-sample-dbs.sh --db foo.duckdb   # test a single DB
#   ./scripts/test-sample-dbs.sh --no-build        # skip 'make release' step
#
# Requires: native postgresql@16 running on port 54329, built extension (make release).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DUCKDB="${ROOT_DIR}/build/release/duckdb"
TESTDATA="${ROOT_DIR}/testdata"
RESULTS_DIR="${ROOT_DIR}/testdata/results"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"

# ── Argument parsing
NO_TEARDOWN=false
SINGLE_DB=""
NO_BUILD=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-teardown) NO_TEARDOWN=true  ;;
    --no-build)    NO_BUILD=true     ;;
    --db)          SINGLE_DB="$2"; shift ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
  shift
done

[[ -x "$DUCKDB" ]] || { echo "No duckdb binary — run 'CC=gcc CXX=g++ make release' first." >&2; exit 1; }

mkdir -p "$RESULTS_DIR"

# ── Native psql — prefer postgresql@16 from Homebrew, fall back to PATH
PSQL="$(command -v psql 2>/dev/null || echo "")"
for _candidate in \
    /opt/homebrew/opt/postgresql@16/bin/psql \
    /usr/local/opt/postgresql@16/bin/psql \
    /opt/homebrew/bin/psql; do
    [[ -x "$_candidate" ]] && { PSQL="$_candidate"; break; }
done
[[ -n "$PSQL" ]] || { echo "psql not found — install postgresql@16 via Homebrew" >&2; exit 1; }

# ── Helpers
query()  { "$DUCKDB" "$1" -noheader -list -c "$2" 2>/dev/null || echo ""; }
pgq()    { "$PSQL" "$PG_URL" -At -c "$1" 2>/dev/null || echo ""; }
now_ms() { python3 -c "import time; print(int(time.time() * 1000))"; }
now_iso(){ date -u '+%Y-%m-%dT%H:%M:%SZ'; }
safe()   { local v="$1"; [[ -z "$v" ]] && echo "?" || echo "$v"; }

# ── Results file: one JSONL per run, filename = ISO timestamp
RUN_TS="$(date -u '+%Y-%m-%dT%H-%M-%SZ')"
JSONL_FILE="${RESULTS_DIR}/${RUN_TS}.jsonl"
SUMMARY_FILE="${RESULTS_DIR}/${RUN_TS}-summary.json"

echo "==> Verifying native PostgreSQL on port 54329..."
"$PSQL" "$PG_URL" -c "SELECT 1;" > /dev/null 2>&1 || {
    echo "ERROR: cannot connect to $PG_URL" >&2
    echo "Start with: brew services start postgresql@16" >&2
    exit 1
}
echo "    Postgres OK"

# CSV staging uses the system temp dir. To stage on faster/larger storage (and avoid
# filling the internal disk), export TMPDIR yourself before running this script.

# ── Collect DB files
if [[ -n "$SINGLE_DB" ]]; then
  DB_FILES=("${TESTDATA}/${SINGLE_DB}")
  [[ -f "${DB_FILES[0]}" ]] || { echo "File not found: ${DB_FILES[0]}" >&2; exit 1; }
else
  DB_FILES=()
  while IFS= read -r f; do DB_FILES+=("$f"); done \
    < <(find -L "$TESTDATA" -maxdepth 1 \( -name "*.db" -o -name "*.duckdb" -o -name "*.ddb" \) | sort)
fi

[[ ${#DB_FILES[@]} -gt 0 ]] || {
  echo "No .db/.duckdb files in $TESTDATA — run scripts/download-testdata.sh first."
  exit 1
}

echo ""
echo "========================================================"
echo "  hyphasync full sync test harness"
printf "  %s\n" "$(date)"
echo "  ${#DB_FILES[@]} database(s) to test"
echo "  results → $JSONL_FILE"
echo "========================================================"

PASS=0; FAIL=0; SKIP=0
declare -A RESULTS
RUN_START_MS=$(now_ms)
RUN_START_ISO=$(now_iso)

emit_record() {
  # Append one JSON record to the JSONL file.
  # Args: all as named env vars set by the caller.
  printf '%s\n' "$1" >> "$JSONL_FILE"
}

for DB_FILE in "${DB_FILES[@]}"; do
  DB_NAME="$(basename "$DB_FILE")"
  DB_SIZE_BYTES="$(stat -f%z "$DB_FILE" 2>/dev/null || stat --format='%s' "$DB_FILE" 2>/dev/null || echo 0)"
  DB_SIZE_HUMAN="$(ls -lh "$DB_FILE" | awk '{print $5}')"
  echo ""
  echo "--------------------------------------------------------"
  echo "  $DB_NAME  ($DB_SIZE_HUMAN)"
  echo "--------------------------------------------------------"

  # Validate the file is a real DuckDB database.
  VALID=$("$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT 42;" 2>&1 || true)
  if [[ "$VALID" != "42" ]]; then
    echo "  SKIP — not a valid DuckDB file:"
    echo "$VALID" | head -2 | sed 's/^/    /'
    RESULTS["$DB_NAME"]="SKIP (invalid file)"
    SKIP=$((SKIP+1))
    emit_record "{\"run_ts\":\"${RUN_START_ISO}\",\"db\":\"${DB_NAME}\",\"status\":\"SKIP\",\"reason\":\"invalid DuckDB file\"}"
    continue
  fi

  # ── Step 1: hypha_init (connect + local metadata)
  T0=$(now_ms)
  INIT_OUT=$("$DUCKDB" "$DB_FILE" -noheader -list 2>&1 \
    -c "LOAD hyphasync; SELECT hypha_init('${PG_URL}');") && RC=0 || RC=$?
  T_INIT=$(( $(now_ms) - T0 ))

  if [[ $RC -ne 0 ]]; then
    echo "  FAIL — hypha_init (${T_INIT}ms)"
    echo "$INIT_OUT" | grep -iE "Error|Failed" | head -3 | sed 's/^/    /'
    RESULTS["$DB_NAME"]="FAIL (init)"
    FAIL=$((FAIL+1))
    emit_record "{\"run_ts\":\"${RUN_START_ISO}\",\"db\":\"${DB_NAME}\",\"status\":\"FAIL\",\"stage\":\"hypha_init\",\"init_ms\":${T_INIT},\"db_bytes\":${DB_SIZE_BYTES}}"
    continue
  fi
  printf "  %-22s %s\n" "hypha_init:" "${T_INIT}ms"

  # ── Step 2: hypha_base_snapshot (catalog walk + Postgres COPY)
  T0=$(now_ms)
  SNAP_OUT=$("$DUCKDB" "$DB_FILE" -noheader -list 2>&1 \
    -c "LOAD hyphasync; SELECT * FROM hypha_base_snapshot();") && RC=0 || RC=$?
  T_SNAP=$(( $(now_ms) - T0 ))

  if [[ $RC -ne 0 ]]; then
    echo "  FAIL — hypha_base_snapshot (${T_SNAP}ms)"
    echo "$SNAP_OUT" | grep -iE "Error|Failed" | head -3 | sed 's/^/    /'
    RESULTS["$DB_NAME"]="FAIL (snapshot)"
    FAIL=$((FAIL+1))
    emit_record "{\"run_ts\":\"${RUN_START_ISO}\",\"db\":\"${DB_NAME}\",\"status\":\"FAIL\",\"stage\":\"hypha_base_snapshot\",\"init_ms\":${T_INIT},\"snapshot_ms\":${T_SNAP},\"db_bytes\":${DB_SIZE_BYTES}}"
    continue
  fi
  printf "  %-22s %s\n" "hypha_base_snapshot:" "${T_SNAP}ms"

  # ── Step 3: local metadata stats
  TABLES=$(safe "$(query "$DB_FILE" "SELECT COUNT(DISTINCT object_name) FROM hypha.object_snapshot")")
  COLUMNS=$(safe "$(query "$DB_FILE" "SELECT COUNT(*) FROM hypha.column_snapshot")")
  ROWS=$(safe "$(query "$DB_FILE" "SELECT SUM(row_count)::BIGINT FROM hypha.table_snapshot")")
  SKIPPED=$(safe "$(query "$DB_FILE" "SELECT COUNT(*) FROM hypha.event_log WHERE operation='base_snapshot' AND code='TABLE_SKIP'")")
  TYPE_CT=$(safe "$(query "$DB_FILE" "SELECT COUNT(DISTINCT postgres_type) FROM hypha.column_snapshot WHERE NOT postgres_type LIKE '(unsupported%'")")
  UNSUP=$(safe "$(query "$DB_FILE" "SELECT coalesce(string_agg(DISTINCT duckdb_type,', ' ORDER BY duckdb_type),'') FROM hypha.column_snapshot WHERE postgres_type LIKE '(unsupported%'")")
  EVENTS=$(safe "$(query "$DB_FILE" "SELECT COUNT(*) FROM hypha.event_log")")

  printf "  %-22s %s / %s columns / %s rows\n" "catalog:" "$TABLES tables" "$COLUMNS" "$ROWS"
  printf "  %-22s %s\n" "tables_skipped:" "$SKIPPED"
  printf "  %-22s %s\n" "distinct pg types:" "$TYPE_CT"
  [[ -n "$UNSUP" && "$UNSUP" != "?" && "$UNSUP" != "" ]] && printf "  %-22s %s\n" "UNSUPPORTED:" "$UNSUP"
  printf "  %-22s %s\n" "event_log entries:" "$EVENTS"

  # ── Step 4: verify on Postgres
  DB_STEM="${DB_NAME%.*}"
  PG_PREFIX="${DB_STEM//-/_}_"

  PG_TABLES=$(safe "$(pgq "SELECT COUNT(*) FROM pg_stat_user_tables WHERE schemaname LIKE '${PG_PREFIX}%'")")
  PG_ROWS=$(safe "$(pgq   "SELECT COALESCE(SUM(n_live_tup),0) FROM pg_stat_user_tables WHERE schemaname LIKE '${PG_PREFIX}%'")")
  PG_SCHEMAS=$(safe "$(pgq "SELECT COUNT(DISTINCT schemaname) FROM pg_stat_user_tables WHERE schemaname LIKE '${PG_PREFIX}%'")")

  printf "  %-22s %s schemas / %s tables / ~%s rows\n" "on Postgres:" "$PG_SCHEMAS" "$PG_TABLES" "$PG_ROWS"
  printf "  %-22s local %s rows → Postgres ~%s rows\n" "row fidelity:" "$ROWS" "$PG_ROWS"

  T_TOTAL=$(( T_INIT + T_SNAP ))
  printf "  %-22s %s ms total\n" "elapsed:" "$T_TOTAL"

  PASS=$((PASS+1))
  RESULTS["$DB_NAME"]="OK  ${T_TOTAL}ms | ${TABLES}t ${ROWS}r → PG ${PG_TABLES}t ${PG_ROWS}r (skipped ${SKIPPED})"

  # ── Persist JSONL record
  # Escape potential quotes in UNSUP
  UNSUP_ESC="${UNSUP//\"/\'}"
  emit_record "{\"run_ts\":\"${RUN_START_ISO}\",\"db\":\"${DB_NAME}\",\"db_bytes\":${DB_SIZE_BYTES},\"status\":\"OK\",\"init_ms\":${T_INIT},\"snapshot_ms\":${T_SNAP},\"total_ms\":${T_TOTAL},\"local_tables\":${TABLES:-0},\"local_columns\":${COLUMNS:-0},\"local_rows\":${ROWS:-0},\"skipped_tables\":${SKIPPED:-0},\"distinct_pg_types\":${TYPE_CT:-0},\"unsupported_types\":\"${UNSUP_ESC}\",\"event_log_entries\":${EVENTS:-0},\"pg_schemas\":${PG_SCHEMAS:-0},\"pg_tables\":${PG_TABLES:-0},\"pg_rows\":${PG_ROWS:-0}}"
done

RUN_END_MS=$(now_ms)
RUN_TOTAL_MS=$(( RUN_END_MS - RUN_START_MS ))

echo ""
echo "========================================================"
echo "  Summary: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================================"
for DB_NAME in $(echo "${!RESULTS[@]}" | tr ' ' '\n' | sort); do
  printf "  %-42s %s\n" "$DB_NAME" "${RESULTS[$DB_NAME]}"
done
echo ""
printf "  Total elapsed: %s ms (%.1f min)\n" \
  "$RUN_TOTAL_MS" "$(python3 -c "print(${RUN_TOTAL_MS}/60000)")"

# ── Write summary JSON
cat > "$SUMMARY_FILE" <<JSON
{
  "run_ts": "${RUN_START_ISO}",
  "run_end_ts": "$(now_iso)",
  "total_ms": ${RUN_TOTAL_MS},
  "total_dbs": $(( PASS + FAIL + SKIP )),
  "passed": ${PASS},
  "failed": ${FAIL},
  "skipped": ${SKIP},
  "pg_url": "${PG_URL}",
  "duckdb_binary": "${DUCKDB}",
  "results_file": "${JSONL_FILE}"
}
JSON

echo ""
echo "==> Results written to:"
echo "    JSONL: $JSONL_FILE"
echo "    JSON:  $SUMMARY_FILE"

echo ""
echo "==> Native Postgres stays running (use 'brew services stop postgresql@16' to stop)"

[[ $FAIL -eq 0 && $SKIP -eq 0 ]]
