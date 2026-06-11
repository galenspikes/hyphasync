# hyphasync fingerprinting & hashing — spec v1 / v2 / v3

Status: **v3 implemented**. The rules below are in production in
`src/hypha_fingerprint.cpp`. All workflow functions (`hypha_base_snapshot_plan`,
`hypha_base_snapshot`, `hypha_sync_plan`, `hypha_sync`) compute and use
fingerprints. Roadmap and planning are tracked on the `develop` branch.

v3 replaces the O(n) full-row sha256 scan with a fast rowid-statistics fingerprint
(see §6.4). v2 column type encoding (`FieldEncodingExpr`, `RowHashExpr`) is retained
for row-level diff in `hypha.row_hash` but is no longer used for `table_hash`.

This document is the single source of truth for how hyphasync computes
fingerprints. Because changing any rule invalidates every stored hash, the v1
scalar rules are **frozen**. v2 extends them with nested-type support; further
changes require a `v3` tag.

---

## 1. Purpose

hyphasync syncs by **snapshot-diff**, not CDC/WAL. To answer "what changed since
the last snapshot?" each sync recomputes fingerprints of the current local state
and compares them against the fingerprints recorded for what was last pushed to
the target. Fingerprints are therefore the correctness foundation of the entire
sync pipeline — a wrong fingerprint means either a spurious re-sync (annoying) or
**silent data divergence** (unacceptable).

## 2. Core principle: single-engine hashing

**All hashes are computed in DuckDB. hyphasync never compares a DuckDB-computed
hash against a Postgres-computed hash.**

The remote target's fingerprints are the values hyphasync itself computed (in
DuckDB) at push time and recorded in the remote `hypha` metadata. A sync compares:

```
sha(local rows now)   vs   sha(rows we last pushed)   ← both computed in DuckDB
```

This eliminates the entire class of cross-engine canonicalization mismatches
(float formatting, NUMERIC trailing zeros, timestamp precision/zone, text
collation, bytea hex, NULL/bool spellings). If independent remote verification is
ever needed, remote rows are pulled back into DuckDB and hashed with the *same*
code path — never hashed by Postgres.

## 3. Algorithm

- **Hash function:** SHA-256 (full 256-bit, collision-safe).
- **Encoding:** lowercase hex, 64 characters. Stored in the existing `VARCHAR`
  hash columns.
- **Version tag:** `v1`. Every stored fingerprint is associated with its
  algorithm version (see §8). A stored fingerprint whose version differs from the
  running code's version is **never diffed**; it forces a full re-snapshot.

## 4. Canonical value encoding (TEXT form)

Values are serialized to a canonical, human-readable text form before hashing.
Concatenation is made unambiguous with **explicit length prefixes** (so that
`("ab","c")` and `("a","bc")` can never produce the same bytes).

### 4.1 Field framing

Each value is encoded as a field:

```
field := tag "(" byte_length ")" ":" payload
```

- `tag` — one character identifying the logical type (table below).
- `byte_length` — decimal byte length of `payload` (UTF-8).
- `payload` — the canonical string for the value (may be empty).

A row / key is the concatenation of its field encodings joined by the ASCII unit
separator `0x1F`. The length prefix already guarantees injectivity; the separator
is for readability.

### 4.2 Type tags and canonical payloads

| Type | tag | Canonical payload |
|------|-----|-------------------|
| NULL (any type) | `n` | empty — distinct from empty string, `0`, and `false` |
| BOOLEAN | `b` | `0` or `1` |
| Integer (all widths, signed/unsigned) | `i` | decimal, no leading zeros, leading `-` if negative, `0` for zero |
| DECIMAL / NUMERIC | `d` | sign + digits with a single `.`; **trailing fractional zeros stripped**; no exponent; canonical zero is `0` (e.g. `1.50`→`1.5`, `0.00`→`0`, `-0`→`0`) |
| REAL / FLOAT / DOUBLE | `f` | shortest round-trippable decimal repr; special tokens `nan`, `inf`, `-inf`; `-0.0`→`0` |
| VARCHAR / TEXT | `s` | UTF-8 bytes; length prefix is byte count via `strlen()` |
| BLOB / BYTEA | `x` | lowercase hex |
| DATE | `D` | `YYYY-MM-DD` |
| TIME | `T` | `HH:MM:SS.ffffff` (microsecond precision, zero-padded) |
| TIMESTAMP (no tz) | `t` | `YYYY-MM-DDThh:mm:ss.ffffff` (microsecond) |
| TIMESTAMPTZ | `z` | converted to **UTC**: `YYYY-MM-DDThh:mm:ss.ffffffZ` |
| UUID | `u` | canonical lowercase `8-4-4-4-12` |
| ENUM | `e` | the label text, NFC normalized |
| LIST / ARRAY | `L` | DuckDB `::JSON::VARCHAR` serialization (JSON array), length-prefixed |
| STRUCT / ROW | `R` | DuckDB `::JSON::VARCHAR` serialization (JSON object, fields in declaration order), length-prefixed |
| MAP | `M` | DuckDB `::JSON::VARCHAR` serialization (JSON object; DuckDB sorts map keys in output), length-prefixed |
| JSON | `J` | DuckDB `::JSON::VARCHAR` serialization (canonical JSON text), length-prefixed |

### 4.3 Implementation note

Scalar types follow the canonical encoding above exactly.

Nested/document types (`L`, `R`, `M`, `J`) use DuckDB's native `::JSON::VARCHAR`
cast as the payload instead of the recursive sub-field encoding the spec
describes.  This satisfies the **same-engine comparison guarantee** (§2) — both
the "rows now" hash and the "rows we last pushed" hash are computed by the same
DuckDB code path, so format differences never produce false positives.

The byte-length prefix for these payloads is computed with `strlen()` (UTF-8
byte length), **not** `octet_length(CAST(... AS BLOB))`: DuckDB rejects the
`VARCHAR → BLOB` cast for any string containing a non-ASCII byte ("Invalid byte
encountered in STRING -> BLOB conversion"), which would crash fingerprinting of
JSON holding accented/Unicode text. `strlen()` returns the same byte count for
ASCII payloads, so this changes no existing ASCII fingerprint.

The `J` encoding is **byte-exact**: it hashes the engine's canonical JSON text
without additional key-sort or whitespace normalization. Re-reading the same
stored value is deterministic, so there are no in-engine false positives; the
accepted tradeoff is that an upstream writer reformatting a JSON value
(reordering keys, changing whitespace) registers as a change. A SQL `NULL`
encodes as `n():`; a JSON `null` literal encodes as `J(4):null` — the two are
kept distinct.

The spec's full recursive sub-field encoding (each element itself a tagged field)
is reserved for a future major revision if cross-version hash stability for
nested types is required.

The JSON tag `J` was added **additively** within `v3`: it changes no existing
tag's bytes, and JSON columns previously produced no row hash (they fell back to
the `MUTABLE_ENTITY` strategy), so there is no prior `J` hash to invalidate. No
algorithm-version bump is required — tables containing JSON columns re-fingerprint
to `EXACT` on their next snapshot and self-heal, instead of forcing a full
re-snapshot for every user.

An implementation MUST NOT silently skip a column of an unsupported type; it
MUST throw `NotImplementedException` naming the column and type.

## 5. Row hash and row identity

### 5.1 Row hash

```
row_hash = sha256_hex( join_0x1F( field_encoding(col) for col in identity_order ) )
```

`identity_order` = all columns ordered by ordinal position. Stored in
`hypha.row_hash.row_hash`.

### 5.2 Row identity (`pk_json`)

Row-level diffing needs a stable key, stored as canonical JSON in
`hypha.row_hash.pk_json`:

- **Table has a PRIMARY KEY or UNIQUE constraint:** key = those columns. `pk_json`
  is a JSON object of `{column: canonical_text_value}` with keys sorted ascending.
- **Table has no key (chosen policy: full-row identity):** the row hash itself is
  the identity. `pk_json = {"_hypha_rowhash": "<row_hash hex>"}`.
  - **Documented limitation:** duplicate (fully identical) rows are
    indistinguishable. v1 handles this as a *multiset*: identity is
    `(row_hash, occurrence_count)`. `hypha.table_snapshot.row_count` plus the
    per-hash count detect duplicate-count changes. True per-duplicate-row updates
    on keyless tables cannot be expressed as targeted updates and will be applied
    as delete-all-matching + re-insert for that hash group.
  - This policy is recorded per table so a later sync can detect if a key was
    added/removed.

## 6. Table, object, and definition hashes

### 6.1 `table_hash` (content of a base table)

Order-independent over rows, duplicate-correct:

```
table_hash = sha256_hex( concat( row_hash for each row ORDER BY row_hash ASC, sep="\n" ) )
```

Computable directly in DuckDB, e.g.
`sha256(string_agg(row_hash, '\n' ORDER BY row_hash))`. Sorting (not XOR) is used
so that duplicate rows are preserved correctly. Stored in
`hypha.table_snapshot.table_hash`; `row_count` is stored alongside as a cheap
pre-check.

### 6.2 `definition_hash` (schema/DDL of an object)

Hash of the normalized structure, derived from `hypha.column_snapshot` so it is
engine-stable:

```
definition_hash = sha256_hex( join_0x1F(
    field("schema_name"), field("object_name"), field("object_type"),
    for each column ORDER BY ordinal_position:
        field(column_name), field(ordinal_position), field(duckdb_type),
        field(postgres_type), field(is_nullable), field(default_expr),
    field(primary_key_columns_csv)
) )
```

Stored in `hypha.object_snapshot.definition_hash`.

### 6.3 `content_hash` and `object_fingerprint`

- `content_hash` = `table_hash` for base tables (reserved for view/materialized
  content in future). Stored in `hypha.object_snapshot.content_hash`.
- `object_fingerprint = sha256_hex( definition_hash + ":" + content_hash )`.
  Stored in `hypha.object_snapshot.object_fingerprint`. This single value answers
  "did anything about this object change?" in one comparison.

## 6.4 v3 fingerprint strategy classifier (current algorithm)

The classifier assigns each table exactly one strategy. **EXACT is tried first**: when a
table is small enough that a full per-row sha256 costs < 1 MB, it is both cheap *and*
blind-spot-free, so it beats every statistical strategy. The remaining strategies are
large-table fast paths reached only once EXACT is too expensive — a small table is never
silently downgraded to a less-precise fingerprint. Classification priority (highest first):

1. **EXACT** — estimated bytes < 1 MB.
2. **CHEMINFORMATICS_COMPOUNDS** — a SMILES/InChI/mol structure column is present.
3. **CHEMINFORMATICS_ASSAY** — a numeric IC50/EC50/Ki/activity column is present.
4. **APPEND_ONLY** — a monotonic integer PK or timestamp ordering column is present.
5. **WIDE_ANALYTICAL** — more than 50 columns.
6. **MUTABLE_ENTITY** — default.

The structural strategies (EXACT, APPEND_ONLY, MUTABLE_ENTITY) are described below;
the domain-semantic and wide strategies follow in §6.5.

### EXACT (estimated bytes < 1 MB)

```sql
table_hash = sha256(string_agg(sha256(each_row_encoding), chr(10) ORDER BY row_hash))
```

Full per-row sha256 using the canonical `FieldEncodingExpr` for every column (§4).
Exact and deterministic. Applied when the estimated serialized table size is below the
1 MB cost threshold: `estimated_bytes_per_row × row_count < 1,048,576 bytes`. This
correctly promotes wide tables with few rows (e.g. 5k rows × 200 bytes/row ≈ 1 MB →
EXACT) and demotes narrow tables with many rows (e.g. 200k rows × 11 bytes/row ≈ 2.2 MB
→ not EXACT). The per-row byte estimate is computed from column type heuristics and does
not require a full table scan.

### APPEND_ONLY (has monotonic integer PK or timestamp ordering column)

```sql
table_hash = sha256(COUNT(*)::text || '|' || COALESCE(MAX(pk_col)::text, ''))
```

Detects inserts (the only mutation type for append-only tables) in O(1). Triggered
when a column named `id`, `*_id`, `seq`, `*_seq`, `pk_*`, `created_at`, `*_at`,
`*_ts`, etc. is found. Falls through to MUTABLE_ENTITY when no such column exists.

### MUTABLE_ENTITY (default — all other tables)

```sql
table_hash = sha256(
    COUNT(*)::text                    || '|' ||
    COALESCE(MIN(rowid)::text, '')    || '|' ||
    COALESCE(MAX(rowid)::text, '')
)
```

DuckDB maintains zone-map metadata for the internal `rowid` pseudo-column.
`COUNT(*)`, `MIN(rowid)`, and `MAX(rowid)` are typically answered directly from
zone-map statistics — O(1), no full row scan.

**Sensitivity:** Detects inserts (`COUNT` and `MAX(rowid)` increase), deletes
(`COUNT`, `MIN`/`MAX(rowid)` change), and most updates (DuckDB moves updated rows
to new rowid slots, changing `MAX(rowid)`).

**Known limitation:** In-place updates that preserve rowid assignments (rare in
DuckDB's current storage engine) may not be detected. The definition_hash still
catches all schema changes regardless.

**Mitigations for the blind spot.** Two opt-in tools close this gap:

- **`exact_verify` mode** — `hypha_init(conn, max_rows, fast_mode, exact_verify := true)`
  forces the `EXACT` strategy (full per-row SHA-256) for *every* table regardless of
  size, so no change is ever missed. Cost: O(n) scan + hash per table on each snapshot
  and sync. `ClassifyTable(..., force_exact=true)` short-circuits to `EXACT` (falling back
  to structural classification only when a column type cannot be hashed).
- **`hypha_verify()`** — an on-demand tripwire that recomputes the `EXACT` table_hash for
  every table and compares it to a baseline in `hypha.verify_state`. It reports
  `VERIFY_DRIFT` (an in-place change the fast fingerprint would miss — Postgres is stale)
  versus `VERIFY_PENDING` (a change `hypha_sync()` will catch), then advances the baseline.
  Lets the fast path stay fast while still catching blind-spot drift on demand.

## 6.5 Domain-semantic and wide strategies

These are large-table fast paths: each is reached only when EXACT is too expensive, so a
small table that would hash exactly is never downgraded to one of them.

### CHEMINFORMATICS_COMPOUNDS (a structure-identity column is present)

```sql
table_hash = sha256(COUNT(*) || '|' || MIN(structure_col) || '|' || MAX(structure_col))
```

Triggered when a column name contains `smiles` or `inchi`, or is `mol`/`molblock`/`molfile`
(or `*_mol`). Compound registries are dominated by inserts and structure replacements; the
structure string is an effectively immutable identity, so `COUNT` + `MIN`/`MAX` over it
detects those mutations far more cheaply than a full per-row hash.

### CHEMINFORMATICS_ASSAY (a numeric potency/activity column is present)

```sql
table_hash = sha256(COUNT(*) || '|' || MIN(val) || '|' || MAX(val) || '|' || AVG(val))
```

Triggered when a **numeric** column name contains `ic50`/`ec50`/`ac50`/`cc50`/`pic50`/
`activity`/`potency`, or is `ki`/`kd` (or `*_ki`/`*_kd`). Key columns (`id`, `*_id`) are
excluded so foreign keys like `bioactivity_type_id` are not mistaken for measurements.
Adding `AVG` to the count/min/max signal catches bulk value revisions that leave the
extremes unchanged.

### WIDE_ANALYTICAL (> 50 columns)

```sql
table_hash = sha256(COUNT(*)
    || MIN(c1) || MAX(c1) || MIN(c2) || MAX(c2) || MIN(c3) || MAX(c3))
```

For very wide tables, hashing every column of every row is expensive and rarely necessary.
`COUNT` plus `MIN`/`MAX` over the first three *simple scalar* columns (numeric, text, date,
timestamp, boolean, uuid) gives a cheap change signal. Falls through to MUTABLE_ENTITY if no
simple scalar column exists.

### Strategy change is self-healing (no algorithm-version bump)

Adding these strategies changes the `table_hash` formula for tables that newly match them
(previously MUTABLE_ENTITY / APPEND_ONLY). On the next sync the stored hash (old formula)
differs from the freshly computed hash (new formula), so the table is re-synced **once** and
the new baseline is stored — it errs toward a spurious re-sync, never toward silent
divergence. As with the additive JSON tag, no `fingerprint_algo` bump is required.

## 7. Comparison hierarchy (how sync uses these)

From cheapest to most expensive — short-circuit at the first level that matches:

1. `object_fingerprint` equal → object unchanged, skip entirely.
2. else `definition_hash` differs → schema change (plan DDL).
3. else `row_count` + `table_hash` equal → data unchanged, skip row diff.
4. else row-level diff: compare `(pk_json → row_hash)` maps to derive
   insert / update / delete sets.

**Performance reality (v3):** MUTABLE_ENTITY and APPEND_ONLY `table_hash` values
are computed in O(1) via DuckDB zone-map statistics — no full row scan. EXACT
tables (estimated bytes < 1 MB) do a full per-row sha256, which is fast in practice
given their small serialized size. Row-level diff (level 4) still requires reading
`hypha.row_hash` entries captured during the snapshot plan, but those were captured
at snapshot time and do not require re-scanning the source table on sync.

*Historical note (v2):* The v2 algorithm required a full scan + per-row sha256 +
sort to compute `table_hash` for all tables, which was O(n) and took >5 minutes on
8.8 GB tables. v3 eliminates this bottleneck for large tables.

## 8. Versioning & required metadata

The `fingerprint_algo` column on `hypha.commit` records the algorithm version
used for each snapshot.  On sync, if the stored `fingerprint_algo` ≠ the
running code's version, hyphasync refuses to diff and forces a full re-snapshot,
logging the reason to `hypha.event_log`.

### Version history

| Version | `HYPHA_FINGERPRINT_ALGO` | Description |
|---------|--------------------------|-------------|
| v1 | `v1` | Scalar types only. Nested types (LIST/STRUCT/MAP) throw `NotImplementedException`. |
| v2 | `v2` | Adds nested type support (tags `L`, `R`, `M`) using DuckDB `::JSON::VARCHAR` as payload. All users with v1 snapshots must run `hypha_base_snapshot()` once to re-establish a v2 baseline before syncing. |
| v3 | `v3` | Replaces the O(n) full-row sha256 scan with a fast rowid-statistics fingerprint (§6.4). All column type support from v2 is retained; only `table_hash` computation changes. All users with v2 snapshots must run `hypha_base_snapshot()` once to re-establish a v3 baseline. Later extended additively with the JSON tag `J` (first-class `JSON`-type fingerprinting) — no version bump, since the change is additive and JSON tables self-heal on next snapshot. |

### Migration from v2 to v3

When `RunSync` detects a stored `fingerprint_algo = 'v2'` against running code
`v3`, it raises:

```
fingerprint_algo mismatch: the last applied snapshot used 'v2' but the current
code uses 'v3'. Run hypha_base_snapshot() to re-establish a baseline first.
```

Run `hypha_base_snapshot()` once; all subsequent syncs proceed normally with the
faster v3 fingerprinting.

### Migration from v1 to v2

When `RunSync` detects a stored `fingerprint_algo = 'v1'` against running code
`v2`, it raises:

```
fingerprint_algo mismatch: the last applied snapshot used 'v1' but the current
code uses 'v2'. Run hypha_base_snapshot() to re-establish a baseline first.
```

Run `hypha_base_snapshot()` once; all subsequent syncs proceed normally.

## 9. Safety rules (no silent failures)

- Unsupported type for hashing → **throw** an explicit error naming the
  column/type. Never skip a column.
- No usable row identity and policy unsatisfiable → **throw**, naming the table.
- Version mismatch (§8) → force full re-snapshot, never a partial/ambiguous diff.
- Every capture and diff decision (skipped table, forced re-snapshot, rejected
  type) is recorded in `hypha.event_log` with a level and code.

## 10. Build order

Fingerprinting is fully buildable and unit-testable **before** any remote-write
code exists:

1. Freeze this spec (done — this document).
2. Implement scalar `field_encoding()` + `row_hash` capture into
   `hypha.row_hash`, with golden-vector tests (known value → known hex).
3. Implement `table_hash` + `row_count` into `hypha.table_snapshot`.
4. Implement `definition_hash` / `content_hash` / `object_fingerprint` into
   `hypha.object_snapshot`.
5. Add the `fingerprint_algo` metadata (§8).
6. Only then build `hypha_sync_plan()` on top of the comparison hierarchy (§7).

## 11. Test strategy

- **Golden vectors:** a table of `(type, value) → expected canonical payload →
  expected sha256 hex`, checked in and asserted, so any accidental change to the
  rules fails loudly.
- **Determinism:** same data hashed twice (and across DuckDB restarts) yields
  identical fingerprints.
- **Order independence:** `table_hash` is identical regardless of row insertion
  order.
- **Sensitivity:** NULL ≠ `''` ≠ `0` ≠ `false`; `1.50` == `1.5`; tz-equal
  timestamps in different zones hash equal.
- **Duplicates:** keyless tables with duplicate rows produce stable, correct
  multiset hashes.
