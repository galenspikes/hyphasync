#!/usr/bin/env bash
# sync-dummy.sh — hyphasync walkthrough
#
# Creates a randomly-named DuckDB database, populates it with sample data
# across five tables (various types, sizes, and edge cases), then syncs the
# whole thing to Postgres using hyphasync's three-step workflow:
#
#   1. hypha_init()               — connect & initialize local metadata
#   2. hypha_base_snapshot_plan() — fingerprint every table, build the sync plan
#   3. hypha_base_snapshot()      — COPY all tables to Postgres
#
# Usage:
#   ./scripts/sync-dummy.sh
#
# At the end you'll be prompted whether to keep the .duckdb file and where
# to save it (default: ~/Downloads/<name>.duckdb).
#
# Requires: built extension  →  CC=gcc CXX=g++ make release

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB="${ROOT_DIR}/build/release/duckdb"
PG_URL="postgresql://hypha:hypha@127.0.0.1:54329/hypha_test"

# ── Preflight check ────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║            hyphasync — end-to-end walkthrough            ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

[[ -x "$DUCKDB" ]] || {
  echo "✗ No duckdb binary found at ${DUCKDB}"
  echo "  Run 'CC=gcc CXX=g++ make release' first, then retry."
  exit 1
}
echo "✓ Found DuckDB binary: ${DUCKDB}"

pause() {
  echo ""
  echo -n "  Press Enter to continue..."
  read -r
  echo ""
}

# ── Step 0: Create a fresh DuckDB database ─────────────────────────────────────
SUFFIX=$(openssl rand -hex 3)
DB_NAME="dummy_${SUFFIX}"
DB_FILE="/tmp/${DB_NAME}.duckdb"

pause
echo "━━━ Step 0: Create the source DuckDB database ━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  Every DuckDB file is self-contained — tables, indexes, and data all live"
echo "  inside a single file. We'll create one at:"
echo "  ${DB_FILE}"
echo ""

"$DUCKDB" "$DB_FILE" <<'SQL'
SET autoinstall_known_extensions=1;
SET autoload_known_extensions=1;

-- ── users: 200 rows, common scalar types ──────────────────────────────────────
-- Demonstrates: INTEGER PK, VARCHAR, BOOLEAN, DOUBLE, TIMESTAMP
CREATE TABLE users (
    id        INTEGER PRIMARY KEY,
    name      VARCHAR,
    email     VARCHAR,
    active    BOOLEAN,
    score     DOUBLE,
    joined_at TIMESTAMP
);

INSERT INTO users SELECT
    i,
    'User ' || i,
    'user' || i || '@example.com',
    (i % 2 = 0),
    round(random() * 100, 2),
    TIMESTAMP '2024-01-01' + INTERVAL (i) DAY
FROM range(1, 201) t(i);

-- ── products: 50 rows, TIMESTAMP_S triggers a TYPE_COERCE event ───────────────
-- DuckDB's TIMESTAMP_S (second precision) maps to Postgres 'timestamp without
-- time zone'. hyphasync logs a TYPE_COERCE event to hypha.event_log.
CREATE TABLE products (
    product_id  INTEGER PRIMARY KEY,
    sku         VARCHAR,
    name        VARCHAR,
    price       DECIMAL(10,2),
    stock       INTEGER,
    created_at  TIMESTAMP_S
);

INSERT INTO products SELECT
    i,
    'SKU-' || lpad(i::VARCHAR, 5, '0'),
    'Product ' || i,
    round(random() * 500 + 1, 2),
    (random() * 1000)::INTEGER,
    TIMESTAMP '2023-06-01' + INTERVAL (i * 3) DAY
FROM range(1, 51) t(i);

-- ── events: 500 rows, UUID PK + JSON payload ──────────────────────────────────
-- Demonstrates: UUID primary key, JSON → jsonb in Postgres.
-- hyphasync auto-loads the 'json' extension when it sees JSON columns.
CREATE TABLE events (
    event_id    UUID DEFAULT gen_random_uuid(),
    user_id     INTEGER,
    kind        VARCHAR,
    payload     JSON,
    occurred_at TIMESTAMP
);

INSERT INTO events SELECT
    gen_random_uuid(),
    (random() * 200 + 1)::INTEGER,
    CASE (i % 4)
        WHEN 0 THEN 'click'
        WHEN 1 THEN 'view'
        WHEN 2 THEN 'purchase'
        ELSE 'logout'
    END,
    '{"source": "web", "seq": ' || i || '}',
    TIMESTAMP '2024-01-01' + INTERVAL (i % 365) DAY
FROM range(1, 501) t(i);

-- ── tags: 5 rows, VARCHAR primary key ─────────────────────────────────────────
-- A tiny lookup table — exercises EXACT fingerprinting (< 1 MB serialized).
CREATE TABLE tags (
    tag    VARCHAR PRIMARY KEY,
    weight DOUBLE
);

INSERT INTO tags VALUES
    ('featured', 1.0), ('sale', 0.8), ('new', 0.9),
    ('limited', 0.7), ('bundle', 0.6);

-- ── empty_log: 0 rows ─────────────────────────────────────────────────────────
-- Edge case: hyphasync should handle zero-row tables cleanly.
CREATE TABLE empty_log (id INTEGER, message VARCHAR);

-- Summary
SELECT
    (SELECT count(*) FROM users)    AS users,
    (SELECT count(*) FROM products) AS products,
    (SELECT count(*) FROM events)   AS events,
    (SELECT count(*) FROM tags)     AS tags,
    (SELECT count(*) FROM empty_log) AS empty_log;
SQL

echo ""
echo "✓ Database created with 5 tables (users, products, events, tags, empty_log)"

# ── Step 1: hypha_init ─────────────────────────────────────────────────────────
pause
echo "━━━ Step 1: hypha_init — connect to Postgres & initialize ━━━━━━━━━━━━━━━━━"
echo ""
echo "  hypha_init() does three things:"
echo "  • Verifies the Postgres connection (host, db, user, latency → stderr)"
echo "  • Creates the 'hypha' bookkeeping schema in DuckDB"
echo "  • Logs an OK event to hypha.event_log"
echo ""
echo "  Target: ${PG_URL}"
echo ""

"$DUCKDB" "$DB_FILE" <<SQL > /dev/null
SELECT hypha_init('${PG_URL}');
SQL

echo ""
echo "✓ hypha_init complete"

# ── Step 2: hypha_base_snapshot_plan ──────────────────────────────────────────

pause
echo "━━━ Step 2: hypha_base_snapshot_plan — fingerprint & classify tables ━━━━━━"
echo ""
echo "  hypha_base_snapshot_plan() walks the DuckDB catalog and assigns each"
echo "  table a fingerprint strategy:"
echo ""
echo "    EXACT          — full sha256 of all rows (used for small tables)"
echo "    APPEND_ONLY    — sha256(COUNT || MAX(pk)) (tables with monotonic PK)"
echo "    MUTABLE_ENTITY — sha256(COUNT || MIN(rowid) || MAX(rowid)) (default)"
echo ""
echo "  Results are stored in hypha.{object,column,table}_snapshot in DuckDB."
echo "  Nothing is written to Postgres yet."
echo ""

"$DUCKDB" "$DB_FILE" <<SQL > /dev/null
SELECT hypha_base_snapshot_plan();
SQL

echo ""
echo "✓ Snapshot plan built"

# ── Step 3: hypha_base_snapshot ───────────────────────────────────────────────
pause
echo "━━━ Step 3: hypha_base_snapshot — COPY all tables to Postgres ━━━━━━━━━━━━━"
echo ""
echo "  hypha_base_snapshot() streams each table directly to Postgres using"
echo "  libpq's COPY protocol (no temp files). It:"
echo "  • Creates the target schema and tables in Postgres"
echo "  • Sets STORAGE EXTERNAL on varlena columns (text/jsonb/bytea) to avoid"
echo "    Postgres's 8 KB heap row limit"
echo "  • Returns one row per table: (table_name, row_count, strategy, ms, status)"
echo ""

"$DUCKDB" "$DB_FILE" <<SQL
SELECT * FROM hypha_base_snapshot();
SQL

echo ""
echo "✓ Base snapshot complete"
echo ""
echo "  Verifying in Postgres..."
psql "${PG_URL}" --no-psqlrc -c "SELECT id, name, email, active, score FROM ${DB_NAME}_main.users LIMIT 3;" \
  -c "SELECT * FROM ${DB_NAME}_main.tags;" \
  -c "SELECT 'users' AS table_name, count(*) AS rows FROM ${DB_NAME}_main.users UNION ALL SELECT 'products', count(*) FROM ${DB_NAME}_main.products UNION ALL SELECT 'events', count(*) FROM ${DB_NAME}_main.events UNION ALL SELECT 'tags', count(*) FROM ${DB_NAME}_main.tags UNION ALL SELECT 'empty_log', count(*) FROM ${DB_NAME}_main.empty_log ORDER BY table_name;" \
  2>/dev/null || echo "  (psql not available — check Postgres manually)"

# ── Step 4: hypha_status ──────────────────────────────────────────────────────
pause
echo "━━━ Step 4: hypha_status — verify the sync state ━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  hypha_status() reads from local DuckDB metadata (hypha.commit) and"
echo "  returns a one-line summary of the most recent sync."
echo ""

"$DUCKDB" "$DB_FILE" <<SQL > /dev/null
SELECT hypha_status();
SQL

# ── Incremental sync prompt ───────────────────────────────────────────────────
echo ""
echo -n "  Want to see an incremental sync? [Y/n] "
read -r INC_ANSWER
INC_ANSWER="${INC_ANSWER:-Y}"

if [[ "$INC_ANSWER" =~ ^[Yy] ]]; then

  # ── Step 5: Mutate data ─────────────────────────────────────────────────────
  pause
  echo "━━━ Step 5: Mutate data — make realistic changes ━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo ""
  echo "  Making realistic data changes to demonstrate incremental sync:"
  echo "  • users: +10 new rows (will trigger APPEND_ONLY change detection)"
  echo "  • products: price & stock updates on 10 rows (EXACT re-hash will detect changes)"
  echo "  • tags: 1 new row inserted"
  echo "  • events: +50 new rows"
  echo "  • empty_log: unchanged (will appear as 'unchanged' in sync output)"
  echo ""

  "$DUCKDB" "$DB_FILE" <<'SQL'
SET autoinstall_known_extensions=1;
SET autoload_known_extensions=1;

-- Add 10 new users (triggers APPEND_ONLY change detection on users table)
INSERT INTO users SELECT
    200 + i,
    'New User ' || (200 + i),
    'newuser' || (200 + i) || '@example.com',
    true,
    round(random() * 100, 2),
    CURRENT_TIMESTAMP
FROM range(1, 11) t(i);

-- Update some product prices
UPDATE products SET price = price * 1.1, stock = stock - 5 WHERE product_id <= 10;

-- Add a new tag
INSERT INTO tags VALUES ('clearance', 0.3);

-- Insert 50 new events
INSERT INTO events SELECT
    gen_random_uuid(),
    (random() * 210 + 1)::INTEGER,
    'purchase',
    '{"source": "mobile", "promo": true}',
    CURRENT_TIMESTAMP
FROM range(1, 51) t(i);

-- Summary of current row counts
SELECT
    (SELECT count(*) FROM users)     AS users,
    (SELECT count(*) FROM products)  AS products,
    (SELECT count(*) FROM events)    AS events,
    (SELECT count(*) FROM tags)      AS tags,
    (SELECT count(*) FROM empty_log) AS empty_log;
SQL

  echo ""
  echo "✓ Data mutations applied"

  # ── Step 6: hypha_sync_plan ─────────────────────────────────────────────────
  pause
  echo "━━━ Step 6: hypha_sync_plan — preview what changed ━━━━━━━━━━━━━━━━━━━━━━"
  echo ""
  echo "  hypha_sync_plan() diffs current fingerprints against the last snapshot."
  echo "  It returns a structured plan showing what changed — no Postgres writes yet."
  echo "  Action values: new, updated, schema_changed, unchanged"
  echo ""

  "$DUCKDB" "$DB_FILE" <<SQL > /dev/null
SELECT hypha_sync_plan();
SQL

  echo ""
  echo "✓ Sync plan built"

  # ── Step 7: hypha_sync ──────────────────────────────────────────────────────
  pause
  echo "━━━ Step 7: hypha_sync — apply the diff ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo ""
  echo "  hypha_sync() applies the diff — only changed tables are COPYed to Postgres."
  echo "  Unchanged tables are counted but not re-sent. Returns one row per changed table."
  echo ""

  "$DUCKDB" "$DB_FILE" <<SQL
SELECT * FROM hypha_sync();
SQL

  echo ""
  echo "✓ Incremental sync complete"
  echo ""
  echo "  Verifying updated data in Postgres..."
  psql "${PG_URL}" --no-psqlrc \
    -c "SELECT id, name, email FROM ${DB_NAME}_main.users WHERE id > 200 ORDER BY id;" \
    -c "SELECT * FROM ${DB_NAME}_main.tags;" \
    -c "SELECT 'users' AS table_name, count(*) AS rows FROM ${DB_NAME}_main.users UNION ALL SELECT 'products', count(*) FROM ${DB_NAME}_main.products UNION ALL SELECT 'events', count(*) FROM ${DB_NAME}_main.events UNION ALL SELECT 'tags', count(*) FROM ${DB_NAME}_main.tags UNION ALL SELECT 'empty_log', count(*) FROM ${DB_NAME}_main.empty_log ORDER BY table_name;" \
    2>/dev/null || echo "  (psql not available — check Postgres manually)"

  # ── Step 8: hypha_status (post-sync) ───────────────────────────────────────
  pause
  echo "━━━ Step 8: hypha_status — updated sync state ━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo ""
  echo "  hypha_status() now reflects the sync commit."
  echo ""

  "$DUCKDB" "$DB_FILE" <<SQL > /dev/null
SELECT hypha_status();
SQL

fi

# ── Wrap up ───────────────────────────────────────────────────────────────────
echo ""
echo "━━━ Done ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  Your data is now in Postgres. To explore it:"
echo ""
echo "    psql '${PG_URL}'"
echo "    \\dn                          -- list schemas (look for ${DB_NAME}_main)"
echo "    \\dt ${DB_NAME}_main.*        -- list synced tables"
echo "    SELECT * FROM ${DB_NAME}_main.users LIMIT 5;"
echo ""

# ── Drop prompt ───────────────────────────────────────────────────────────────
echo -n "  Drop the Postgres schemas created by this walkthrough? [y/N] "
read -r DROP_ANSWER
DROP_ANSWER="${DROP_ANSWER:-N}"

if [[ "$DROP_ANSWER" =~ ^[Yy] ]]; then
  "$DUCKDB" "$DB_FILE" <<SQL
SELECT hypha_drop();
SQL
  echo ""
  echo "  ✓ Postgres schemas dropped."
else
  echo ""
  echo "  To clean up later: open the DB and run SELECT hypha_drop();"
fi

echo ""

# ── Keep-file prompt ──────────────────────────────────────────────────────────
echo -n "  Keep the DuckDB file? [Y/n] "
read -r KEEP_ANSWER
KEEP_ANSWER="${KEEP_ANSWER:-Y}"

if [[ "$KEEP_ANSWER" =~ ^[Yy] ]]; then
  echo -n "  Save to [~/Downloads/${DB_NAME}.duckdb]: "
  read -r DEST
  DEST="${DEST:-${HOME}/Downloads/${DB_NAME}.duckdb}"
  # Expand ~ manually in case the user typed a path with it
  DEST="${DEST/#\~/$HOME}"
  mkdir -p "$(dirname "$DEST")"
  mv "$DB_FILE" "$DEST"
  echo ""
  echo "  DuckDB file saved to: ${DEST}"
  echo "  Open it with: ${DUCKDB} ${DEST}"
else
  rm -f "$DB_FILE"
  echo ""
  echo "  DuckDB file cleaned up. Re-run this script to create a fresh one."
fi

echo ""
