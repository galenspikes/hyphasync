#!/usr/bin/env bash
# Lightweight fingerprint performance regression gate.
# Creates a 100k-row table in DuckDB and runs representative fingerprint SQL.
# Fails if total elapsed time exceeds MAX_SECONDS.
#
# Usage: ./scripts/benchmark-fingerprint.sh [max_seconds]
#   BINARY env var overrides the DuckDB binary path.
set -euo pipefail

MAX_SECONDS="${1:-30}"
BINARY="${BINARY:-./build/release/duckdb}"

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: DuckDB binary not found or not executable: $BINARY" >&2
    exit 1
fi

echo "Fingerprint benchmark: 100k-row table (limit: ${MAX_SECONDS}s)"

START=$(date +%s)
"$BINARY" -c "
CREATE TABLE bench_t AS SELECT range AS id, random()::VARCHAR AS val FROM range(100000);
CREATE TABLE bench_t2 AS SELECT * FROM bench_t;
SELECT COUNT(*), MIN(id), MAX(id) FROM bench_t;
SELECT sha256(string_agg(sha256(concat('i:', id::VARCHAR, '|s:', val)), ''))
FROM (SELECT id, val FROM bench_t ORDER BY id LIMIT 50000);
"
END=$(date +%s)
ELAPSED=$((END - START))

echo "Fingerprint benchmark: ${ELAPSED}s (limit: ${MAX_SECONDS}s)"
if [ "$ELAPSED" -gt "$MAX_SECONDS" ]; then
    echo "FAIL: exceeded time limit"
    exit 1
fi
echo "PASS"
