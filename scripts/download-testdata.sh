#!/usr/bin/env bash
# Download sample DuckDB databases into testdata/ for local testing.
# Usage:
#   ./scripts/download-testdata.sh           # download all
#   ./scripts/download-testdata.sh tpch      # just TPC-H sf1
#   ./scripts/download-testdata.sh ferc      # FERC XBRL files (requires aws CLI)
#   ./scripts/download-testdata.sh stats     # Stack Overflow Stats (618 MB)
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

if [[ "$FILTER" == "all" || "$FILTER" == "tpch" ]]; then
  echo "[tpch-sf1] TPC-H scale factor 1 (250 MB)"
  dl "tpch-sf1" \
    "https://blobs.duckdb.org/data/tpch-sf1.db" \
    "$DEST/tpch-sf1.db"
fi

if [[ "$FILTER" == "all" || "$FILTER" == "demo" ]]; then
  echo "[demo-stock] Timestored time-series/financial demo (~16 KB)"
  dl "demo-stock" \
    "https://www.timestored.com/data/duckdb-demo.duckdb" \
    "$DEST/demo-stock.duckdb"
fi

if [[ "$FILTER" == "all" || "$FILTER" == "ferc" ]]; then
  echo "[ferc6] FERC Form 6 oil pipeline XBRL (~78 MB)"
  dl_s3 "ferc6" \
    "s3://pudl.catalyst.coop/nightly/ferc6_xbrl.duckdb" \
    "$DEST/ferc6-xbrl.duckdb"

  echo "[ferc60] FERC Form 60 natural gas XBRL (~55 MB)"
  dl_s3 "ferc60" \
    "s3://pudl.catalyst.coop/nightly/ferc60_xbrl.duckdb" \
    "$DEST/ferc60-xbrl.duckdb"

  echo "[ferc714] FERC Form 714 electric power transfer XBRL (~63 MB)"
  dl_s3 "ferc714" \
    "s3://pudl.catalyst.coop/nightly/ferc714_xbrl.duckdb" \
    "$DEST/ferc714-xbrl.duckdb"
fi

if [[ "$FILTER" == "all" || "$FILTER" == "stats" ]]; then
  echo "[stats-stackoverflow] Stack Overflow Stats DuckDB (~618 MB, CC-BY-SA 4.0)"
  echo "  NOTE: Zenodo may throttle this download; it can take 1+ hours on a slow connection."
  dl "stats-stackoverflow" \
    "https://zenodo.org/api/records/19131189/files/stats.duckdb/content" \
    "$DEST/stats-stackoverflow.duckdb"
fi

echo ""
echo "==> testdata/ contents:"
ls -lh "$DEST/"
