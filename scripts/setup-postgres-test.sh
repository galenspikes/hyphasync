#!/usr/bin/env bash
# Idempotent setup of the hyphasync test Postgres target.
# Requires: psql accessible in PATH, Postgres running on the given port.
# Usage: ./scripts/setup-postgres-test.sh [port]
#
# In CI the GitHub Actions postgres service creates the hypha user/db directly
# via POSTGRES_USER/POSTGRES_DB env vars — this script is for local native
# Postgres instances where you have a postgres superuser.
set -euo pipefail

PORT="${1:-54329}"

PGPASSWORD="${PGPASSWORD:-}" psql -h 127.0.0.1 -p "$PORT" -U postgres <<'SQL'
DO $$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hypha') THEN
    CREATE ROLE hypha WITH LOGIN PASSWORD 'hypha';
  END IF;
END$$;

SELECT 'CREATE DATABASE hypha_test OWNER hypha'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'hypha_test')\gexec

GRANT ALL PRIVILEGES ON DATABASE hypha_test TO hypha;
SQL

echo "Postgres test target ready: postgresql://hypha:hypha@127.0.0.1:${PORT}/hypha_test"
