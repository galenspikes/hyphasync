#!/usr/bin/env bash
# hyphasync smoke test — a fast end-to-end sanity check.
#
# Proves the core pipeline works on this machine: the extension loads, the
# diagnostic functions run, and a real DuckDB -> Postgres base snapshot + an
# incremental sync both succeed against a live Postgres target. It is the quick
# "is it fundamentally working?" check; test/integration/run.sh is the thorough
# 90+ assertion suite.
#
# Usage:
#   ./scripts/smoke-test.sh [--build]
#
#   --build   Build the release extension first (otherwise an existing
#             ./build/release/duckdb is required; the script builds it only if
#             the binary is missing).
#
# Postgres target:
#   Defaults to the standard local test target. Override with HYPHA_PG_URL.
#   If nothing is listening on the target port and Docker is available, the
#   script starts the docker-compose Postgres service automatically (Linux).
#   On macOS, start Postgres yourself first (e.g. brew services start
#   postgresql@16 plus ./scripts/setup-postgres-test.sh).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DUCKDB="${ROOT_DIR}/build/release/duckdb"
DB_FILE="${ROOT_DIR}/build/smoke_test.duckdb"
PG_URL="${HYPHA_PG_URL:-postgresql://hypha:hypha@127.0.0.1:54329/hypha_test}"

# Parse the host/port out of the URL for the liveness probe (defaults cover the
# standard local target; only used to decide whether to start Docker).
PG_HOST="$(printf '%s' "$PG_URL" | sed -E 's#.*@([^:/]+).*#\1#')"; PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="$(printf '%s' "$PG_URL" | sed -E 's#.*:([0-9]+)/.*#\1#')"; PG_PORT="${PG_PORT:-54329}"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0; FAIL=0
STARTED_DOCKER=false

pass() { echo -e "  ${GREEN}PASS${NC}: $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}FAIL${NC}: $1"; [[ -n "${2:-}" ]] && echo "       $2"; FAIL=$((FAIL + 1)); }

# True if a TCP connection to the Postgres port succeeds. Uses bash /dev/tcp so
# no psql / pg_isready dependency is required.
pg_up() { (exec 3<>"/dev/tcp/${PG_HOST}/${PG_PORT}") 2>/dev/null; }

cleanup() {
    [[ "$STARTED_DOCKER" == true ]] && docker compose down >/dev/null 2>&1 || true
    rm -f "$DB_FILE"
}
trap cleanup EXIT

# --- Build (only if asked or binary missing) -------------------------------
if [[ "${1:-}" == "--build" || ! -x "$DUCKDB" ]]; then
    echo "Building hyphasync (CC=gcc CXX=g++ on Linux; default toolchain on macOS)..."
    if [[ "$(uname -s)" == "Darwin" ]]; then
        make release -j"$(sysctl -n hw.ncpu)"
    else
        CC=gcc CXX=g++ make release -j"$(nproc 2>/dev/null || echo 4)"
    fi
fi
[[ -x "$DUCKDB" ]] || { echo "duckdb binary not found at $DUCKDB (run with --build)" >&2; exit 1; }

# --- Ensure a Postgres target is reachable ---------------------------------
if ! pg_up; then
    if command -v docker >/dev/null 2>&1; then
        echo "No Postgres on ${PG_HOST}:${PG_PORT}; starting docker-compose service..."
        docker compose up -d postgres
        STARTED_DOCKER=true
        for _ in $(seq 1 30); do pg_up && break; sleep 1; done
    fi
fi
pg_up || { echo "ERROR: no Postgres reachable at ${PG_HOST}:${PG_PORT} (set HYPHA_PG_URL or start Postgres)" >&2; exit 1; }

rm -f "$DB_FILE"
echo "Smoke DB:  $DB_FILE"
echo "PG target: $PG_URL"

# Run SQL against the persistent smoke DB and echo stdout. Errors propagate via
# the captured output (callers grep for the expected truthy result).
q() { "$DUCKDB" "$DB_FILE" -noheader -list -c "$1" 2>/dev/null || echo "ERROR"; }

echo
echo "── Smoke test ────────────────────────────────────────"

# 1. Extension loaded — hypha_hello() returns a non-empty greeting.
out="$(q "SELECT hypha_hello();")"
[[ -n "$out" && "$out" != "ERROR" ]] && pass "hypha_hello() responds" || fail "hypha_hello()" "$out"

# 2. Diagnostics — hypha_doctor() mentions the extension.
out="$(q "SELECT hypha_doctor();")"
echo "$out" | grep -qi "hyphasync" && pass "hypha_doctor() reports status" || fail "hypha_doctor()" "$out"

# 3. Create a tiny table and register the Postgres target.
q "CREATE TABLE smoke_t(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO smoke_t VALUES (1,'a'),(2,'b'),(3,'c');" >/dev/null
out="$(q "SELECT hypha_init('${PG_URL}') IS NOT NULL;")"
echo "$out" | grep -qi true && pass "hypha_init() connects to Postgres" || fail "hypha_init()" "$out"

# 4. Base snapshot — every returned table pushed with status 'ok'.
out="$(q "SELECT count(*) FILTER (WHERE status='ok') = count(*) AND count(*) > 0 FROM hypha_base_snapshot();")"
echo "$out" | grep -qi true && pass "hypha_base_snapshot() pushed all tables" || fail "hypha_base_snapshot()" "$out"

# 5. Incremental sync after an in-place change + insert — applies cleanly.
q "UPDATE smoke_t SET v='z' WHERE id=1; INSERT INTO smoke_t VALUES (4,'d');" >/dev/null
out="$(q "SELECT count(*) FILTER (WHERE status='ok') = count(*) AND count(*) > 0 FROM hypha_sync();")"
echo "$out" | grep -qi true && pass "hypha_sync() applied changes" || fail "hypha_sync()" "$out"

echo
echo "──────────────────────────────────────────────────────"
echo -e "  ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" -eq 0 ]] || exit 1
echo -e "  ${GREEN}smoke test passed${NC}"
