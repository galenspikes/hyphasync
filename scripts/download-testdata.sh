#!/usr/bin/env bash
# Download sample DuckDB databases into testdata/ for local testing.
#
# Usage:
#   ./scripts/download-testdata.sh           # download all (excludes very large files)
#   ./scripts/download-testdata.sh tpch      # TPC-H SF1 (250 MB)
#   ./scripts/download-testdata.sh tpch-big  # TPC-H SF3 + SF10 (754 MB + 2.5 GB)
#   ./scripts/download-testdata.sh ferc      # FERC XBRL forms 6, 60, 714 (requires aws CLI)
#   ./scripts/download-testdata.sh ferc-big  # FERC Form 1 + 2 large filings (requires aws CLI)
#   ./scripts/download-testdata.sh stats     # Stack Overflow Stats (~618 MB, CC-BY-SA 4.0)
#   ./scripts/download-testdata.sh demo      # small Timestored demo
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT_DIR}/testdata"
mkdir -p "$DEST"

FILTER="${1:-all}"

dl() {
  local name="$1" url="$2" dest="$3"
  if [[ -f "$dest" ]]; then
    echo "  already present: $dest"
    return
  fi
  echo "  downloading $name..."
  curl -L --progress-bar -o "$dest" "$url"
  echo "  done: $(ls -lh "$dest" | awk '{print $5}') — $dest"
}

dl_s3() {
  local name="$1" s3path="$2" dest="$3"
  if [[ -f "$dest" ]]; then
    echo "  already present: $dest"
    return
  fi
  if ! command -v aws &>/dev/null; then
    echo "  SKIP $name (aws CLI not found — brew install awscli)" >&2
    return
  fi
  echo "  downloading $name from S3..."
  aws s3 cp --no-sign-request "$s3path" "$dest"
  echo "  done: $(ls -lh "$dest" | awk '{print $5}') — $dest"
}

echo "==> Downloading testdata to $DEST (filter: $FILTER)"
echo ""

# ── TPC-H: industry-standard OLAP benchmark (8 tables, DATE/DECIMAL/CHAR heavy)
if [[ "$FILTER" == "all" || "$FILTER" == "tpch" ]]; then
  echo "[tpch-sf1] TPC-H scale factor 1 — ~6M rows (250 MB)"
  dl "tpch-sf1" \
    "https://blobs.duckdb.org/data/tpch-sf1.db" \
    "$DEST/tpch-sf1.db"
fi

if [[ "$FILTER" == "tpch-big" ]]; then
  echo "[tpch-sf3] TPC-H scale factor 3 — ~18M rows (754 MB)"
  dl "tpch-sf3" \
    "https://blobs.duckdb.org/data/tpch-sf3.db" \
    "$DEST/tpch-sf3.db"

  echo "[tpch-sf10] TPC-H scale factor 10 — ~60M rows (2.5 GB)"
  dl "tpch-sf10" \
    "https://blobs.duckdb.org/data/tpch-sf10.db" \
    "$DEST/tpch-sf10.db"
fi

# ── Timestored demo: tiny time-series / financial multi-table DB
if [[ "$FILTER" == "all" || "$FILTER" == "demo" ]]; then
  echo "[demo-stock] Timestored time-series/financial demo (~16 KB)"
  dl "demo-stock" \
    "https://www.timestored.com/data/duckdb-demo.duckdb" \
    "$DEST/demo-stock.duckdb"
fi

# ── FERC regulatory filings (oil pipeline, natural gas, electric grid)
if [[ "$FILTER" == "all" || "$FILTER" == "ferc" ]]; then
  echo "[ferc6] FERC Form 6 — oil pipeline XBRL (~78 MB)"
  dl_s3 "ferc6" \
    "s3://pudl.catalyst.coop/nightly/ferc6_xbrl.duckdb" \
    "$DEST/ferc6-xbrl.duckdb"

  echo "[ferc60] FERC Form 60 — natural gas XBRL (~55 MB)"
  dl_s3 "ferc60" \
    "s3://pudl.catalyst.coop/nightly/ferc60_xbrl.duckdb" \
    "$DEST/ferc60-xbrl.duckdb"

  echo "[ferc714] FERC Form 714 — electric power transfer XBRL (~63 MB)"
  dl_s3 "ferc714" \
    "s3://pudl.catalyst.coop/nightly/ferc714_xbrl.duckdb" \
    "$DEST/ferc714-xbrl.duckdb"
fi

# ── FERC large forms (multi-hundred MB; electric utilities + gas distribution)
if [[ "$FILTER" == "ferc-big" ]]; then
  echo "[ferc1] FERC Form 1 — Annual Report of Major Electric Utilities XBRL (large)"
  dl_s3 "ferc1" \
    "s3://pudl.catalyst.coop/nightly/ferc1_xbrl.duckdb" \
    "$DEST/ferc1-xbrl.duckdb"

  echo "[ferc2] FERC Form 2 — Annual Report of Natural Gas Companies XBRL (large)"
  dl_s3 "ferc2" \
    "s3://pudl.catalyst.coop/nightly/ferc2_xbrl.duckdb" \
    "$DEST/ferc2-xbrl.duckdb"
fi

# ── Stack Overflow Stats: JOB/Stats benchmark (10 tables, complex FK graph)
if [[ "$FILTER" == "all" || "$FILTER" == "stats" ]]; then
  echo "[stats-stackoverflow] Stack Overflow Stats (~618 MB, CC-BY-SA 4.0)"
  echo "  NOTE: Zenodo may throttle downloads; can take 1+ hour on slow connections."
  dl "stats-stackoverflow" \
    "https://zenodo.org/api/records/19131189/files/stats.duckdb/content" \
    "$DEST/stats-stackoverflow.duckdb"
fi

echo ""
echo "==> testdata/ contents:"
ls -lh "$DEST/"
