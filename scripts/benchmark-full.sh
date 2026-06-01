#!/opt/homebrew/bin/bash
# Full sequential benchmark of all sample databases < 5 GB.
# For each DB: fresh Postgres reinit → hypha_init → hypha_base_snapshot
# Outputs: console table + testdata/results/benchmark-<timestamp>.tsv
set -euo pipefail

ROOT_DIR="/Users/galenspikes/repos/hyphasync"
DUCKDB="${ROOT_DIR}/build/release/duckdb"
TESTDATA="${ROOT_DIR}/testdata"
RESULTS_DIR="${ROOT_DIR}/testdata/results"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"
PSQL="/opt/homebrew/opt/postgresql@16/bin/psql"
INITDB="/opt/homebrew/opt/postgresql@16/bin/initdb"
PG_DATADIR="/opt/homebrew/var/postgresql@16"

# Databases in order smallest → largest
DATABASES=(
    "tpch-sf1.duckdb"
    "ferc60-xbrl.duckdb"
    "ferc714-xbrl.duckdb"
    "ferc6-xbrl.duckdb"
    "ferc2-xbrl.duckdb"
    "tpch-sf1.db"
    "frankenstein.duckdb"
    "ferc1-xbrl.duckdb"
    "tpch-sf3.db"
    "tpch-sf10.db"
)

mkdir -p "$RESULTS_DIR"

RUN_TS="$(date -u '+%Y-%m-%dT%H-%M-%SZ')"
TSV_FILE="${RESULTS_DIR}/benchmark-${RUN_TS}.tsv"
JSONL_FILE="${RESULTS_DIR}/benchmark-${RUN_TS}.jsonl"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

now_ms() { python3 -c "import time; print(int(time.time()*1000))"; }

query_db() {
    local db="$1" sql="$2"
    "$DUCKDB" "$db" -noheader -list -c "$sql" 2>/dev/null || echo "?"
}

query_pg() {
    local sql="$1"
    "$PSQL" "$PG_URL" -At -c "$sql" 2>/dev/null || echo "?"
}

reinit_postgres() {
    log "Stopping postgresql@16..."
    brew services stop postgresql@16 2>/dev/null || true
    # wait for it to actually stop
    local retries=10
    while pg_isready -h 127.0.0.1 -p 54329 -q 2>/dev/null && [[ $retries -gt 0 ]]; do
        sleep 1; retries=$((retries-1))
    done
    log "Wiping datadir..."
    rm -rf "$PG_DATADIR"
    log "Running initdb..."
    "$INITDB" -D "$PG_DATADIR" --username=galenspikes --auth=trust > /dev/null
    echo "port = 54329" >> "${PG_DATADIR}/postgresql.conf"
    log "Starting postgresql@16..."
    brew services start postgresql@16
    # wait for ready
    retries=15
    while ! pg_isready -h 127.0.0.1 -p 54329 -q 2>/dev/null; do
        sleep 1
        retries=$((retries-1))
        [[ $retries -gt 0 ]] || { log "ERROR: Postgres did not start"; exit 1; }
    done
    sleep 1  # extra settle time
    log "Postgres up. Creating hypha user/db..."
    psql -h 127.0.0.1 -p 54329 -U galenspikes postgres \
        -c "CREATE USER hypha WITH PASSWORD 'hypha';" 2>/dev/null
    psql -h 127.0.0.1 -p 54329 -U galenspikes postgres \
        -c "CREATE DATABASE hypha_test OWNER hypha;" 2>/dev/null
    psql -h 127.0.0.1 -p 54329 -U galenspikes hypha_test \
        -c "GRANT ALL ON SCHEMA public TO hypha;" 2>/dev/null
    log "Postgres ready on port 54329."
}

# Write TSV header
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "database" "file_size_mb" "src_tables" "src_rows" "src_cols" \
    "init_ms" "fingerprint_ms" "copy_ms" "total_ms" "peak_rss_mb" \
    "pg_tables" "pg_rows" "pg_size" "rows_per_sec" "mb_per_sec" "notes" \
    > "$TSV_FILE"

PASS=0; FAIL=0; SKIP=0

echo ""
echo "========================================================"
echo "  hyphasync sequential benchmark — ${RUN_TS}"
echo "  ${#DATABASES[@]} databases, fresh Postgres per DB"
echo "  results → $TSV_FILE"
echo "========================================================"

for DB_NAME in "${DATABASES[@]}"; do
    DB_FILE="${TESTDATA}/${DB_NAME}"
    TMP_FILE="/tmp/bench_${DB_NAME}"

    echo ""
    echo "────────────────────────────────────────────────────────"
    log "  Starting: $DB_NAME"
    echo "────────────────────────────────────────────────────────"

    if [[ ! -f "$DB_FILE" ]]; then
        log "  SKIP — file not found: $DB_FILE"
        SKIP=$((SKIP+1))
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$DB_NAME" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "FILE_NOT_FOUND" \
            >> "$TSV_FILE"
        continue
    fi

    FILE_SIZE_BYTES=$(stat -f%z "$DB_FILE" 2>/dev/null || stat --format='%s' "$DB_FILE")
    FILE_SIZE_MB=$(python3 -c "print(round(${FILE_SIZE_BYTES}/1048576,1))")
    log "  File size: ${FILE_SIZE_MB} MB"

    # ── Step 1: Copy to /tmp (Mac internal SSD)
    log "  Copying to /tmp..."
    T_COPY_START=$(now_ms)
    cp "$DB_FILE" "$TMP_FILE"
    T_COPY_MS=$(( $(now_ms) - T_COPY_START ))
    log "  Copy done in ${T_COPY_MS}ms"

    # ── Validate it's a real DuckDB file
    VALID=$("$DUCKDB" "$TMP_FILE" -noheader -list -c "SELECT 42;" 2>&1 || true)
    if [[ "$VALID" != "42" ]]; then
        log "  SKIP — not a valid DuckDB file"
        rm -f "$TMP_FILE"
        SKIP=$((SKIP+1))
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$DB_NAME" "$FILE_SIZE_MB" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "?" "INVALID_DUCKDB" \
            >> "$TSV_FILE"
        continue
    fi

    # ── Step 2: Collect source DB stats (before wiping hypha schema)
    log "  Collecting source stats..."
    SRC_TABLES=$(query_db "$TMP_FILE" "SELECT COUNT(*) FROM information_schema.tables WHERE table_type='BASE TABLE' AND table_schema NOT IN ('hypha','information_schema','pg_catalog');")
    SRC_ROWS=$(query_db "$TMP_FILE" "SELECT COALESCE(SUM(estimated_size),0) FROM duckdb_tables() WHERE schema_name NOT IN ('hypha','information_schema','pg_catalog','memory');")
    SRC_COLS=$(query_db "$TMP_FILE" "SELECT COALESCE(SUM(column_count),0) FROM duckdb_tables() WHERE schema_name NOT IN ('hypha','information_schema','pg_catalog','memory');")
    log "  Source: ${SRC_TABLES} tables / ${SRC_ROWS} rows (est) / ${SRC_COLS} cols"

    # Drop any prior hypha schema so event_log starts clean
    "$DUCKDB" "$TMP_FILE" -c "DROP SCHEMA IF EXISTS hypha CASCADE;" 2>/dev/null || true

    # ── Step 3: Reinit Postgres
    reinit_postgres

    # ── Datadir size before sync
    PG_DATADIR_PRE=$(du -sk "$PG_DATADIR" 2>/dev/null | awk '{print $1}')

    # ── Step 4a: Time hypha_init
    log "  Running hypha_init..."
    T0=$(now_ms)
    INIT_OUT=$("$DUCKDB" "$TMP_FILE" -noheader -list \
        -c "LOAD hyphasync; SELECT hypha_init('${PG_URL}');" 2>&1) && INIT_RC=0 || INIT_RC=$?
    T_INIT=$(( $(now_ms) - T0 ))
    log "  hypha_init: ${T_INIT}ms (rc=${INIT_RC})"

    if [[ $INIT_RC -ne 0 ]]; then
        log "  FAIL — hypha_init failed"
        echo "$INIT_OUT" | grep -iE "Error|Fail" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL+1))
        rm -f "$TMP_FILE"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$DB_NAME" "$FILE_SIZE_MB" "$SRC_TABLES" "$SRC_ROWS" "$SRC_COLS" \
            "$T_INIT" "?" "?" "?" "?" "?" "?" "?" "?" "?" "INIT_FAILED" \
            >> "$TSV_FILE"
        continue
    fi

    # ── Step 4b: Time hypha_base_snapshot with peak RSS capture
    log "  Running hypha_base_snapshot (this may take a while)..."
    STDERR_FILE="/tmp/bench_stderr_${DB_NAME}.txt"

    T0=$(now_ms)
    /usr/bin/time -l "$DUCKDB" "$TMP_FILE" \
        -c "LOAD hyphasync; SELECT * FROM hypha_base_snapshot();" \
        > /dev/null 2>"$STDERR_FILE" && SNAP_RC=0 || SNAP_RC=$?
    T_SNAP=$(( $(now_ms) - T0 ))
    log "  hypha_base_snapshot: ${T_SNAP}ms (rc=${SNAP_RC})"

    # Extract peak RSS from /usr/bin/time -l output (bytes)
    PEAK_RSS_BYTES=$(grep -E "maximum resident" "$STDERR_FILE" 2>/dev/null | awk '{print $1}' | tr -d ' ,' || echo "0")
    PEAK_RSS_MB=$(python3 -c "print(round(${PEAK_RSS_BYTES:-0}/1048576,0))" 2>/dev/null || echo "?")
    log "  Peak RSS: ${PEAK_RSS_MB} MB"

    if [[ $SNAP_RC -ne 0 ]]; then
        log "  FAIL — hypha_base_snapshot failed (stderr follows):"
        grep -iE "Error|Fail|Warning" "$STDERR_FILE" 2>/dev/null | head -5 | sed 's/^/    /' || true
        rm -f "$STDERR_FILE"
        FAIL=$((FAIL+1))
        rm -f "$TMP_FILE"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$DB_NAME" "$FILE_SIZE_MB" "$SRC_TABLES" "$SRC_ROWS" "$SRC_COLS" \
            "$T_INIT" "?" "?" "$T_SNAP" "$PEAK_RSS_MB" "?" "?" "?" "?" "?" "SNAPSHOT_FAILED" \
            >> "$TSV_FILE"
        continue
    fi

    rm -f "$STDERR_FILE"

    # ── Step 5: Extract event_log timing breakdown
    log "  Extracting timing from event_log..."
    FINGERPRINT_MS=$(query_db "$TMP_FILE" "
        SELECT COALESCE(
            epoch_ms(MAX(event_time) FILTER (WHERE operation='base_snapshot_plan'))
            - epoch_ms(MIN(event_time) FILTER (WHERE operation='base_snapshot_plan' AND code='FINGERPRINT_START')),
            0)
        FROM hypha.event_log;")
    COPY_MS=$(query_db "$TMP_FILE" "
        SELECT COALESCE(
            epoch_ms(MAX(event_time) FILTER (WHERE operation='base_snapshot'))
            - epoch_ms(MIN(event_time) FILTER (WHERE operation='base_snapshot' AND code='START')),
            0)
        FROM hypha.event_log;")
    log "  fingerprint_ms=${FINGERPRINT_MS} copy_ms=${COPY_MS}"

    # Fingerprint strategy distribution
    STRATEGIES=$(query_db "$TMP_FILE" "
        SELECT string_agg(strategy || '×' || cnt, ', ' ORDER BY cnt DESC)
        FROM (
            SELECT REGEXP_EXTRACT(details_json, '\"strategy\":\"([^\"]+)\"', 1) AS strategy,
                   COUNT(*) AS cnt
            FROM hypha.event_log
            WHERE operation='base_snapshot_plan' AND code='FINGERPRINT_DONE'
            GROUP BY 1
        ) t;")
    log "  Fingerprint strategies: ${STRATEGIES}"

    # ── Step 6: Postgres result stats
    log "  Collecting Postgres stats..."
    # Brief settle for pg_stat_user_tables to populate
    sleep 2

    PG_TABLES=$(query_pg "SELECT COUNT(*) FROM pg_stat_user_tables WHERE schemaname NOT IN ('public','hypha','pg_catalog','information_schema');")
    PG_ROWS=$(query_pg "SELECT COALESCE(SUM(n_live_tup),0) FROM pg_stat_user_tables WHERE schemaname NOT IN ('public','hypha','pg_catalog','information_schema');")
    PG_SIZE=$(query_pg "SELECT pg_size_pretty(COALESCE(SUM(pg_total_relation_size(schemaname||'.'||relname)),0)) FROM pg_stat_user_tables WHERE schemaname NOT IN ('public','hypha','pg_catalog','information_schema');")
    PG_DATADIR_POST=$(du -sk "$PG_DATADIR" 2>/dev/null | awk '{print $1}')
    PG_DATADIR_GROWTH_KB=$(( PG_DATADIR_POST - PG_DATADIR_PRE ))
    log "  PG: ${PG_TABLES} tables / ~${PG_ROWS} rows / ${PG_SIZE} / datadir +${PG_DATADIR_GROWTH_KB}KB"

    # ── Step 7: Compute throughput
    TOTAL_MS=$(( T_INIT + T_SNAP ))
    ROWS_PER_SEC=$(python3 -c "
try:
    rows = int('${SRC_ROWS}')
    ms = int('${TOTAL_MS}')
    print(round(rows / (ms/1000)) if ms > 0 else '?')
except:
    print('?')
" 2>/dev/null || echo "?")
    MB_PER_SEC=$(python3 -c "
try:
    mb = float('${FILE_SIZE_MB}')
    ms = int('${TOTAL_MS}')
    print(round(mb / (ms/1000), 1) if ms > 0 else '?')
except:
    print('?')
" 2>/dev/null || echo "?")
    log "  Throughput: ${ROWS_PER_SEC} rows/sec | ${MB_PER_SEC} MB/sec"

    # ── Write TSV record
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$DB_NAME" "$FILE_SIZE_MB" "$SRC_TABLES" "$SRC_ROWS" "$SRC_COLS" \
        "$T_INIT" "$FINGERPRINT_MS" "$COPY_MS" "$TOTAL_MS" "$PEAK_RSS_MB" \
        "$PG_TABLES" "$PG_ROWS" "$PG_SIZE" "$ROWS_PER_SEC" "$MB_PER_SEC" \
        "${STRATEGIES:-}" \
        >> "$TSV_FILE"

    # Write JSONL record
    printf '%s\n' "{\"db\":\"${DB_NAME}\",\"file_mb\":${FILE_SIZE_MB},\"src_tables\":${SRC_TABLES},\"src_rows\":${SRC_ROWS},\"src_cols\":${SRC_COLS},\"init_ms\":${T_INIT},\"fingerprint_ms\":${FINGERPRINT_MS},\"copy_ms\":${COPY_MS},\"total_ms\":${TOTAL_MS},\"peak_rss_mb\":${PEAK_RSS_MB},\"pg_tables\":${PG_TABLES},\"pg_rows\":${PG_ROWS},\"pg_size\":\"${PG_SIZE}\",\"rows_per_sec\":${ROWS_PER_SEC},\"mb_per_sec\":${MB_PER_SEC},\"strategies\":\"${STRATEGIES:-}\"}" \
        >> "$JSONL_FILE"

    PASS=$((PASS+1))

    # ── Step 8: Cleanup /tmp
    log "  Cleaning up /tmp..."
    rm -f "$TMP_FILE"
    log "  Done: $DB_NAME"
done

echo ""
echo "========================================================"
echo "  Benchmark complete: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================================"

# ── Print final Markdown table
echo ""
echo "## Results"
echo ""
python3 - "$TSV_FILE" <<'PYEOF'
import sys, csv

with open(sys.argv[1]) as f:
    rows = list(csv.DictReader(f, delimiter='\t'))

headers = [
    "Database", "File MB", "Src Tables", "Src Rows", "Src Cols",
    "init ms", "fp ms", "copy ms", "total ms", "Peak RSS MB",
    "PG Tables", "PG Rows", "PG Size", "Rows/sec", "MB/sec", "Notes"
]
keys = [
    "database", "file_size_mb", "src_tables", "src_rows", "src_cols",
    "init_ms", "fingerprint_ms", "copy_ms", "total_ms", "peak_rss_mb",
    "pg_tables", "pg_rows", "pg_size", "rows_per_sec", "mb_per_sec", "notes"
]

# Print markdown table
print("| " + " | ".join(headers) + " |")
print("| " + " | ".join(["---"] * len(headers)) + " |")
for row in rows:
    cells = [row.get(k, "?") for k in keys]
    print("| " + " | ".join(cells) + " |")
PYEOF

echo ""
echo "==> Results:"
echo "    TSV:   $TSV_FILE"
echo "    JSONL: $JSONL_FILE"
