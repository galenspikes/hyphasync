#!/usr/bin/env bash
# Build testdata/frankenstein.duckdb — a multi-schema stress-test database
# assembled from TPC-H, FERC714, FERC6, FERC60, and synthetic generated data.
#
# Schemas:
#   analytics  — TPC-H (8 core tables, ~8.7 M rows, DECIMAL/DATE/CHAR)
#   grid       — FERC714 electric power transfer (all tables, ~19.7 M rows)
#   pipeline   — FERC6 oil pipelines top-20 tables
#   gas        — FERC60 natural gas companies top-20 tables
#   synthetic  — generated data: UUID, BOOLEAN, TINYINT, DECIMAL, TIMESTAMPTZ, INTERVAL
#
# Usage: ./scripts/build-frankenstein.sh
# Requires: make release done; source DBs in testdata/
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DB="${ROOT_DIR}/build/release/duckdb"
OUT="${ROOT_DIR}/testdata/frankenstein.duckdb"

[[ -x "$DB" ]] || { echo "No binary at $DB — run 'make release' first." >&2; exit 1; }
for src in tpch-sf1.db ferc714-xbrl.duckdb ferc6-xbrl.duckdb ferc60-xbrl.duckdb; do
  [[ -f "testdata/$src" ]] || { echo "Missing testdata/$src — run scripts/download-testdata.sh." >&2; exit 1; }
done

rm -f "$OUT"
START=$(date +%s)
echo "==> Building frankenstein.duckdb ..."

# ── Helper: copy tables from a source DB into a target schema.
# Skips tables with > MAX_COLS columns (ultra-wide XBRL tables) and any
# that fail (e.g. name clash). Prints one line per table.
copy_tables() {
  local src_file="$1" src_alias="$2" target_schema="$3" limit="$4" max_cols="${5:-500}"
  echo "  [${target_schema}] querying source tables..."
  # Get top-N tables by estimated_size, skipping hypha metadata and ultra-wide tables
  local tables
  tables=$("$DB" -noheader -list -c "
    ATTACH '${src_file}' AS __src (READ_ONLY);
    SELECT t.table_name
    FROM duckdb_tables() t
    WHERE t.database_name = '__src'
      AND t.internal = false AND t.temporary = false
      AND t.schema_name = 'main'
      AND t.table_name NOT IN ('target','commit','object_snapshot','column_snapshot',
                               'table_snapshot','row_hash','event_log','meta')
      AND (SELECT COUNT(*) FROM duckdb_columns() c
           WHERE c.database_name='__src' AND c.table_name=t.table_name) <= ${max_cols}
    ORDER BY t.estimated_size DESC
    LIMIT ${limit};
  " 2>/dev/null)

  local n=0
  while IFS= read -r tbl; do
    [[ -z "$tbl" ]] && continue
    # Sanitize: truncate to safe target name (avoid Postgres-style collisions later)
    local target_name="${tbl}"
    "$DB" "$OUT" -c "
      ATTACH '${src_file}' AS __src (READ_ONLY);
      CREATE SCHEMA IF NOT EXISTS ${target_schema};
      CREATE TABLE ${target_schema}.\"${target_name}\" AS SELECT * FROM __src.main.\"${tbl}\";
    " 2>/dev/null && n=$((n+1)) || echo "    SKIP: ${tbl}"
  done <<< "$tables"
  echo "  [${target_schema}] copied ${n} tables"
}

# ── analytics: TPC-H core tables (8 tables, ~8.7 M rows)
echo "  [analytics] copying TPC-H..."
"$DB" "$OUT" <<'SQL'
ATTACH 'testdata/tpch-sf1.db' AS tpch (READ_ONLY);
CREATE SCHEMA analytics;
CREATE TABLE analytics.lineitem  AS SELECT * FROM tpch.main.lineitem;
CREATE TABLE analytics.orders    AS SELECT * FROM tpch.main.orders;
CREATE TABLE analytics.partsupp  AS SELECT * FROM tpch.main.partsupp;
CREATE TABLE analytics.part      AS SELECT * FROM tpch.main.part;
CREATE TABLE analytics.customer  AS SELECT * FROM tpch.main.customer;
CREATE TABLE analytics.supplier  AS SELECT * FROM tpch.main.supplier;
CREATE TABLE analytics.nation    AS SELECT * FROM tpch.main.nation;
CREATE TABLE analytics.region    AS SELECT * FROM tpch.main.region;
SQL
echo "  [analytics] done"

# ── grid: FERC714 all tables (~19.7 M rows, electric power grid)
copy_tables "testdata/ferc714-xbrl.duckdb" f714 grid 20

# ── pipeline: FERC6 top-20 tables (oil pipelines)
copy_tables "testdata/ferc6-xbrl.duckdb" f6 pipeline 20

# ── gas: FERC60 top-20 tables, skip the 1355-column monster
copy_tables "testdata/ferc60-xbrl.duckdb" f60 gas 20 400

# ── synthetic: generated data covering all common types
echo "  [synthetic] generating type-coverage tables..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA synthetic;

-- 500K rows, 15 column types (UUID, BOOL, TINYINT, SMALLINT, INT, BIGINT, UBIGINT,
-- FLOAT, DOUBLE, DECIMAL, TEXT, DATE, TIMESTAMP, TIMESTAMPTZ, INTERVAL)
CREATE TABLE synthetic.type_showcase AS
SELECT
    range                                                                AS id,
    gen_random_uuid()                                                    AS uid,
    range % 2 = 0                                                        AS is_active,
    (range % 127)::TINYINT                                               AS tiny_val,
    (range % 32767)::SMALLINT                                            AS small_val,
    range::INTEGER                                                       AS int_val,
    range::BIGINT * 1000                                                 AS big_val,
    range::UBIGINT * 2                                                   AS ubig_val,
    (random() * 1000)::FLOAT                                             AS float_val,
    random() * 1000000                                                   AS double_val,
    round(random() * 9999, 2)::DECIMAL(10,2)                             AS decimal_val,
    'record_' || range::VARCHAR                                          AS text_val,
    (DATE '2020-01-01' + (range % 1825)::INTEGER)                        AS date_val,
    (TIMESTAMP '2020-01-01' + INTERVAL (range * 60 || ' seconds'))       AS ts_val,
    ((TIMESTAMP '2020-01-01' + INTERVAL (range * 60 || ' seconds'))
     ::TIMESTAMPTZ)                                                       AS tstz_val,
    INTERVAL ((range % 365)::VARCHAR || ' days')                         AS interval_val
FROM range(1, 500001);

-- 1M row event stream (time-series style)
CREATE TABLE synthetic.events AS
SELECT
    range                                                                 AS event_id,
    ('evt_' || (range % 100)::VARCHAR)                                   AS event_type,
    (TIMESTAMP '2023-01-01' + INTERVAL (range * 30 || ' seconds'))
        ::TIMESTAMPTZ                                                     AS occurred_at,
    round(random() * 100, 4)::DECIMAL(8,4)                               AS magnitude,
    ('src_' || (range % 20)::VARCHAR)                                    AS source_id,
    range % 1000 = 0                                                      AS is_anomaly,
    chr((65 + (range % 26))::INTEGER)::VARCHAR                           AS category
FROM range(1, 1000001);

-- Small reference/lookup table
CREATE TABLE synthetic.lookup AS
SELECT
    range                                                        AS lookup_id,
    ('code_' || lpad(range::VARCHAR, 4, '0'))::VARCHAR           AS code,
    ('Description for entry ' || range)::VARCHAR                AS description,
    range % 10 = 0                                               AS is_deprecated,
    (DATE '2010-01-01' + range::INTEGER)                         AS effective_date
FROM range(1, 1001);

-- Build manifest
CREATE TABLE synthetic.build_manifest (
    key      VARCHAR NOT NULL,
    value    VARCHAR,
    built_at TIMESTAMP DEFAULT now()
);
INSERT INTO synthetic.build_manifest (key, value) VALUES
    ('source_tpch',    'tpch-sf1.db'),
    ('source_ferc714', 'ferc714-xbrl.duckdb'),
    ('source_ferc6',   'ferc6-xbrl.duckdb'),
    ('source_ferc60',  'ferc60-xbrl.duckdb'),
    ('schemas',        'analytics,grid,pipeline,gas,synthetic'),
    ('purpose',        'hyphasync stress-test / type-coverage frankenstein DB'),
    ('see_also',       'scripts/build-cheminformatics.sh for 1B+ row domain DB');
SQL
echo "  [synthetic] done"

END=$(date +%s)
echo ""
echo "==> Build complete in $((END - START))s"
echo ""
echo "==> Schema summary:"
"$DB" "$OUT" -c "
SELECT
    schema_name,
    COUNT(*)            AS tables,
    SUM(estimated_size) AS approx_rows
FROM duckdb_tables()
WHERE database_name = current_database()
  AND schema_name NOT IN ('hypha','information_schema')
  AND internal = false
GROUP BY schema_name
ORDER BY schema_name;
"
echo ""
echo "==> File size: $(ls -lh "$OUT" | awk '{print $5}')"
