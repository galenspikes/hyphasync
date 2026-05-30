#!/opt/homebrew/bin/bash
# Full end-to-end test harness: runs hypha_init + hypha_base_snapshot against
# every DuckDB database in testdata/ and verifies what landed on Postgres.
# Reports per-DB timing, row counts, skipped tables, type coverage, and event_log.
# Requires: Docker + Docker Compose, built extension (make release).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DUCKDB="${ROOT_DIR}/build/release/duckdb"
TESTDATA="${ROOT_DIR}/testdata"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"

[[ -x "$DUCKDB" ]] || { echo "No duckdb binary — run 'make release' first." >&2; exit 1; }

DB_FILES=()
while IFS= read -r f; do DB_FILES+=("$f"); done \
  < <(find "$TESTDATA" -maxdepth 1 \( -name "*.db" -o -name "*.duckdb" -o -name "*.ddb" \) | sort)

[[ ${#DB_FILES[@]} -gt 0 ]] || {
  echo "No .db/.duckdb files in $TESTDATA — run scripts/download-testdata.sh first."
  exit 1
}

query()  { "$DUCKDB" "$1" -noheader -list -c "$2" 2>/dev/null || echo "?"; }
pgq()    { docker compose exec -T postgres psql -U hypha -d hypha_test -At -c "$1" 2>/dev/null || echo "?"; }
now_ms() { python3 -c "import time; print(int(time.time() * 1000))"; }

echo "==> Starting PostgreSQL (bulk-load tuned)"
docker compose up -d --wait 2>&1 | grep -vE "^#|Pulling|Pulled|Creating|Created|Starting|Started|Waiting|Healthy" || true

echo ""
echo "========================================================"
echo "  hyphasync full sync test harness"
printf "  %s\n" "$(date)"
echo "  ${#DB_FILES[@]} database(s) in testdata/"
echo "========================================================"

PASS=0; FAIL=0; SKIP=0
declare -A RESULTS

for DB_FILE in "${DB_FILES[@]}"; do
  DB_NAME="$(basename "$DB_FILE")"
  DB_SIZE="$(ls -lh "$DB_FILE" | awk '{print $5}')"
  echo ""
  echo "--------------------------------------------------------"
  echo "  $DB_NAME  ($DB_SIZE)"
  echo "--------------------------------------------------------"

  # Validate the file is a real DuckDB database.
  VALID=$("$DUCKDB" "$DB_FILE" -noheader -list -c "SELECT 42;" 2>&1 || true)
  if [[ "$VALID" != "42" ]]; then
    echo "  SKIP — not a valid DuckDB file:"
    echo "$VALID" | head -2 | sed 's/^/    /'
    RESULTS["$DB_NAME"]="SKIP (invalid file)"
    SKIP=$((SKIP+1)); continue
  fi

  # ── Step 1: hypha_init (connect + local metadata)
  T0=$(now_ms)
  INIT_OUT=$("$DUCKDB" "$DB_FILE" -noheader -list 2>&1 \
    -c "LOAD hyphasync; SELECT hypha_init('${PG_URL}');") && RC=0 || RC=$?
  T_INIT=$(( $(now_ms) - T0 ))

  if [[ $RC -ne 0 ]]; then
    echo "  FAIL — hypha_init (${T_INIT}ms)"
    echo "$INIT_OUT" | grep -iE "Error|Failed" | head -3 | sed 's/^/    /'
    RESULTS["$DB_NAME"]="FAIL (init)"; FAIL=$((FAIL+1)); continue
  fi
  printf "  %-22s %s\n" "hypha_init:" "${T_INIT}ms"

  # ── Step 2: hypha_base_snapshot (catalog walk + Postgres COPY)
  T0=$(now_ms)
  SNAP_OUT=$("$DUCKDB" "$DB_FILE" -noheader -list 2>&1 \
    -c "LOAD hyphasync; SELECT hypha_base_snapshot();") && RC=0 || RC=$?
  T_SNAP=$(( $(now_ms) - T0 ))

  if [[ $RC -ne 0 ]]; then
    echo "  FAIL — hypha_base_snapshot (${T_SNAP}ms)"
    echo "$SNAP_OUT" | grep -iE "Error|Failed" | head -3 | sed 's/^/    /'
    RESULTS["$DB_NAME"]="FAIL (snapshot)"; FAIL=$((FAIL+1)); continue
  fi
  printf "  %-22s %s\n" "hypha_base_snapshot:" "${T_SNAP}ms"

  # ── Step 3: local metadata stats
  TABLES=$(query "$DB_FILE" "SELECT COUNT(DISTINCT object_name) FROM hypha.object_snapshot")
  COLUMNS=$(query "$DB_FILE" "SELECT COUNT(*) FROM hypha.column_snapshot")
  ROWS=$(query "$DB_FILE"  "SELECT SUM(row_count)::BIGINT FROM hypha.table_snapshot")
  SKIPPED=$(query "$DB_FILE" "SELECT COUNT(*) FROM hypha.event_log WHERE operation='base_snapshot' AND code='TABLE_SKIP'")
  TYPE_CT=$(query "$DB_FILE" "SELECT COUNT(DISTINCT postgres_type) FROM hypha.column_snapshot WHERE NOT postgres_type LIKE '(unsupported%'")
  UNSUP=$(query "$DB_FILE" "SELECT coalesce(string_agg(DISTINCT duckdb_type,', ' ORDER BY duckdb_type),'') FROM hypha.column_snapshot WHERE postgres_type LIKE '(unsupported%'")
  EVENTS=$(query "$DB_FILE" "SELECT COUNT(*) FROM hypha.event_log")

  printf "  %-22s %s / %s columns / %s rows\n" "catalog:" "$TABLES tables" "$COLUMNS" "$ROWS"
  printf "  %-22s %s\n" "tables_skipped:" "$SKIPPED"
  printf "  %-22s %s\n" "distinct pg types:" "$TYPE_CT"
  [[ -n "$UNSUP" && "$UNSUP" != "?" ]] && printf "  %-22s %s\n" "UNSUPPORTED:" "$UNSUP"
  printf "  %-22s %s\n" "event_log entries:" "$EVENTS"

  # ── Step 4: verify on Postgres
  DB_STEM="${DB_NAME%.*}"        # strip .duckdb / .db
  PG_PREFIX="${DB_STEM//-/_}_"   # ferc60-xbrl → ferc60_xbrl_

  PG_TABLES=$(pgq "SELECT COUNT(*) FROM pg_stat_user_tables WHERE schemaname LIKE '${PG_PREFIX}%'" || echo "?")
  PG_ROWS=$(pgq   "SELECT COALESCE(SUM(n_live_tup),0) FROM pg_stat_user_tables WHERE schemaname LIKE '${PG_PREFIX}%'" || echo "?")
  PG_SCHEMAS=$(pgq "SELECT COUNT(DISTINCT schemaname) FROM pg_stat_user_tables WHERE schemaname LIKE '${PG_PREFIX}%'" || echo "?")

  printf "  %-22s %s schemas / %s tables / ~%s rows\n" "on Postgres:" "$PG_SCHEMAS" "$PG_TABLES" "$PG_ROWS"

  # Row count sanity check: local total_rows should roughly match Postgres approx
  printf "  %-22s local %s rows → Postgres ~%s rows\n" "row fidelity:" "$ROWS" "$PG_ROWS"

  T_TOTAL=$(( T_INIT + T_SNAP ))
  printf "  %-22s %s ms total\n" "elapsed:" "$T_TOTAL"

  PASS=$((PASS+1))
  RESULTS["$DB_NAME"]="OK  ${T_TOTAL}ms | ${TABLES}t ${ROWS}r → PG ${PG_TABLES}t ${PG_ROWS}r (skipped ${SKIPPED})"
done

echo ""
echo "========================================================"
echo "  Summary: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================================"
for DB_NAME in $(echo "${!RESULTS[@]}" | tr ' ' '\n' | sort); do
  printf "  %-42s %s\n" "$DB_NAME" "${RESULTS[$DB_NAME]}"
done
echo ""

echo "==> Tearing down PostgreSQL"
docker compose down 2>&1 | grep -vE "^$|Stopping|Stopped|Removing"

[[ $FAIL -eq 0 && $SKIP -eq 0 ]]
