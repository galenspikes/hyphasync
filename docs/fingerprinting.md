# hyphasync fingerprinting & hashing — spec v1

Status: **implemented (v1)**. The frozen rules below are in production in
`src/hypha_fingerprint.cpp`. All workflow functions (`hypha_base_snapshot_plan`,
`hypha_base_snapshot`, `hypha_sync_plan`, `hypha_sync`) compute and use
fingerprints. Tracked in [ROADMAP.md](ROADMAP.md).

This document is the single source of truth for how hyphasync computes
fingerprints. Because changing any rule invalidates every stored hash, the v1
rules below are **frozen**: changes require a new version tag (`v2`), not an
edit in place.

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
| VARCHAR / TEXT | `s` | UTF-8 bytes, **Unicode NFC** normalized |
| BLOB / BYTEA | `x` | lowercase hex |
| DATE | `D` | `YYYY-MM-DD` |
| TIME | `T` | `HH:MM:SS.ffffff` (microsecond precision, zero-padded) |
| TIMESTAMP (no tz) | `t` | `YYYY-MM-DDThh:mm:ss.ffffff` (microsecond) |
| TIMESTAMPTZ | `z` | converted to **UTC**: `YYYY-MM-DDThh:mm:ss.ffffffZ` |
| UUID | `u` | canonical lowercase `8-4-4-4-12` |
| ENUM | `e` | the label text, NFC normalized |
| LIST / ARRAY | `L` | length-prefixed concatenation of each element's field encoding, in list order |
| STRUCT / ROW | `R` | for each field in declared order: `s(...)` of the field name, then the value's field encoding |
| MAP | `M` | entries sorted ascending by the canonical encoding of the key; each entry = key field encoding then value field encoding |

### 4.3 Implementation note (initial cut)

The initial implementation MUST support all scalar types above. Nested types
(`L`, `R`, `M`) are **reserved in the spec** but the first implementation MAY
reject a column of a not-yet-supported type with an explicit
`NotImplementedException` ("fingerprinting of type X is not implemented yet").
It MUST NOT silently skip the column.

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

## 7. Comparison hierarchy (how sync uses these)

From cheapest to most expensive — short-circuit at the first level that matches:

1. `object_fingerprint` equal → object unchanged, skip entirely.
2. else `definition_hash` differs → schema change (plan DDL).
3. else `row_count` + `table_hash` equal → data unchanged, skip row diff.
4. else row-level diff: compare `(pk_json → row_hash)` maps to derive
   insert / update / delete sets.

**Performance reality:** with no CDC, levels 3–4 still require a **full scan +
hash of every row** to *compute* the current `table_hash`. `table_hash` lets us
skip *applying* and skip the row-level diff, but not the scan. This is the
inherent cost of the snapshot-diff model and should be communicated to users.
(A future opt-in "watermark column" could bound the scan; out of scope for v1.)

## 8. Versioning & required metadata

The current schema has no place to record the algorithm version. v1 requires a
small additive change (to be made when snapshotting is implemented):

- Record `fingerprint_algo` (e.g. `'v1'`) with each captured snapshot — proposed
  as a new column on `hypha.commit` (and mirrored in remote `hypha` metadata).
- On sync, if the stored `fingerprint_algo` ≠ the running code's version, do
  **not** diff: force a full re-snapshot and log the reason to
  `hypha.event_log`.

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
