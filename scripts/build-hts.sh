#!/usr/bin/env bash
# Build testdata/hts-pipeline.duckdb — raw high-throughput screening pipeline data.
#
# This is pure velocity data: every plate read, every well signal, every
# dose-response point. No attempt at biological realism — just the firehose.
#
# Tables (row counts):
#   hts.instruments         500       plate reader configs
#   hts.assay_batches       100,000   campaign-level batch records
#   hts.plates            2,000,000   per-plate QC metadata
#   hts.wells         1,500,000,000   ← raw well-level signal data (THE BIG ONE)
#   hts.dr_points         500,000,000 dose-response curve measurements
#   hts.compound_funnel   100,000,000 hit progression tracking per compound per stage
#   hts.hit_calls          50,000,000 final hit classification per compound per assay
#   hts.qc_daily            200,000   daily instrument QC records
#
# Usage: ./scripts/build-hts.sh [--fast]
#   --fast: 10M wells + 5M DR points (quick sanity check)
#
# Requires: make release done (./build/release/duckdb must exist)
# Runtime:  ~20-60 min full; ~30s fast
set -euo pipefail

FAST=false
for arg in "$@"; do [[ "$arg" == "--fast" ]] && FAST=true; done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DB="${ROOT_DIR}/build/release/duckdb"
OUT="${ROOT_DIR}/testdata/hts-pipeline.duckdb"

[[ -x "$DB" ]] || { echo "No binary at $DB — run 'CC=gcc CXX=g++ make release' first." >&2; exit 1; }

rm -f "$OUT"
mkdir -p "$(dirname "$OUT")"
START=$(date +%s)
progress() { echo ""; echo "  [$(( $(date +%s) - START ))s] $*"; }

if [[ "$FAST" == "true" ]]; then
  WELLS_N=10000000
  DR_N=5000000
  FUNNEL_N=1000000
  HITS_N=500000
else
  WELLS_N=1500000000
  DR_N=500000000
  FUNNEL_N=100000000
  HITS_N=50000000
fi

echo "==> Building hts-pipeline.duckdb ..."
echo "    fast=${FAST}  wells=${WELLS_N}  dr_points=${DR_N}"

# ─────────────────────────────────────────────────────────────────────────────
# instruments + assay_batches (small, static context)
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] instruments + batches..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA hts;

-- Plate reader instruments
CREATE TABLE hts.instruments AS
SELECT
    (range + 1)::SMALLINT                                     AS instrument_id,
    CASE (range % 5)::INT
        WHEN 0 THEN 'EnVision'        -- PerkinElmer multimode
        WHEN 1 THEN 'PHERAstar FSX'   -- BMG Labtech multimode
        WHEN 2 THEN 'CLARIOstar Plus' -- BMG Labtech
        WHEN 3 THEN 'Synergy Neo2'    -- BioTek multimode
        ELSE        'FLIPR Tetra'     -- Molecular Devices GPCR/ion channel
    END                                                       AS model,
    CASE (range % 5)::INT
        WHEN 0 THEN 'PerkinElmer'
        WHEN 1 THEN 'BMG Labtech'
        WHEN 2 THEN 'BMG Labtech'
        WHEN 3 THEN 'Agilent/BioTek'
        ELSE        'Molecular Devices'
    END                                                       AS manufacturer,
    CASE (range % 4)::INT
        WHEN 0 THEN 'fluorescence_intensity'
        WHEN 1 THEN 'luminescence'
        WHEN 2 THEN 'time_resolved_fluorescence'
        ELSE        'fluorescence_polarization'
    END                                                       AS detection_mode,
    ('INST-' || lpad((range + 1)::VARCHAR, 3, '0'))           AS serial_number,
    ('Lab-' || ((range % 8) + 1)::VARCHAR)                    AS lab_location,
    (DATE '2010-01-01' + ((hash(range * 7) % 5475)::INTEGER)) AS commissioned_date,
    (range % 20 != 0)                                         AS is_operational,
    -- max plates/day capacity
    CASE (range % 5)::INT
        WHEN 0 THEN 200 WHEN 1 THEN 150 WHEN 2 THEN 120
        WHEN 3 THEN 300 ELSE 180
    END::SMALLINT                                             AS capacity_plates_per_day
FROM range(0, 500);

-- Assay batch / campaign records
-- Each batch is one assay run against a subset of the compound library
CREATE TABLE hts.assay_batches AS
SELECT
    (range + 1)::INTEGER                                      AS batch_id,
    ('BATCH-' || lpad((range + 1)::VARCHAR, 6, '0'))          AS batch_code,

    -- Assay type (maps loosely to bioassay.assays)
    CASE (range % 8)::INT
        WHEN 0 THEN 'primary_HTS'       -- single-point 10 µM screen
        WHEN 1 THEN 'primary_HTS'
        WHEN 2 THEN 'confirmation'      -- retest actives in duplicate
        WHEN 3 THEN 'dose_response'     -- 10-point curve
        WHEN 4 THEN 'selectivity'       -- counter-screen panel
        WHEN 5 THEN 'cytotox'           -- cell viability counter
        WHEN 6 THEN 'ADME_panel'        -- metabolic stability, solubility
        ELSE        'mechanistic'       -- MOA follow-up
    END                                                       AS batch_type,

    -- Target (simplified)
    ('TGT-' || lpad(((range * 7) % 100000 + 1)::VARCHAR, 6, '0'))
                                                              AS target_id,

    -- Compound library subset
    CASE (range % 4)::INT
        WHEN 0 THEN 'full_deck'         -- all 10M compounds
        WHEN 1 THEN 'diversity_set'     -- ~500K diverse subset
        WHEN 2 THEN 'focused_set'       -- ~50K target-class biased
        ELSE        'actives_only'      -- just confirmed hits
    END                                                       AS library_subset,

    -- Plate format
    CASE (range % 3)::INT
        WHEN 0 THEN 384 WHEN 1 THEN 1536 ELSE 96
    END::SMALLINT                                             AS plate_format,

    -- Test concentration (µM)
    CASE (range % 5)::INT
        WHEN 0 THEN 10.000 WHEN 1 THEN 5.000 WHEN 2 THEN 1.000
        WHEN 3 THEN 0.100  ELSE 20.000
    END::DECIMAL(8,3)                                         AS test_conc_um,

    ((range % 500) + 1)::SMALLINT                             AS instrument_id,
    (DATE '2005-01-01' + ((hash(range * 11) % 7671)::INTEGER)) AS run_date,

    -- How many plates in this batch
    CASE (range % 4)::INT
        WHEN 0 THEN ((hash(range * 13) % 1000) + 100)::INTEGER   -- large primary
        WHEN 1 THEN ((hash(range * 13) % 200)  + 10)::INTEGER    -- confirmation
        WHEN 2 THEN ((hash(range * 13) % 50)   + 5)::INTEGER     -- DR
        ELSE        ((hash(range * 13) % 30)   + 2)::INTEGER     -- focused
    END                                                       AS plate_count,

    (range % 10 = 0)                                          AS is_archived,

    -- Overall batch Z' (quality metric; >0.5 is acceptable)
    round(0.3 + (hash(range * 17) % 700)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                              AS zprime_median,

    -- Signal-to-background ratio
    round(2.0 + (hash(range * 19) % 9800)::DOUBLE / 100.0, 2)::DECIMAL(7,2)
                                                              AS sb_ratio_median

FROM range(0, 100000);
SQL
progress "[hts.instruments + assay_batches] done"

# ─────────────────────────────────────────────────────────────────────────────
# plates — 2M plate QC records
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] building plates (2M rows)..."
"$DB" "$OUT" <<'SQL'
-- Per-plate metadata and QC statistics.
-- In a live HTS facility each plate gets full QC before results are used.
CREATE TABLE hts.plates AS
SELECT
    (range + 1)::INTEGER                                      AS plate_id,
    ('PLT-' || lpad((range + 1)::VARCHAR, 8, '0'))            AS barcode,

    ((range % 100000) + 1)::INTEGER                           AS batch_id,
    ((range % 500) + 1)::SMALLINT                             AS instrument_id,

    -- Plate format
    CASE (range % 3)::INT
        WHEN 0 THEN 384 WHEN 1 THEN 1536 ELSE 96
    END::SMALLINT                                             AS plate_format,

    -- Position in batch
    ((range % 2000) + 1)::SMALLINT                            AS plate_position_in_batch,

    -- Run timestamp
    (TIMESTAMP '2005-01-01' + INTERVAL ((hash(range * 7) % 5256000)::VARCHAR || ' minutes'))
                                                              AS run_timestamp,

    -- Z' factor (plate quality; 1=perfect, 0.5=acceptable, <0=unusable)
    round(-0.2 + (hash(range * 11) % 1200)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                              AS zprime,

    -- Signal-to-background ratio (positive / negative control ratio)
    round(1.5 + (hash(range * 13) % 9850)::DOUBLE / 100.0, 3)::DECIMAL(8,3)
                                                              AS sb_ratio,

    -- Coefficient of variation for positive controls (%)
    round(0.5 + (hash(range * 17) % 2000)::DOUBLE / 100.0, 3)::DECIMAL(6,3)
                                                              AS pos_ctrl_cv_pct,

    -- Coefficient of variation for negative controls (%)
    round(0.5 + (hash(range * 19) % 1500)::DOUBLE / 100.0, 3)::DECIMAL(6,3)
                                                              AS neg_ctrl_cv_pct,

    -- Raw signal stats for the plate
    round(10000.0 + (hash(range * 23) % 90000)::DOUBLE, 1)::DECIMAL(10,1)
                                                              AS signal_mean,
    round(500.0 + (hash(range * 29) % 4500)::DOUBLE, 1)::DECIMAL(10,1)
                                                              AS signal_sd,
    round(50.0 + (hash(range * 31) % 9950)::DOUBLE, 1)::DECIMAL(10,1)
                                                              AS signal_min,
    round(100000.0 - (hash(range * 37) % 10000)::DOUBLE, 1)::DECIMAL(10,1)
                                                              AS signal_max,

    -- QC pass/fail (Z' >= 0.5 = pass)
    ((-0.2 + (hash(range * 11) % 1200)::DOUBLE / 1000.0) >= 0.5)
                                                              AS qc_pass,

    -- Failure reason (NULL if passing)
    CASE WHEN ((-0.2 + (hash(range * 11) % 1200)::DOUBLE / 1000.0) < 0.5)
         THEN CASE (hash(range * 41) % 4)::INT
                  WHEN 0 THEN 'low_zprime'
                  WHEN 1 THEN 'dispensing_error'
                  WHEN 2 THEN 'evaporation'
                  ELSE        'instrument_fault'
              END
         ELSE NULL
    END                                                       AS fail_reason,

    -- Temperature and humidity during run
    round(22.0 + (hash(range * 43) % 600)::DOUBLE / 100.0, 1)::DECIMAL(4,1)
                                                              AS temp_celsius,
    round(40.0 + (hash(range * 47) % 3000)::DOUBLE / 100.0, 1)::DECIMAL(5,1)
                                                              AS humidity_pct,

    -- Operator
    ('OP-' || lpad(((hash(range * 53) % 50) + 1)::VARCHAR, 2, '0'))
                                                              AS operator_id

FROM range(0, 2000000);
SQL
progress "[hts.plates] done (2M rows)"

# ─────────────────────────────────────────────────────────────────────────────
# wells — THE BIG ONE: 1.5B raw well signal readings
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] building wells (${WELLS_N} rows) — this is the big one..."
"$DB" "$OUT" -c "
-- Raw well-level fluorescence / luminescence / FP readings.
-- One row per well per plate. At 1536-well format:
--   1.5B wells / 1536 wells-per-plate = ~976K plates contributing data.
-- Plate format determines well position encoding:
--   384-well:  rows A-P (16), cols 01-24
--   1536-well: rows A-AF (32), cols 01-48
-- Controls: column 1 = positive control (100% inhibition), column 2 = negative (0%)
CREATE TABLE hts.wells AS
SELECT
    (range + 1)::BIGINT                                       AS well_id,

    -- Plate: distribute across 2M plates
    ((range % 2000000) + 1)::INTEGER                          AS plate_id,

    -- Well index within plate (0-1535 for 1536-format, 0-383 for 384-format)
    (range % 1536)::SMALLINT                                  AS well_index,

    -- Well position string (row letter + 2-digit column, 1536-well layout)
    chr(65 + ((range % 1536) / 48)::INT) ||
    lpad(((range % 1536) % 48 + 1)::VARCHAR, 2, '0')          AS well_position,

    -- Compound ID (NULL for control wells; controls = col 01 and 02)
    CASE WHEN ((range % 1536) % 48) < 2 THEN NULL
         ELSE ((range * 7 + hash(range) % 13) % 10000000 + 1)::BIGINT
    END                                                       AS compound_id,

    -- Test concentration (nM)
    CASE WHEN ((range % 1536) % 48) >= 2
         THEN CASE (range % 3)::INT
                  WHEN 0 THEN 10000.0   -- 10 µM
                  WHEN 1 THEN  1000.0   -- 1 µM
                  ELSE         100.0    -- 100 nM
              END
         ELSE NULL
    END::DECIMAL(12,3)                                        AS conc_nm,

    -- Control type
    CASE ((range % 1536) % 48)::INT
        WHEN 0 THEN 'pos_ctrl'   -- column 1: known inhibitor
        WHEN 1 THEN 'neg_ctrl'   -- column 2: DMSO vehicle
        ELSE        NULL
    END                                                       AS control_type,

    -- Raw signal (RFU / RLU / mP depending on instrument)
    -- Controls have tight distributions; samples vary widely
    CASE WHEN ((range % 1536) % 48) = 0  -- positive control: low signal
         THEN round(1000.0 + (hash(range * 7) % 1000)::DOUBLE, 1)
         WHEN ((range % 1536) % 48) = 1  -- negative control: high signal
         THEN round(90000.0 + (hash(range * 7) % 5000)::DOUBLE, 1)
         ELSE -- compound wells: log-normal around negative control
              round(
                  greatest(100.0,
                      90000.0 * pow(10::DOUBLE,
                          -0.5 * (hash(range * 11) % 2000)::DOUBLE / 1000.0 +
                           0.1 * (hash(range * 13) % 1000)::DOUBLE / 1000.0 - 0.05
                      )
                  ), 1
              )
    END::DECIMAL(12,1)                                        AS raw_signal,

    -- Normalized activity % (0=no effect, 100=full inhibition)
    -- = (neg_ctrl - signal) / (neg_ctrl - pos_ctrl) * 100
    CASE WHEN ((range % 1536) % 48) = 0 THEN 100.0
         WHEN ((range % 1536) % 48) = 1 THEN   0.0
         ELSE round(
             greatest(-30.0, least(130.0,
                 (1.0 - pow(10::DOUBLE,
                     -0.5 * (hash(range * 11) % 2000)::DOUBLE / 1000.0 +
                      0.1 * (hash(range * 13) % 1000)::DOUBLE / 1000.0 - 0.05
                 )) * 100.0 +
                 (hash(range * 17) % 1000)::DOUBLE / 100.0 - 5.0  -- noise
             )), 3
         )
    END::DECIMAL(7,3)                                         AS activity_pct,

    -- Z-score relative to plate controls
    round(
        CASE WHEN ((range % 1536) % 48) = 0 THEN  3.5 + (hash(range * 19) % 200)::DOUBLE / 100.0
             WHEN ((range % 1536) % 48) = 1 THEN -0.1 + (hash(range * 19) % 200)::DOUBLE / 100.0 - 1.0
             ELSE (hash(range * 23) % 9000)::DOUBLE / 1000.0 - 4.5
        END, 4
    )::DECIMAL(7,4)                                           AS zscore,

    -- Flag: 0=OK, 1=suspect (high CV or near-threshold), 2=failed/exclude
    CASE (hash(range * 29) % 100)::INT
        WHEN 0 THEN 2::TINYINT   -- 1% outright failed
        WHEN 1 THEN 1::TINYINT   -- 1% suspect
        WHEN 2 THEN 1::TINYINT
        ELSE        0::TINYINT   -- 98% clean
    END                                                       AS well_flag,

    -- DMSO concentration (% v/v) — should be ≤0.1% for most assays
    round(0.01 + (hash(range * 31) % 20)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                              AS dmso_pct,

    -- Dispensing volume (nL, acoustic dispensing e.g. Echo)
    ((hash(range * 37) % 100) * 2 + 100)::SMALLINT            AS dispense_vol_nl

FROM range(0, ${WELLS_N});
"
progress "[hts.wells] done (${WELLS_N} rows)"

# ─────────────────────────────────────────────────────────────────────────────
# dr_points — 500M dose-response curve measurements
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] building dose-response points (${DR_N} rows)..."
"$DB" "$OUT" -c "
-- 10-point dose-response curves, 2 replicates per point.
-- Each compound-assay pair that progressed past primary screening gets a DR curve.
-- 500M rows = ~25M compound-assay DR experiments × 10 conc × 2 reps.
CREATE TABLE hts.dr_points AS
SELECT
    (range + 1)::BIGINT                                       AS dp_id,

    -- Curve: group rows into 20-row blocks (10 conc × 2 rep)
    (range / 20 + 1)::BIGINT                                  AS curve_id,

    -- Compound (25M unique compounds across confirmed hits)
    ((range / 20) % 10000000 + 1)::BIGINT                     AS compound_id,

    -- Assay (500K assays, DR phase only ~50K)
    ((range / 20) % 50000 + 1)::INTEGER                       AS assay_id,

    -- Concentration point (1-10, half-log spacing: 0.001 µM to 100 µM)
    ((range % 20) / 2 + 1)::TINYINT                           AS conc_point,

    -- Replicate (1 or 2)
    ((range % 2) + 1)::TINYINT                                AS replicate,

    -- Actual concentration (µM): 0.001, 0.003, 0.01, 0.03, 0.1, 0.3, 1, 3, 10, 30
    CASE ((range % 20) / 2)::INT
        WHEN 0 THEN   0.001
        WHEN 1 THEN   0.003
        WHEN 2 THEN   0.010
        WHEN 3 THEN   0.030
        WHEN 4 THEN   0.100
        WHEN 5 THEN   0.300
        WHEN 6 THEN   1.000
        WHEN 7 THEN   3.000
        WHEN 8 THEN  10.000
        ELSE          30.000
    END::DECIMAL(10,4)                                        AS conc_um,

    -- Log10 concentration (for curve fitting)
    CASE ((range % 20) / 2)::INT
        WHEN 0 THEN -3.0 WHEN 1 THEN -2.5 WHEN 2 THEN -2.0
        WHEN 3 THEN -1.5 WHEN 4 THEN -1.0 WHEN 5 THEN -0.5
        WHEN 6 THEN  0.0 WHEN 7 THEN  0.5 WHEN 8 THEN  1.0
        ELSE          1.5
    END::DECIMAL(5,2)                                         AS log10_conc,

    -- Response (% inhibition or % activity) — 4-parameter logistic model
    -- y = bottom + (top-bottom) / (1 + (EC50/x)^hill)
    -- Using curve_id to vary EC50 (some potent, some weak, some inactive)
    round(
        greatest(-10.0, least(110.0,
            5.0 +  -- bottom
            (95.0) / (  -- top - bottom
                1.0 + pow(
                    pow(10::DOUBLE, -0.5 + (hash(range / 20 * 7) % 3000)::DOUBLE / 1000.0) /
                    CASE ((range % 20) / 2)::INT
                        WHEN 0 THEN 0.001 WHEN 1 THEN 0.003 WHEN 2 THEN 0.010
                        WHEN 3 THEN 0.030 WHEN 4 THEN 0.100 WHEN 5 THEN 0.300
                        WHEN 6 THEN 1.000 WHEN 7 THEN 3.000 WHEN 8 THEN 10.000
                        ELSE 30.000
                    END,
                    1.5 + (hash(range / 20 * 11) % 200)::DOUBLE / 100.0  -- hill slope
                )
            ) +
            (hash(range * 13) % 1000)::DOUBLE / 100.0 - 5.0  -- noise ±5%
        )), 4
    )::DECIMAL(8,4)                                           AS response_pct,

    -- Raw signal (arbitrary units, pre-normalization)
    round(
        1000.0 + (hash(range * 17) % 99000)::DOUBLE +
        (hash(range * 19) % 1000)::DOUBLE - 500.0,
        1
    )::DECIMAL(12,1)                                          AS raw_signal,

    -- Outlier flag (5% of replicates are flagged as suspect)
    ((hash(range * 23) % 20) = 0)                             AS is_outlier,

    -- Normalized signal (plate-level z-score)
    round((hash(range * 29) % 9000)::DOUBLE / 1000.0 - 4.5, 4)::DECIMAL(7,4)
                                                              AS plate_zscore

FROM range(0, ${DR_N});
"
progress "[hts.dr_points] done (${DR_N} rows)"

# ─────────────────────────────────────────────────────────────────────────────
# compound_funnel — 100M HTS funnel tracking rows
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] building compound funnel (${FUNNEL_N} rows)..."
"$DB" "$OUT" -c "
-- Hit progression tracking: every compound's journey through the HTS triage funnel.
-- One row per compound per assay per stage. Funnel stages:
--   primary → confirmation → dose_response → selectivity → ADME → lead
CREATE TABLE hts.compound_funnel AS
SELECT
    (range + 1)::BIGINT                                       AS record_id,
    ((range % 10000000) + 1)::BIGINT                          AS compound_id,
    ((range % 50000) + 1)::INTEGER                            AS assay_id,

    CASE (range % 6)::INT
        WHEN 0 THEN 'primary'
        WHEN 1 THEN 'confirmation'
        WHEN 2 THEN 'dose_response'
        WHEN 3 THEN 'selectivity'
        WHEN 4 THEN 'ADME_panel'
        ELSE        'lead_optimization'
    END                                                       AS stage,

    CASE (hash(range * 7) % 10)::INT
        WHEN 0 THEN 'active'
        WHEN 1 THEN 'active'
        WHEN 2 THEN 'inconclusive'
        ELSE        'inactive'
    END                                                       AS outcome,

    -- Activity value at this stage (IC50 in nM for DR stage, % inh for primary)
    CASE WHEN (range % 6) IN (2, 3)  -- DR or selectivity
         THEN round(pow(10::DOUBLE, 0.0 + (hash(range * 11) % 4000)::DOUBLE / 1000.0), 3)
         ELSE NULL
    END::DECIMAL(14,4)                                        AS activity_value,

    CASE WHEN (range % 6) IN (2, 3) THEN 'nM' ELSE NULL END  AS activity_units,

    -- Selectivity index vs closest counter-screen (higher is better)
    CASE WHEN (range % 6) = 3
         THEN round(1.0 + (hash(range * 13) % 9900)::DOUBLE / 100.0, 2)
         ELSE NULL
    END::DECIMAL(8,2)                                         AS selectivity_ratio,

    -- Compound promoted to next stage?
    ((hash(range * 17) % 4) = 0)                              AS promoted,

    -- Reason not promoted (if applicable)
    CASE WHEN ((hash(range * 17) % 4) != 0)
         THEN CASE (hash(range * 19) % 6)::INT
                  WHEN 0 THEN 'inactive'
                  WHEN 1 THEN 'cytotoxic'
                  WHEN 2 THEN 'poor_selectivity'
                  WHEN 3 THEN 'ADME_failure'
                  WHEN 4 THEN 'aggregator'
                  ELSE        'redox_active'
              END
         ELSE NULL
    END                                                       AS deprioritization_reason,

    (DATE '2005-01-01' + ((hash(range * 23) % 7671)::INTEGER)) AS stage_date,
    ((range % 100000) + 1)::INTEGER                           AS batch_id

FROM range(0, ${FUNNEL_N});
"
progress "[hts.compound_funnel] done (${FUNNEL_N} rows)"

# ─────────────────────────────────────────────────────────────────────────────
# hit_calls — 50M final hit classification decisions
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] building hit calls (${HITS_N} rows)..."
"$DB" "$OUT" -c "
-- Final algorithmic hit call per compound per assay.
-- Aggregates replicate data and applies cutoffs to give a definitive classification.
CREATE TABLE hts.hit_calls AS
SELECT
    (range + 1)::BIGINT                                       AS call_id,
    ((range % 10000000) + 1)::BIGINT                          AS compound_id,
    ((range % 500000) + 1)::INTEGER                           AS assay_id,
    ((range % 100000) + 1)::INTEGER                           AS batch_id,

    -- Mean activity across replicates
    round((hash(range * 7) % 10001)::DOUBLE / 100.0, 3)::DECIMAL(7,3)
                                                              AS mean_activity_pct,

    -- Std dev of replicates
    round((hash(range * 11) % 2000)::DOUBLE / 100.0, 3)::DECIMAL(6,3)
                                                              AS stdev_activity_pct,

    -- Number of replicates included
    ((hash(range * 13) % 3) + 2)::TINYINT                    AS n_replicates,

    -- Cutoff used (typically mean_neg_ctrl + 3*sd_neg_ctrl)
    round(20.0 + (hash(range * 17) % 3000)::DOUBLE / 100.0, 3)::DECIMAL(7,3)
                                                              AS activity_cutoff_pct,

    -- Algorithmic hit call
    CASE
        WHEN (hash(range * 7) % 10001)::DOUBLE / 100.0 >= 50.0 THEN 'confirmed_active'
        WHEN (hash(range * 7) % 10001)::DOUBLE / 100.0 >= 30.0 THEN 'weak_active'
        WHEN (hash(range * 7) % 10001)::DOUBLE / 100.0 >= 20.0 THEN 'borderline'
        ELSE 'inactive'
    END                                                       AS hit_call,

    -- Interference flags (assay interference mechanisms)
    ((hash(range * 19) % 50) = 0)                             AS is_fluorescent,
    ((hash(range * 23) % 100) = 0)                            AS is_aggregator,
    ((hash(range * 29) % 200) = 0)                            AS is_redox_active,
    ((hash(range * 31) % 150) = 0)                            AS is_cytotoxic,

    -- Confidence tier (A=high, B=medium, C=low)
    CASE (hash(range * 37) % 3)::INT
        WHEN 0 THEN 'A' WHEN 1 THEN 'B' ELSE 'C'
    END                                                       AS confidence_tier,

    (DATE '2005-01-01' + ((hash(range * 41) % 7671)::INTEGER)) AS call_date

FROM range(0, ${HITS_N});
"
progress "[hts.hit_calls] done (${HITS_N} rows)"

# ─────────────────────────────────────────────────────────────────────────────
# qc_daily — 200K daily instrument QC log entries
# ─────────────────────────────────────────────────────────────────────────────
progress "[hts] building daily QC log (200K rows)..."
"$DB" "$OUT" <<'SQL'
-- Daily instrument qualification records (liquid handlers + plate readers)
CREATE TABLE hts.qc_daily AS
SELECT
    (range + 1)::INTEGER                                       AS qc_id,
    ((range % 500) + 1)::SMALLINT                              AS instrument_id,
    (DATE '2005-01-01' + (range / 500)::INTEGER)               AS qc_date,
    CASE (range % 4)::INT
        WHEN 0 THEN 'daily_qualification'
        WHEN 1 THEN 'calibration'
        WHEN 2 THEN 'maintenance'
        ELSE        'reference_plate'
    END                                                        AS qc_type,
    -- Control plate Z'
    round(0.3 + (hash(range * 7) % 700)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                               AS zprime,
    round(2.0 + (hash(range * 11) % 9800)::DOUBLE / 100.0, 2)::DECIMAL(7,2)
                                                               AS sb_ratio,
    round(0.1 + (hash(range * 13) % 1500)::DOUBLE / 100.0, 3)::DECIMAL(6,3)
                                                               AS cv_pct,
    ((-0.2 + (hash(range * 7) % 1200)::DOUBLE / 1000.0) >= 0.5)
                                                               AS qc_pass,
    ('OP-' || lpad(((hash(range * 17) % 50) + 1)::VARCHAR, 2, '0'))
                                                               AS operator_id,
    (range % 10 = 0)                                           AS requires_service
FROM range(0, 200000);
SQL
progress "[hts.qc_daily] done (200K rows)"

# ─────────────────────────────────────────────────────────────────────────────
# Build manifest + summary
# ─────────────────────────────────────────────────────────────────────────────
"$DB" "$OUT" <<'SQL'
CREATE TABLE hts.build_manifest (
    key VARCHAR NOT NULL, value VARCHAR, built_at TIMESTAMP DEFAULT now()
);
INSERT INTO hts.build_manifest (key, value) VALUES
    ('database_name', 'hts-pipeline'),
    ('domain',        'high-throughput screening / drug discovery pipeline'),
    ('purpose',       'hyphasync stress-test: velocity/throughput-oriented bulk data'),
    ('schema',        'hts'),
    ('generator',     'build-hts.sh');
SQL

END=$(date +%s)
echo ""
echo "==> Build complete in $((END - START))s"
echo ""
echo "==> Table summary:"
"$DB" "$OUT" -c "
SELECT
    table_name,
    estimated_size::BIGINT AS approx_rows
FROM duckdb_tables()
WHERE database_name = current_database() AND schema_name = 'hts'
ORDER BY approx_rows DESC;
" 2>/dev/null || true
echo ""
echo "==> File size: $(ls -lh "$OUT" | awk '{print $5}')"
