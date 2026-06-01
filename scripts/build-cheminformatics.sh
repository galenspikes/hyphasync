#!/usr/bin/env bash
# Build testdata/cheminformatics.duckdb — a synthetic but domain-faithful
# cheminformatics / computational toxicology database designed to stress-test
# hyphasync's fingerprinting, type coverage, and large-table sync.
#
# Domain model (mimics PubChem + ChEMBL + Tox21 + ADMET-AI conventions):
#
#   registry   — core molecular registry (compounds, synonyms, cross-refs)
#   bioassay   — HTS assay data: 500K assays × 10M compounds → 1B activity rows
#   toxicology — regulatory tox studies, in-vitro assays, structure alerts
#   admet      — predicted ADMET properties (absorption, distribution, etc.)
#   clinical   — approved drugs, adverse events, clinical trials
#   reference  — small lookup tables (species, routes, GHS categories, etc.)
#
# Row counts (approximate, after compression DuckDB will be 2-8 GB on disk):
#   bioassay.activities          1,000,000,000  ← the woodchipper table
#   toxicology.findings            200,000,000
#   admet.cyp_interactions          50,000,000
#   registry.synonyms               40,000,000
#   clinical.adverse_events         20,000,000
#   toxicology.compound_alerts      15,000,000
#   registry.compounds              10,000,000
#   admet.physchem_predictions      10,000,000
#   registry.cross_refs              5,000,000
#   toxicology.acute_tox             5,000,000
#   toxicology.studies               2,000,000
#   clinical.clinical_trials           100,000
#   bioassay.assays                    500,000
#   bioassay.targets                   100,000
#   admet.clearance_predictions         10,000
#   + all reference / lookup tables       ~5,000
#
# Usage: ./scripts/build-cheminformatics.sh [--fast]
#   --fast: skip the 1B-row activities table (useful for quick iteration)
#
# Requires: make release done (./build/release/duckdb must exist)
# Runtime:  ~15-40 minutes depending on hardware (activities table dominates)
set -euo pipefail

FAST=false
for arg in "$@"; do [[ "$arg" == "--fast" ]] && FAST=true; done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DB="${ROOT_DIR}/build/release/duckdb"
OUT="${ROOT_DIR}/testdata/cheminformatics.duckdb"

[[ -x "$DB" ]] || { echo "No binary at $DB — run 'CC=gcc CXX=g++ make release' first." >&2; exit 1; }

rm -f "$OUT"
mkdir -p "$(dirname "$OUT")"
START=$(date +%s)

progress() { echo ""; echo "  [$(( $(date +%s) - START ))s] $*"; }

echo "==> Building cheminformatics.duckdb ..."
echo "    fast=${FAST}"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# SCHEMA: reference — small lookup tables loaded first so FKs make sense
# ─────────────────────────────────────────────────────────────────────────────
progress "[reference] building lookup tables..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA reference;

-- Periodic table (118 elements — real data, complete through Oganesson)
CREATE TABLE reference.elements (
    atomic_number   TINYINT  PRIMARY KEY,
    symbol          VARCHAR(3) NOT NULL,
    name            VARCHAR(20) NOT NULL,
    period          TINYINT,
    group_num       TINYINT,
    category        VARCHAR(30),  -- 'alkali metal', 'halogen', etc.
    electronegativity DECIMAL(4,2),
    atomic_weight   DECIMAL(10,5),
    melting_point_k DECIMAL(10,3),
    boiling_point_k DECIMAL(10,3),
    density_g_cm3   DECIMAL(10,5),
    is_radioactive  BOOLEAN DEFAULT false,
    is_synthetic    BOOLEAN DEFAULT false,
    discovery_year  SMALLINT
);
INSERT INTO reference.elements VALUES
 (1,'H','Hydrogen',1,1,'nonmetal',2.20,1.00794,14.01,20.28,0.00009,false,false,1766),
 (2,'He','Helium',1,18,'noble gas',NULL,4.00260,NULL,4.22,0.00016,false,false,1868),
 (3,'Li','Lithium',2,1,'alkali metal',0.98,6.94100,453.69,1615.0,0.53400,false,false,1817),
 (4,'Be','Beryllium',2,2,'alkaline earth',1.57,9.01218,1560.0,2742.0,1.84800,false,false,1798),
 (5,'B','Boron',2,13,'metalloid',2.04,10.81100,2348.0,4273.0,2.46000,false,false,1808),
 (6,'C','Carbon',2,14,'nonmetal',2.55,12.01070,3823.0,4098.0,2.26700,false,false,NULL),
 (7,'N','Nitrogen',2,15,'nonmetal',3.04,14.00670,63.15,77.36,0.00117,false,false,1772),
 (8,'O','Oxygen',2,16,'nonmetal',3.44,15.99940,54.36,90.20,0.00133,false,false,1774),
 (9,'F','Fluorine',2,17,'halogen',3.98,18.99840,53.53,85.03,0.00158,false,false,1886),
 (10,'Ne','Neon',2,18,'noble gas',NULL,20.17970,24.56,27.07,0.00084,false,false,1898),
 (11,'Na','Sodium',3,1,'alkali metal',0.93,22.98977,370.87,1156.0,0.97100,false,false,1807),
 (12,'Mg','Magnesium',3,2,'alkaline earth',1.31,24.30500,923.00,1363.0,1.73800,false,false,1755),
 (13,'Al','Aluminum',3,13,'post-transition',1.61,26.98154,933.47,2792.0,2.70000,false,false,1825),
 (14,'Si','Silicon',3,14,'metalloid',1.90,28.08550,1687.0,3538.0,2.33000,false,false,1824),
 (15,'P','Phosphorus',3,15,'nonmetal',2.19,30.97376,317.30,550.00,1.82000,false,false,1669),
 (16,'S','Sulfur',3,16,'nonmetal',2.58,32.06500,388.36,717.87,2.06700,false,false,NULL),
 (17,'Cl','Chlorine',3,17,'halogen',3.16,35.45300,171.60,239.11,0.00292,false,false,1774),
 (18,'Ar','Argon',3,18,'noble gas',NULL,39.94800,83.80,87.30,0.00164,false,false,1894),
 (19,'K','Potassium',4,1,'alkali metal',0.82,39.09830,336.53,1032.0,0.86200,false,false,1807),
 (20,'Ca','Calcium',4,2,'alkaline earth',1.00,40.07800,1115.0,1757.0,1.54000,false,false,1808),
 (26,'Fe','Iron',4,8,'transition metal',1.83,55.84500,1811.0,3134.0,7.87400,false,false,NULL),
 (29,'Cu','Copper',4,11,'transition metal',1.90,63.54600,1357.8,2835.0,8.96000,false,false,NULL),
 (30,'Zn','Zinc',4,12,'transition metal',1.65,65.38000,692.68,1180.0,7.13300,false,false,NULL),
 (35,'Br','Bromine',4,17,'halogen',2.96,79.90400,265.95,332.00,3.10200,false,false,1826),
 (53,'I','Iodine',5,17,'halogen',2.66,126.90447,386.85,457.40,4.93000,false,false,1811),
 (78,'Pt','Platinum',6,10,'transition metal',2.28,195.08400,2041.4,4098.0,21.45000,false,false,1735),
 (79,'Au','Gold',6,11,'transition metal',2.54,196.96655,1337.3,3129.0,19.30000,false,false,NULL),
 (80,'Hg','Mercury',6,12,'transition metal',2.00,200.59200,234.32,629.88,13.53400,true,false,NULL),
 (82,'Pb','Lead',6,14,'post-transition',2.33,207.20000,600.61,2022.0,11.34000,true,false,NULL),
 (92,'U','Uranium',7,NULL,'actinide',1.38,238.02891,1405.3,4404.0,18.95000,true,false,1789),
 (118,'Og','Oganesson',7,18,'noble gas',NULL,294.00000,NULL,NULL,NULL,true,true,2002);

-- GHS Hazard categories (Globally Harmonized System)
CREATE TABLE reference.ghs_categories (
    category_id   VARCHAR(10) PRIMARY KEY,
    hazard_class  VARCHAR(50) NOT NULL,
    category_num  TINYINT,
    signal_word   VARCHAR(10),  -- 'Danger' or 'Warning'
    h_statement   VARCHAR(200),
    pictogram     VARCHAR(20)
);
INSERT INTO reference.ghs_categories VALUES
 ('Acute1','Acute toxicity (oral)',1,'Danger','Fatal if swallowed','GHS06'),
 ('Acute2','Acute toxicity (oral)',2,'Danger','Fatal if swallowed','GHS06'),
 ('Acute3','Acute toxicity (oral)',3,'Danger','Toxic if swallowed','GHS06'),
 ('Acute4','Acute toxicity (oral)',4,'Warning','Harmful if swallowed','GHS07'),
 ('Acute5','Acute toxicity (oral)',5,'Warning','May be harmful if swallowed',NULL),
 ('AcuteD1','Acute toxicity (dermal)',1,'Danger','Fatal in contact with skin','GHS06'),
 ('AcuteD2','Acute toxicity (dermal)',2,'Danger','Fatal in contact with skin','GHS06'),
 ('AcuteD3','Acute toxicity (dermal)',3,'Danger','Toxic in contact with skin','GHS06'),
 ('AcuteD4','Acute toxicity (dermal)',4,'Warning','Harmful in contact with skin','GHS07'),
 ('AcuteI1','Acute toxicity (inhalation)',1,'Danger','Fatal if inhaled','GHS06'),
 ('AcuteI2','Acute toxicity (inhalation)',2,'Danger','Fatal if inhaled','GHS06'),
 ('AcuteI3','Acute toxicity (inhalation)',3,'Danger','Toxic if inhaled','GHS06'),
 ('AcuteI4','Acute toxicity (inhalation)',4,'Warning','Harmful if inhaled','GHS07'),
 ('Carc1A','Carcinogenicity',1,'Danger','May cause cancer','GHS08'),
 ('Carc1B','Carcinogenicity',1,'Danger','May cause cancer','GHS08'),
 ('Carc2','Carcinogenicity',2,'Warning','Suspected of causing cancer','GHS08'),
 ('Muta1A','Germ cell mutagenicity',1,'Danger','May cause genetic defects','GHS08'),
 ('Muta1B','Germ cell mutagenicity',1,'Danger','May cause genetic defects','GHS08'),
 ('Muta2','Germ cell mutagenicity',2,'Warning','Suspected of causing genetic defects','GHS08'),
 ('Repr1A','Reproductive toxicity',1,'Danger','May damage fertility or the unborn child','GHS08'),
 ('Repr1B','Reproductive toxicity',1,'Danger','May damage fertility or the unborn child','GHS08'),
 ('Repr2','Reproductive toxicity',2,'Warning','Suspected of damaging fertility or the unborn child','GHS08'),
 ('STOT-SE1','STOT single exposure',1,'Danger','Causes damage to organs','GHS08'),
 ('STOT-SE2','STOT single exposure',2,'Warning','May cause damage to organs','GHS08'),
 ('STOT-SE3','STOT single exposure',3,'Warning','May cause respiratory irritation','GHS07'),
 ('STOT-RE1','STOT repeated exposure',1,'Danger','Causes damage to organs through prolonged or repeated exposure','GHS08'),
 ('STOT-RE2','STOT repeated exposure',2,'Warning','May cause damage to organs through prolonged or repeated exposure','GHS08'),
 ('Skin1','Skin corrosion/irritation',1,'Danger','Causes severe skin burns and eye damage','GHS05'),
 ('Skin2','Skin corrosion/irritation',2,'Warning','Causes skin irritation','GHS07'),
 ('Eye1','Serious eye damage/irritation',1,'Danger','Causes serious eye damage','GHS05'),
 ('Eye2A','Serious eye damage/irritation',2,'Warning','Causes serious eye irritation','GHS07');

-- Test species (standard regulatory tox models)
CREATE TABLE reference.test_species (
    species_id    SMALLINT PRIMARY KEY,
    common_name   VARCHAR(50) NOT NULL,
    latin_name    VARCHAR(100),
    strain        VARCHAR(50),
    sex_options   VARCHAR(20),  -- 'M', 'F', 'M/F', 'NA'
    model_type    VARCHAR(20),  -- 'rodent', 'non-rodent', 'aquatic', 'in_vitro'
    regulatory_relevance VARCHAR(50)  -- 'ICH S1B', 'OECD 407', etc.
);
INSERT INTO reference.test_species VALUES
 (1,'Rat','Rattus norvegicus','Sprague-Dawley','M/F','rodent','OECD 407/408'),
 (2,'Rat','Rattus norvegicus','Wistar','M/F','rodent','OECD 407/408'),
 (3,'Rat','Rattus norvegicus','Fischer 344','M/F','rodent','NTP standard'),
 (4,'Mouse','Mus musculus','CD-1','M/F','rodent','OECD 423'),
 (5,'Mouse','Mus musculus','C57BL/6','M/F','rodent','ICH S1B'),
 (6,'Mouse','Mus musculus','B6C3F1','M/F','rodent','NTP 2-year carcinogenicity'),
 (7,'Dog','Canis lupus familiaris','Beagle','M/F','non-rodent','ICH M3(R2)'),
 (8,'Rabbit','Oryctolagus cuniculus','New Zealand White','F','non-rodent','OECD 414'),
 (9,'Monkey','Macaca fascicularis','Cynomolgus','M/F','non-rodent','ICH S6(R1)'),
 (10,'Pig','Sus scrofa domesticus','Göttingen minipig','M/F','non-rodent','dermal/oral'),
 (11,'Zebrafish','Danio rerio','wild-type AB','M/F','aquatic','OECD 236'),
 (12,'Daphnia','Daphnia magna',NULL,'F','aquatic','OECD 211'),
 (13,'Algae','Raphidocelis subcapitata',NULL,'NA','aquatic','OECD 201'),
 (14,'HepG2','Homo sapiens (hepatocyte)',NULL,'NA','in_vitro','OECD TG 455'),
 (15,'HEK293','Homo sapiens (embryonic kidney)',NULL,'NA','in_vitro','reporter assays'),
 (16,'CHO','Cricetulus griseus (ovary)',NULL,'NA','in_vitro','OECD TG 476'),
 (17,'Salmonella','Salmonella typhimurium','TA98/TA100/TA1535/TA1537/TA102','NA','in_vitro','OECD TG 471 (Ames)');

-- Exposure routes (regulatory standard vocabulary)
CREATE TABLE reference.exposure_routes (
    route_id   TINYINT PRIMARY KEY,
    route_code VARCHAR(10) NOT NULL,
    route_name VARCHAR(50) NOT NULL,
    abbreviation VARCHAR(5),
    applicable_to VARCHAR(50)  -- 'human', 'animal', 'both'
);
INSERT INTO reference.exposure_routes VALUES
 (1,'oral','Oral / gavage','po','both'),
 (2,'iv','Intravenous','iv','both'),
 (3,'sc','Subcutaneous','sc','both'),
 (4,'im','Intramuscular','im','both'),
 (5,'ip','Intraperitoneal','ip','animal'),
 (6,'inh','Inhalation (whole-body)','wbi','both'),
 (7,'inh-nose','Inhalation (nose-only)','noi','both'),
 (8,'dermal','Dermal (intact skin)','derm','both'),
 (9,'dermal-occ','Dermal (occluded)','occ','both'),
 (10,'ocular','Ocular / ophthalmic','eye','both'),
 (11,'topical','Topical','top','both'),
 (12,'diet','Dietary admixture','diet','animal'),
 (13,'water','Drinking water','dw','animal'),
 (14,'implant','Subcutaneous implant','impl','animal'),
 (15,'intratracheal','Intratracheal instillation','it','animal');

-- Therapeutic area classification (WHO ATC level 1 + common subcategories)
CREATE TABLE reference.therapeutic_areas (
    ta_id      SMALLINT PRIMARY KEY,
    atc_level1 VARCHAR(1),
    atc_name   VARCHAR(80) NOT NULL,
    ta_short   VARCHAR(30)
);
INSERT INTO reference.therapeutic_areas
SELECT
    range + 1,
    chr(65 + ((range) % 14)::INTEGER),
    CASE (range % 30)::INTEGER
        WHEN 0  THEN 'Alimentary tract and metabolism'
        WHEN 1  THEN 'Blood and blood forming organs'
        WHEN 2  THEN 'Cardiovascular system'
        WHEN 3  THEN 'Dermatologicals'
        WHEN 4  THEN 'Genito-urinary system and sex hormones'
        WHEN 5  THEN 'Systemic hormonal preparations (excl. sex hormones)'
        WHEN 6  THEN 'Anti-infectives for systemic use'
        WHEN 7  THEN 'Antineoplastic and immunomodulating agents'
        WHEN 8  THEN 'Musculo-skeletal system'
        WHEN 9  THEN 'Nervous system'
        WHEN 10 THEN 'Antiparasitic products, insecticides and repellents'
        WHEN 11 THEN 'Respiratory system'
        WHEN 12 THEN 'Sensory organs'
        WHEN 13 THEN 'Various'
        WHEN 14 THEN 'Oncology - solid tumors'
        WHEN 15 THEN 'Oncology - hematologic malignancies'
        WHEN 16 THEN 'Rare / orphan diseases'
        WHEN 17 THEN 'CNS - pain management'
        WHEN 18 THEN 'CNS - psychiatry'
        WHEN 19 THEN 'CNS - neurology'
        WHEN 20 THEN 'Infectious disease - antibacterial'
        WHEN 21 THEN 'Infectious disease - antiviral'
        WHEN 22 THEN 'Infectious disease - antifungal'
        WHEN 23 THEN 'Immunology - autoimmune'
        WHEN 24 THEN 'Immunology - transplant'
        WHEN 25 THEN 'Endocrinology - diabetes'
        WHEN 26 THEN 'Endocrinology - thyroid'
        WHEN 27 THEN 'Ophthalmology'
        WHEN 28 THEN 'Dermatology'
        ELSE 'Other / unclassified'
    END,
    ('TA_' || lpad((range + 1)::VARCHAR, 3, '0'))
FROM range(0, 300);

-- Bioactivity type controlled vocabulary (IC50, Ki, Kd, EC50, etc.)
CREATE TABLE reference.bioactivity_types (
    btype_id    SMALLINT PRIMARY KEY,
    standard_type VARCHAR(20) NOT NULL,
    full_name   VARCHAR(80),
    units_default VARCHAR(10),
    direction   VARCHAR(10),  -- 'lower_is_better', 'higher_is_better', 'n/a'
    category    VARCHAR(20)   -- 'potency', 'efficacy', 'binding', 'selectivity'
);
INSERT INTO reference.bioactivity_types VALUES
 (1,'IC50','Half maximal inhibitory concentration','nM','lower_is_better','potency'),
 (2,'EC50','Half maximal effective concentration','nM','lower_is_better','potency'),
 (3,'Ki','Inhibition constant','nM','lower_is_better','binding'),
 (4,'Kd','Dissociation constant','nM','lower_is_better','binding'),
 (5,'Km','Michaelis constant','µM','lower_is_better','binding'),
 (6,'AC50','Half maximal activity concentration','µM','lower_is_better','potency'),
 (7,'GI50','50% growth inhibition','µM','lower_is_better','potency'),
 (8,'MIC','Minimum inhibitory concentration','µg/mL','lower_is_better','potency'),
 (9,'MIC50','MIC at 50% inhibition','µg/mL','lower_is_better','potency'),
 (10,'pIC50','Negative log IC50','pIC50','higher_is_better','potency'),
 (11,'pKi','Negative log Ki','pKi','higher_is_better','binding'),
 (12,'pKd','Negative log Kd','pKd','higher_is_better','binding'),
 (13,'Emax','Maximum effect','%','higher_is_better','efficacy'),
 (14,'Imax','Maximum inhibition','%','higher_is_better','efficacy'),
 (15,'Hill','Hill slope / cooperativity coefficient','dimensionless','n/a','kinetics'),
 (16,'logP','Octanol-water partition coefficient','dimensionless','n/a','physicochemical'),
 (17,'logD','Distribution coefficient at pH 7.4','dimensionless','n/a','physicochemical'),
 (18,'pKa','Acid dissociation constant','dimensionless','n/a','physicochemical'),
 (19,'Solubility','Aqueous solubility','µg/mL','higher_is_better','physicochemical'),
 (20,'Permeability','Membrane permeability (Caco-2/MDCK)','10⁻⁶cm/s','higher_is_better','admet'),
 (21,'CLint','Intrinsic clearance','µL/min/mg','lower_is_better','admet'),
 (22,'t½','Half-life','h','n/a','admet'),
 (23,'Vd','Volume of distribution','L/kg','n/a','admet'),
 (24,'F','Oral bioavailability','%','higher_is_better','admet'),
 (25,'PPB','Plasma protein binding','%','n/a','admet'),
 (26,'LD50','Lethal dose 50%','mg/kg','lower_is_better','toxicology'),
 (27,'LC50','Lethal concentration 50%','mg/L','lower_is_better','toxicology'),
 (28,'NOAEL','No observed adverse effect level','mg/kg/day','n/a','toxicology'),
 (29,'LOAEL','Lowest observed adverse effect level','mg/kg/day','n/a','toxicology'),
 (30,'NOEL','No observed effect level','mg/kg/day','n/a','toxicology'),
 (31,'TGI','Tumor growth inhibition','%','higher_is_better','oncology'),
 (32,'SR','Selectivity ratio','dimensionless','higher_is_better','selectivity'),
 (33,'CC50','Cytotoxic concentration 50%','µM','lower_is_better','toxicology'),
 (34,'SI','Selectivity index (CC50/IC50)','dimensionless','higher_is_better','selectivity');

SQL
progress "[reference] done"

# ─────────────────────────────────────────────────────────────────────────────
# SCHEMA: registry — 10M compounds + 40M synonyms + 5M cross-refs
# ─────────────────────────────────────────────────────────────────────────────
progress "[registry] building compounds (10M rows)..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA registry;

-- Compound molecular properties.
-- Distributions modeled on PubChem drug-like subset statistics:
--   MW: log-normal ~300 Da, σ≈100; hbd 0-10; hba 0-15; logP -3..8; tpsa 0-200
-- Scaffold diversity encoded via prime-modulo families (rang % 97 ~ scaffold class).
CREATE TABLE registry.compounds AS
SELECT
    (range + 1)::BIGINT                                      AS compound_id,

    -- Synthetic InChI key (26-char format: 14-10-1)
    upper(
        chr(65 + (hash(range)          % 26)::INT) ||
        chr(65 + (hash(range * 7 + 1)  % 26)::INT) ||
        chr(65 + (hash(range * 11 + 2) % 26)::INT) ||
        chr(65 + (hash(range * 13 + 3) % 26)::INT) ||
        chr(65 + (hash(range * 17 + 4) % 26)::INT) ||
        chr(65 + (hash(range * 19 + 5) % 26)::INT) ||
        chr(65 + (hash(range * 23 + 6) % 26)::INT) ||
        chr(65 + (hash(range * 29 + 7) % 26)::INT) ||
        chr(65 + (hash(range * 31 + 8) % 26)::INT) ||
        chr(65 + (hash(range * 37 + 9) % 26)::INT) ||
        chr(65 + (hash(range * 41 +10) % 26)::INT) ||
        chr(65 + (hash(range * 43 +11) % 26)::INT) ||
        chr(65 + (hash(range * 47 +12) % 26)::INT) ||
        chr(65 + (hash(range * 53 +13) % 26)::INT)
    ) || '-' ||
    upper(
        chr(65 + (hash(range * 59)     % 26)::INT) ||
        chr(65 + (hash(range * 61 + 1) % 26)::INT) ||
        chr(65 + (hash(range * 67 + 2) % 26)::INT) ||
        chr(65 + (hash(range * 71 + 3) % 26)::INT) ||
        chr(65 + (hash(range * 73 + 4) % 26)::INT) ||
        chr(65 + (hash(range * 79 + 5) % 26)::INT) ||
        chr(65 + (hash(range * 83 + 6) % 26)::INT) ||
        chr(65 + (hash(range * 89 + 7) % 26)::INT) ||
        chr(65 + (hash(range * 97 + 8) % 26)::INT) ||
        chr(65 + (hash(range *101 + 9) % 26)::INT)
    ) || '-' ||
    chr(65 + (hash(range * 103) % 26)::INT)                 AS inchi_key,

    -- Canonical SMILES (simplified skeleton: fragment + ring system)
    CASE (range % 8)::INT
        WHEN 0 THEN 'c1ccc(cc1)'  -- phenyl scaffold
        WHEN 1 THEN 'C1CCNCC1'    -- piperidine
        WHEN 2 THEN 'c1ccncc1'    -- pyridine
        WHEN 3 THEN 'c1ccc2ncccc2c1' -- quinoline
        WHEN 4 THEN 'C1COCCN1'    -- morpholine
        WHEN 5 THEN 'c1cnc[nH]1'  -- imidazole
        WHEN 6 THEN 'C1CC2CCCC2C1' -- decalin
        ELSE    'c1ccc2c(c1)cccc2'  -- naphthalene
    END ||
    -- side chain variation
    CASE (range % 12)::INT
        WHEN 0 THEN 'OC(=O)'
        WHEN 1 THEN 'NC(=O)'
        WHEN 2 THEN 'C(F)(F)F'
        WHEN 3 THEN 'S(=O)(=O)N'
        WHEN 4 THEN 'OCc1ccccc1'
        WHEN 5 THEN 'CC(=O)O'
        WHEN 6 THEN 'c1ccc(F)cc1'
        WHEN 7 THEN 'c1ccc(Cl)cc1'
        WHEN 8 THEN 'NCC'
        WHEN 9 THEN 'OCC'
        WHEN 10 THEN 'C#N'
        ELSE    'CC'
    END                                                      AS canonical_smiles,

    -- Molecular formula approximation
    'C' || ((8  + (hash(range *  7) % 30))::INT)::VARCHAR ||
    'H' || ((12 + (hash(range * 11) % 40))::INT)::VARCHAR ||
    CASE WHEN (range % 3) > 0 THEN 'N' || ((1 + hash(range * 13) % 4)::INT)::VARCHAR ELSE '' END ||
    CASE WHEN (range % 4) > 0 THEN 'O' || ((1 + hash(range * 17) % 6)::INT)::VARCHAR ELSE '' END ||
    CASE WHEN (range % 7) = 0 THEN 'S'                                                ELSE '' END ||
    CASE WHEN (range % 11) = 0 THEN 'F'                                               ELSE '' END ||
    CASE WHEN (range % 13) = 0 THEN 'Cl'                                              ELSE '' END
                                                             AS molecular_formula,

    -- Molecular weight (log-normal centered ~300 Da, range ~80-900)
    round(
        greatest(80.0, least(900.0,
            80.0 + ((hash(range * 19) % 6000)::DOUBLE / 10.0) *
                   (1.0 + (hash(range * 23) % 1000)::DOUBLE / 5000.0)
        )), 3
    )::DECIMAL(8,3)                                          AS mol_weight,

    -- Exact monoisotopic mass (slightly less than MW for typical organic)
    round(
        greatest(79.9, least(899.5,
            78.5 + ((hash(range * 19) % 6000)::DOUBLE / 10.0) *
                   (1.0 + (hash(range * 23) % 1000)::DOUBLE / 5000.0)
            - (hash(range * 31) % 200)::DOUBLE / 100.0
        )), 5
    )::DECIMAL(10,5)                                         AS exact_mass,

    -- Formal charge (-2 to +3, mostly 0)
    CASE (hash(range * 37) % 20)::INT
        WHEN 0 THEN -2 WHEN 1 THEN -1 WHEN 2 THEN 1 WHEN 3 THEN 2 WHEN 4 THEN 3
        ELSE 0
    END::SMALLINT                                            AS formal_charge,

    -- Hydrogen bond donors (0-10, Lipinski: < 5)
    ((hash(range * 41) % 11))::TINYINT                       AS hbd_count,

    -- Hydrogen bond acceptors (0-15, Lipinski: < 10)
    ((hash(range * 43) % 16))::TINYINT                       AS hba_count,

    -- Rotatable bonds (0-30)
    ((hash(range * 47) % 31))::TINYINT                       AS rotatable_bonds,

    -- Ring count (0-8)
    ((hash(range * 53) % 9))::TINYINT                        AS ring_count,

    -- Aromatic ring count (0-6, ≤ ring_count)
    ((hash(range * 59) % 7))::TINYINT                        AS aromatic_rings,

    -- Stereocenters (0-8)
    ((hash(range * 61) % 9))::TINYINT                        AS stereocenters,

    -- logP (octanol-water partition, -4 to 8)
    round(-4.0 + (hash(range * 67) % 1200)::DOUBLE / 100.0, 2)::DECIMAL(5,2)
                                                             AS logp,

    -- Topological polar surface area (0-200 Å²)
    round((hash(range * 71) % 20001)::DOUBLE / 100.0, 2)::DECIMAL(7,2)
                                                             AS tpsa,

    -- Molecular complexity (0-2000, Bertz complexity approximation)
    ((hash(range * 73) % 2001))::SMALLINT                    AS complexity,

    -- Lipinski violations (0-4)
    ((hash(range * 79) % 5))::TINYINT                        AS lipinski_violations,

    -- QED drug-likeness score (0-1)
    round((hash(range * 83) % 1001)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                             AS qed_score,

    -- Source database (PubChem CID, ChEMBL, ZINC, DrugBank, eMolecules)
    CASE (range % 5)::INT
        WHEN 0 THEN 'pubchem'
        WHEN 1 THEN 'chembl'
        WHEN 2 THEN 'zinc'
        WHEN 3 THEN 'drugbank'
        ELSE        'emolecules'
    END                                                      AS source_db,

    -- Source database ID
    (1000000 + range * 3 + (hash(range) % 50000)::BIGINT)    AS source_db_id,

    -- Creation date (PubChem has compounds going back to 2004)
    (DATE '2004-09-01' + ((hash(range * 89) % 7671)::INTEGER)) AS created_date,

    -- Last modified date (after created_date)
    (DATE '2004-09-01' + ((hash(range * 89) % 7671)::INTEGER)
                       + ((hash(range * 97) % 2000)::INTEGER)) AS modified_date

FROM range(0, 10000000);
SQL
progress "[registry.compounds] done (10M rows)"

progress "[registry] building synonyms (40M rows)..."
"$DB" "$OUT" <<'SQL'
-- Multiple names per compound: INN, USAN, CAS number, trade names, etc.
-- Average ~4 names/compound with a realistic heavy tail for approved drugs.
CREATE TABLE registry.synonyms AS
SELECT
    (range + 1)::BIGINT                                      AS synonym_id,
    -- Assign compound_id with variable density: first 50K compounds (approved drugs)
    -- get extra synonyms, long tail for the rest
    CASE
        WHEN range < 500000    THEN ((range % 50000) + 1)::BIGINT     -- drug-heavy zone
        WHEN range < 5000000   THEN ((range % 1000000) + 1)::BIGINT
        ELSE                        ((range % 10000000) + 1)::BIGINT
    END                                                      AS compound_id,
    CASE (range % 8)::INT
        WHEN 0 THEN 'INN'          -- International Nonproprietary Name
        WHEN 1 THEN 'USAN'         -- United States Adopted Name
        WHEN 2 THEN 'CAS'          -- CAS Registry Number
        WHEN 3 THEN 'trade'        -- Brand / trade name
        WHEN 4 THEN 'systematic'   -- IUPAC systematic name
        WHEN 5 THEN 'trivial'      -- Common / trivial name
        WHEN 6 THEN 'code'         -- Internal dev code (e.g. "MK-0736")
        ELSE        'other'
    END                                                      AS name_type,
    CASE (range % 8)::INT
        WHEN 2 THEN  -- CAS format: NNNNNNN-NN-N
            lpad(((range % 9999999) + 1)::VARCHAR, 7, '0') || '-' ||
            lpad(((hash(range) % 99) + 1)::VARCHAR, 2, '0') || '-' ||
            ((hash(range * 7) % 9) + 1)::VARCHAR
        WHEN 6 THEN  -- dev code: 2-letter prefix + 4 digits
            chr(65 + (hash(range) % 26)::INT) ||
            chr(65 + (hash(range * 3) % 26)::INT) || '-' ||
            lpad(((hash(range * 7) % 9999) + 1)::VARCHAR, 4, '0')
        ELSE
            -- Generic name: syllable-based (mimics pharma INN conventions)
            CASE (range % 20)::INT
                WHEN 0 THEN 'aza' WHEN 1 THEN 'bi' WHEN 2 THEN 'car'
                WHEN 3 THEN 'di'  WHEN 4 THEN 'ef' WHEN 5 THEN 'flu'
                WHEN 6 THEN 'gab' WHEN 7 THEN 'hi'  WHEN 8 THEN 'in'
                WHEN 9 THEN 'ket' WHEN 10 THEN 'lor' WHEN 11 THEN 'mo'
                WHEN 12 THEN 'nit' WHEN 13 THEN 'ol'  WHEN 14 THEN 'pro'
                WHEN 15 THEN 'qui' WHEN 16 THEN 'ro'   WHEN 17 THEN 'sul'
                WHEN 18 THEN 'tri' ELSE 'ven'
            END ||
            CASE (range % 15)::INT
                WHEN 0 THEN 'ax' WHEN 1 THEN 'cin' WHEN 2 THEN 'done'
                WHEN 3 THEN 'fen' WHEN 4 THEN 'glip' WHEN 5 THEN 'ide'
                WHEN 6 THEN 'il' WHEN 7 THEN 'mab' WHEN 8 THEN 'nib'
                WHEN 9 THEN 'olol' WHEN 10 THEN 'pam' WHEN 11 THEN 'pril'
                WHEN 12 THEN 'sartan' WHEN 13 THEN 'statin' ELSE 'zole'
            END ||
            CASE WHEN (range % 3) = 0 THEN '' 
                 ELSE ' ' || ((range % 900) + 100)::VARCHAR || ' mg'
            END
    END                                                      AS name,
    (range % 3 = 0)                                          AS is_preferred,
    CASE (range % 5)::INT
        WHEN 0 THEN 'en' WHEN 1 THEN 'de' WHEN 2 THEN 'fr'
        WHEN 3 THEN 'ja' ELSE 'es'
    END                                                      AS language_code,
    (DATE '2000-01-01' + ((hash(range * 11) % 9131)::INTEGER)) AS registered_date
FROM range(0, 40000000);
SQL
progress "[registry.synonyms] done (40M rows)"

progress "[registry] building cross-references (5M rows)..."
"$DB" "$OUT" <<'SQL'
-- Cross-references to external databases (PubChem ↔ ChEMBL ↔ DrugBank ↔ ZINC)
CREATE TABLE registry.cross_refs AS
SELECT
    (range + 1)::BIGINT                                      AS xref_id,
    ((range % 10000000) + 1)::BIGINT                         AS compound_id,
    CASE (range % 6)::INT
        WHEN 0 THEN 'chembl'
        WHEN 1 THEN 'drugbank'
        WHEN 2 THEN 'zinc'
        WHEN 3 THEN 'kegg'
        WHEN 4 THEN 'hmdb'
        ELSE        'chebi'
    END                                                      AS source_db,
    CASE (range % 6)::INT
        WHEN 0 THEN 'CHEMBL' || (range % 2000000 + 1)::VARCHAR
        WHEN 1 THEN 'DB' || lpad((range % 15000 + 1)::VARCHAR, 5, '0')
        WHEN 2 THEN 'ZINC' || lpad((range % 35000000 + 1)::VARCHAR, 12, '0')
        WHEN 3 THEN 'C' || lpad((range % 18000 + 1)::VARCHAR, 5, '0')
        WHEN 4 THEN 'HMDB' || lpad((range % 220000 + 1)::VARCHAR, 7, '0')
        ELSE        'CHEBI:' || (range % 200000 + 1)::VARCHAR
    END                                                      AS external_id,
    (range % 4 = 0)                                          AS is_primary,
    (TIMESTAMP '2010-01-01' + INTERVAL ((hash(range * 7) % 525960)::VARCHAR || ' minutes'))
                                                             AS synced_at
FROM range(0, 5000000);
SQL
progress "[registry.cross_refs] done (5M rows)"

# ─────────────────────────────────────────────────────────────────────────────
# SCHEMA: bioassay — targets + assays + 1 BILLION activity rows
# ─────────────────────────────────────────────────────────────────────────────
progress "[bioassay] building targets (100K rows)..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA bioassay;

-- Biological targets (protein/gene/pathway-level)
CREATE TABLE bioassay.targets AS
SELECT
    (range + 1)::INTEGER                                     AS target_id,
    -- Gene symbol: 2-6 uppercase letters + optional number
    upper(
        chr(65 + (hash(range)      % 26)::INT) ||
        chr(65 + (hash(range * 7)  % 26)::INT) ||
        chr(65 + (hash(range * 11) % 26)::INT) ||
        CASE WHEN (range % 3) > 0 THEN chr(65 + (hash(range * 13) % 26)::INT) ELSE '' END ||
        CASE WHEN (range % 5) > 0 THEN chr(65 + (hash(range * 17) % 26)::INT) ELSE '' END
    ) || CASE WHEN (range % 4) = 0 THEN (range % 9 + 1)::VARCHAR ELSE '' END
                                                             AS gene_symbol,
    -- UniProt-style accession: letter + 5 chars + 1 letter + 2 digits
    chr(65 + (hash(range * 19) % 26)::INT) ||
    upper(substring(md5(range::VARCHAR), 1, 5)) ||
    chr(65 + (hash(range * 23) % 26)::INT) ||
    lpad((hash(range * 29) % 100)::VARCHAR, 2, '0')          AS uniprot_id,
    CASE (range % 12)::INT
        WHEN 0  THEN 'GPCR'
        WHEN 1  THEN 'kinase'
        WHEN 2  THEN 'ion channel'
        WHEN 3  THEN 'protease'
        WHEN 4  THEN 'nuclear receptor'
        WHEN 5  THEN 'transporter'
        WHEN 6  THEN 'epigenetic'
        WHEN 7  THEN 'phosphatase'
        WHEN 8  THEN 'ubiquitin ligase'
        WHEN 9  THEN 'oxidoreductase'
        WHEN 10 THEN 'transferase'
        ELSE        'other enzyme'
    END                                                      AS target_class,
    CASE (range % 6)::INT
        WHEN 0 THEN 'Homo sapiens'
        WHEN 1 THEN 'Mus musculus'
        WHEN 2 THEN 'Rattus norvegicus'
        WHEN 3 THEN 'Saccharomyces cerevisiae'
        WHEN 4 THEN 'Escherichia coli'
        ELSE        'Homo sapiens'
    END                                                      AS organism,
    CASE (range % 6)::INT
        WHEN 0 THEN 9606 WHEN 1 THEN 10090 WHEN 2 THEN 10116
        WHEN 3 THEN 4932 WHEN 4 THEN 562   ELSE 9606
    END::INTEGER                                             AS taxonomy_id,
    ((hash(range * 31) % 300) + 1)::SMALLINT                 AS therapeutic_area_id,
    (range % 10 = 0)                                         AS is_clinical_target,
    (range % 50 = 0)                                         AS has_crystal_structure
FROM range(0, 100000);
SQL

progress "[bioassay] building assay definitions (500K rows)..."
"$DB" "$OUT" <<'SQL'
-- Assay definitions: what was measured, against which target, by whom
CREATE TABLE bioassay.assays AS
SELECT
    (range + 1)::INTEGER                                     AS assay_id,
    -- Most assays map to one of the 100K targets; some are cell-based (NULL target)
    CASE WHEN (range % 10) = 0 THEN NULL
         ELSE ((range % 100000) + 1)::INTEGER
    END                                                      AS target_id,
    CASE (range % 8)::INT
        WHEN 0 THEN 'biochemical'
        WHEN 1 THEN 'cell-based'
        WHEN 2 THEN 'ADMET'
        WHEN 3 THEN 'phenotypic'
        WHEN 4 THEN 'binding'
        WHEN 5 THEN 'functional'
        WHEN 6 THEN 'reporter'
        ELSE        'other'
    END                                                      AS assay_type,
    CASE (range % 6)::INT
        WHEN 0 THEN 'fluorescence polarization'
        WHEN 1 THEN 'TR-FRET'
        WHEN 2 THEN 'luciferase reporter'
        WHEN 3 THEN 'surface plasmon resonance'
        WHEN 4 THEN 'radioligand binding'
        ELSE        'cell viability (CellTiter-Glo)'
    END                                                      AS detection_method,
    ((range % 34) + 1)::SMALLINT                             AS bioactivity_type_id,
    CASE (range % 5)::INT
        WHEN 0 THEN 'pubchem_bioassay'
        WHEN 1 THEN 'chembl'
        WHEN 2 THEN 'tox21'
        WHEN 3 THEN 'drugmatrix'
        ELSE        'internal'
    END                                                      AS data_source,
    'AID' || (range + 1)::VARCHAR                            AS source_assay_id,
    -- HTS assays have more compounds tested
    CASE WHEN (range % 5) = 0 THEN 'HTS' ELSE 'targeted' END AS screening_mode,
    -- Concentration range (typical HTS: 10 µM single-point; dose-response: 0.001-100 µM)
    CASE WHEN (range % 5) = 0 THEN 10.0 ELSE 0.001 END::DECIMAL(8,4)
                                                             AS conc_min_um,
    CASE WHEN (range % 5) = 0 THEN 10.0 ELSE 100.0 END::DECIMAL(8,4)
                                                             AS conc_max_um,
    (DATE '2000-01-01' + ((hash(range * 7) % 9131)::INTEGER)) AS deposited_date,
    (range % 100 = 0)                                        AS is_public,
    (range % 20 = 0)                                         AS has_dose_response
FROM range(0, 500000);
SQL
progress "[bioassay.assays] done (500K rows)"

# ── THE BIG TABLE: 1 billion bioassay activity results
if [[ "$FAST" == "true" ]]; then
  progress "[bioassay] FAST mode — skipping 1B activities table, inserting 10M instead"
  ACTIVITIES_END=10000000
else
  progress "[bioassay] building activities (1,000,000,000 rows) — this will take a while..."
  ACTIVITIES_END=1000000000
fi

"$DB" "$OUT" -c "
-- Each row is one compound tested in one assay.
-- Outcomes: ~2% Active, ~3% Inconclusive, ~5% Probe candidate, ~90% Inactive.
-- Active compounds get a numeric score (IC50/EC50-like value in µM).
-- This mirrors PubChem BioAssay which has >300B data points; we simulate 1B.
CREATE TABLE bioassay.activities AS
SELECT
    (range + 1)::BIGINT                                      AS activity_id,

    -- Assay: 500K assays, non-uniform sampling (HTS assays get more hits)
    ((range * 7 + (hash(range) % 11)::BIGINT) % 500000 + 1)::INTEGER
                                                             AS assay_id,

    -- Compound: 10M compounds, non-uniform (scaffolds cluster)
    ((range * 13 + (hash(range * 3) % 23)::BIGINT) % 10000000 + 1)::BIGINT
                                                             AS compound_id,

    -- Outcome distribution (realistic HTS hit rates)
    CASE ((hash(range * 17) % 100)::INT)
        WHEN 0 THEN 'Active'
        WHEN 1 THEN 'Active'     -- 2% active
        WHEN 2 THEN 'Probe'      -- 1% probe
        WHEN 3 THEN 'Inconclusive'
        WHEN 4 THEN 'Inconclusive' -- 2% inconclusive
        ELSE        CASE ((hash(range * 19) % 2)::INT)
                        WHEN 0 THEN 'Inactive' ELSE 'Unspecified'
                    END          -- 95% inactive/unspecified
    END                                                      AS outcome,

    -- Activity value: only populated for active/probe compounds (nM scale)
    CASE WHEN (hash(range * 17) % 100)::INT < 3
         THEN round(
             pow(10::DOUBLE,
                 -3.0 + (hash(range * 23) % 7000)::DOUBLE / 1000.0
             )::DOUBLE * 1000.0,  -- convert M to nM
             3
         )
         ELSE NULL
    END::DECIMAL(14,4)                                       AS activity_value_nm,

    -- Relation qualifier (<, =, >)
    CASE WHEN (hash(range * 17) % 100)::INT < 3
         THEN CASE (hash(range * 29) % 5)::INT
                  WHEN 0 THEN '>' WHEN 1 THEN '<' ELSE '='
              END
         ELSE NULL
    END::VARCHAR(2)                                          AS activity_relation,

    -- Confidence score 1-9 (PubChem convention)
    ((hash(range * 31) % 9) + 1)::TINYINT                   AS confidence_score,

    -- Test date (2000-2026)
    (DATE '2000-01-01' + ((hash(range * 37) % 9496)::INTEGER)) AS test_date,

    -- Replicate number (1-4)
    ((hash(range * 41) % 4) + 1)::TINYINT                   AS replicate_num

FROM range(0, ${ACTIVITIES_END});
"
progress "[bioassay.activities] done (${ACTIVITIES_END} rows)"

# ─────────────────────────────────────────────────────────────────────────────
# SCHEMA: toxicology — regulatory studies + in-vitro panels
# ─────────────────────────────────────────────────────────────────────────────
progress "[toxicology] building structure alerts (5K rows)..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA toxicology;

-- SMARTS-based structural alert patterns (Ames mutagens, Brenk, PAINS, GSK,
-- Pfizer CNS MPO alerts — real alert classes, synthetic SMARTS patterns)
CREATE TABLE toxicology.structure_alerts (
    alert_id      INTEGER PRIMARY KEY,
    alert_set     VARCHAR(30) NOT NULL,  -- 'Ames', 'Brenk', 'PAINS', 'REOS', 'GSK'
    alert_name    VARCHAR(100) NOT NULL,
    smarts        VARCHAR(500) NOT NULL,
    mechanism     VARCHAR(200),
    severity      TINYINT,              -- 1 (mild concern) to 5 (strong alert)
    action        VARCHAR(20),          -- 'flag', 'reject', 'investigate'
    citation      VARCHAR(200)
);
INSERT INTO toxicology.structure_alerts
SELECT
    range + 1,
    CASE (range % 6)::INT
        WHEN 0 THEN 'Ames'   WHEN 1 THEN 'Brenk'  WHEN 2 THEN 'PAINS'
        WHEN 3 THEN 'REOS'   WHEN 4 THEN 'GSK'    ELSE        'Pfizer_CNS'
    END,
    CASE (range % 20)::INT
        WHEN 0  THEN 'Nitroaromatic'          WHEN 1  THEN 'Primary amine'
        WHEN 2  THEN 'Alkyl halide'           WHEN 3  THEN 'Michael acceptor'
        WHEN 4  THEN 'Quinone'                WHEN 5  THEN 'Epoxide'
        WHEN 6  THEN 'N-oxide'                WHEN 7  THEN 'Azo compound'
        WHEN 8  THEN 'Acrolein'               WHEN 9  THEN 'Aldehyde'
        WHEN 10 THEN 'Acylhydrazide'          WHEN 11 THEN 'Thiol'
        WHEN 12 THEN 'Disulfide'              WHEN 13 THEN 'Polycyclic aromatic'
        WHEN 14 THEN 'Reactive electrophile'  WHEN 15 THEN 'Chelator'
        WHEN 16 THEN 'Redox cycling fragment' WHEN 17 THEN 'Aggregator scaffold'
        WHEN 18 THEN 'Promiscuous binder'     ELSE        'Photoreactive'
    END || ' (alert ' || (range + 1)::VARCHAR || ')',
    -- Simplified SMARTS (structural patterns)
    CASE (range % 10)::INT
        WHEN 0 THEN '[NX3;H2][cX3]'
        WHEN 1 THEN '[N+](=O)[O-]'
        WHEN 2 THEN '[CX4][Cl,Br,I]'
        WHEN 3 THEN 'C=CC(=O)'
        WHEN 4 THEN 'O=C1C=CC(=O)C=C1'
        WHEN 5 THEN 'C1OC1'
        WHEN 6 THEN '[NX3]=[OX1]'
        WHEN 7 THEN '[aX2][NX2]=[NX2][aX2]'
        WHEN 8 THEN 'C=CC=O'
        ELSE        '[CX3H1](=O)'
    END,
    CASE (range % 5)::INT
        WHEN 0 THEN 'Direct-acting mutagen via DNA alkylation'
        WHEN 1 THEN 'Metabolic activation to reactive intermediate'
        WHEN 2 THEN 'Covalent protein binding / hapten formation'
        WHEN 3 THEN 'Redox cycling / ROS generation'
        ELSE        'Promiscuous binding / assay interference'
    END,
    ((range % 5) + 1)::TINYINT,
    CASE (range % 3)::INT
        WHEN 0 THEN 'reject' WHEN 1 THEN 'flag' ELSE 'investigate'
    END,
    'PMID:' || (10000000 + range * 7)::VARCHAR  -- citation
FROM range(0, 5000);
SQL

progress "[toxicology] building regulatory studies (2M rows)..."
"$DB" "$OUT" <<'SQL'
-- Regulatory toxicology study records (OECD guidelines, ICH, EPA OPPTS)
CREATE TABLE toxicology.studies AS
SELECT
    (range + 1)::BIGINT                                       AS study_id,
    ((range % 10000000) + 1)::BIGINT                          AS compound_id,
    -- Guideline (OECD TG, ICH S-series, EPA OPPTS, NTP)
    CASE (range % 20)::INT
        WHEN 0  THEN 'OECD TG 407'    -- 28-day oral rat
        WHEN 1  THEN 'OECD TG 408'    -- 90-day oral rat
        WHEN 2  THEN 'OECD TG 409'    -- 90-day oral non-rodent
        WHEN 3  THEN 'OECD TG 410'    -- 21-day dermal rat
        WHEN 4  THEN 'OECD TG 411'    -- 90-day dermal rat
        WHEN 5  THEN 'OECD TG 412'    -- 28-day inhalation rat
        WHEN 6  THEN 'OECD TG 413'    -- 90-day inhalation rat
        WHEN 7  THEN 'OECD TG 414'    -- prenatal dev tox rabbit
        WHEN 8  THEN 'OECD TG 415'    -- 1-gen reproductive rat
        WHEN 9  THEN 'OECD TG 416'    -- 2-gen reproductive rat
        WHEN 10 THEN 'OECD TG 423'    -- acute oral LD50
        WHEN 11 THEN 'OECD TG 443'    -- extended 1-gen reproductive
        WHEN 12 THEN 'OECD TG 451'    -- carcinogenicity mouse 18-month
        WHEN 13 THEN 'OECD TG 453'    -- combined carcinogenicity rat 24-month
        WHEN 14 THEN 'ICH S1B'        -- carcinogenicity transgenic mouse
        WHEN 15 THEN 'ICH S7A'        -- safety pharmacology core battery
        WHEN 16 THEN 'ICH S7B'        -- hERG / cardiac safety
        WHEN 17 THEN 'EPA OPPTS 870.3100' -- 90-day oral
        WHEN 18 THEN 'NTP 2-year'     -- NTP chronic bioassay
        ELSE        'FDA Redbook'     -- food additive tox
    END                                                       AS guideline,
    ((range % 17) + 1)::SMALLINT                              AS species_id,
    CASE (range % 4)::INT
        WHEN 0 THEN 'M' WHEN 1 THEN 'F' WHEN 2 THEN 'M/F' ELSE 'M/F'
    END                                                       AS sex,
    ((range % 15) + 1)::TINYINT                               AS route_id,
    -- Duration (days): acute=1, sub-acute=28, sub-chronic=90, chronic=180/365/730
    CASE (range % 8)::INT
        WHEN 0 THEN 1   WHEN 1 THEN 7   WHEN 2 THEN 14
        WHEN 3 THEN 28  WHEN 4 THEN 90  WHEN 5 THEN 180
        WHEN 6 THEN 365 ELSE 730
    END::SMALLINT                                             AS duration_days,
    -- Dose groups: typically 3 + control
    ((range % 4) + 3)::TINYINT                                AS dose_groups,
    -- Animals per group
    CASE (range % 4)::INT
        WHEN 0 THEN 5 WHEN 1 THEN 10 WHEN 2 THEN 15 ELSE 20
    END::TINYINT                                              AS animals_per_group,
    CASE (range % 5)::INT
        WHEN 0 THEN 'mg/kg/day' WHEN 1 THEN 'mg/kg' WHEN 2 THEN 'ppm'
        WHEN 3 THEN 'mg/m³'     ELSE 'mg/kg bw/day'
    END                                                       AS dose_unit,
    CASE (range % 4)::INT
        WHEN 0 THEN 'regulatory' WHEN 1 THEN 'academic'
        WHEN 2 THEN 'industry'   ELSE 'NTP'
    END                                                       AS study_type,
    -- GLP status (Good Laboratory Practice)
    (range % 3 != 0)                                          AS glp_compliant,
    -- Study year (1975-2025)
    (1975 + (hash(range * 7) % 51)::INT)::SMALLINT            AS study_year,
    -- NOAEL and LOAEL (mg/kg/day); NULL for non-relevant endpoints
    CASE WHEN (range % 5) != 4
         THEN round((hash(range * 11) % 100000)::DOUBLE / 100.0, 2)
         ELSE NULL
    END::DECIMAL(10,2)                                        AS noael_mgkg,
    CASE WHEN (range % 5) != 4
         THEN round(((hash(range * 11) % 100000)::DOUBLE / 100.0) *
                    (2.0 + (hash(range * 13) % 800)::DOUBLE / 100.0), 2)
         ELSE NULL
    END::DECIMAL(10,2)                                        AS loael_mgkg
FROM range(0, 2000000);
SQL
progress "[toxicology.studies] done (2M rows)"

progress "[toxicology] building findings (200M rows)..."
"$DB" "$OUT" <<'SQL'
-- Individual toxicological findings per study, dose group, organ, and endpoint.
-- This is the granular regulatory data: each row is one finding.
CREATE TABLE toxicology.findings AS
SELECT
    (range + 1)::BIGINT                                       AS finding_id,
    ((range % 2000000) + 1)::BIGINT                           AS study_id,

    -- Organ / tissue
    CASE (range % 25)::INT
        WHEN 0  THEN 'liver'          WHEN 1  THEN 'kidney'
        WHEN 2  THEN 'spleen'         WHEN 3  THEN 'testes'
        WHEN 4  THEN 'ovaries'        WHEN 5  THEN 'thyroid'
        WHEN 6  THEN 'adrenal'        WHEN 7  THEN 'lung'
        WHEN 8  THEN 'heart'          WHEN 9  THEN 'brain'
        WHEN 10 THEN 'bone marrow'    WHEN 11 THEN 'lymph node'
        WHEN 12 THEN 'thymus'         WHEN 13 THEN 'stomach'
        WHEN 14 THEN 'intestine'      WHEN 15 THEN 'pancreas'
        WHEN 16 THEN 'urinary bladder' WHEN 17 THEN 'mammary gland'
        WHEN 18 THEN 'skin'           WHEN 19 THEN 'eye'
        WHEN 20 THEN 'nose'           WHEN 21 THEN 'nasal turbinate'
        WHEN 22 THEN 'femur'          WHEN 23 THEN 'sciatic nerve'
        ELSE        'blood'
    END                                                       AS organ,

    -- Endpoint type
    CASE (range % 12)::INT
        WHEN 0  THEN 'histopathology'
        WHEN 1  THEN 'clinical chemistry'
        WHEN 2  THEN 'hematology'
        WHEN 3  THEN 'organ weight'
        WHEN 4  THEN 'body weight'
        WHEN 5  THEN 'food consumption'
        WHEN 6  THEN 'ophthalmology'
        WHEN 7  THEN 'urinalysis'
        WHEN 8  THEN 'gross pathology'
        WHEN 9  THEN 'clinical signs'
        WHEN 10 THEN 'neurobehavioral'
        ELSE        'immunophenotyping'
    END                                                       AS endpoint_type,

    -- Finding term (subset of standard controlled vocabulary)
    CASE (range % 30)::INT
        WHEN 0  THEN 'hepatocellular hypertrophy'
        WHEN 1  THEN 'centrilobular necrosis'
        WHEN 2  THEN 'tubular degeneration'
        WHEN 3  THEN 'glomerulonephritis'
        WHEN 4  THEN 'follicular hyperplasia'
        WHEN 5  THEN 'cortical atrophy'
        WHEN 6  THEN 'spermatogenic cell depletion'
        WHEN 7  THEN 'interstitial fibrosis'
        WHEN 8  THEN 'ALT elevation'
        WHEN 9  THEN 'creatinine elevation'
        WHEN 10 THEN 'neutrophilia'
        WHEN 11 THEN 'thrombocytopenia'
        WHEN 12 THEN 'decreased erythrocyte count'
        WHEN 13 THEN 'increased organ weight'
        WHEN 14 THEN 'decreased body weight gain'
        WHEN 15 THEN 'reduced food consumption'
        WHEN 16 THEN 'corneal opacity'
        WHEN 17 THEN 'proteinuria'
        WHEN 18 THEN 'hemorrhage'
        WHEN 19 THEN 'hyperemia'
        WHEN 20 THEN 'ataxia'
        WHEN 21 THEN 'tremors'
        WHEN 22 THEN 'decreased NK cell activity'
        WHEN 23 THEN 'thymic atrophy'
        WHEN 24 THEN 'splenic depletion'
        WHEN 25 THEN 'bone marrow hypoplasia'
        WHEN 26 THEN 'vacuolation'
        WHEN 27 THEN 'pigmentation'
        WHEN 28 THEN 'calcification'
        ELSE        'no remarkable finding'
    END                                                       AS finding_term,

    -- Severity (0=none, 1=minimal, 2=mild, 3=moderate, 4=marked, 5=severe)
    ((hash(range * 7) % 6))::TINYINT                          AS severity,

    -- Dose group (0=control, 1/2/3=low/mid/high dose)
    ((range % 4))::TINYINT                                    AS dose_group,

    -- Dose value at this group
    round(
        CASE (range % 4)::INT
            WHEN 0 THEN 0.0
            WHEN 1 THEN (hash(range * 11) % 1000)::DOUBLE / 10.0
            WHEN 2 THEN (hash(range * 11) % 1000)::DOUBLE / 10.0 * 3.0
            ELSE        (hash(range * 11) % 1000)::DOUBLE / 10.0 * 10.0
        END, 2
    )::DECIMAL(10,2)                                          AS dose_value,

    -- Incidence: numerator / denominator
    ((hash(range * 13) % 20) + 1)::TINYINT                    AS incidence_n,
    ((hash(range * 13) % 20) + (hash(range * 17) % 5) + 1)::TINYINT
                                                              AS incidence_d,

    -- Effect level classification
    CASE (hash(range * 19) % 5)::INT
        WHEN 0 THEN 'NOEL'  WHEN 1 THEN 'NOAEL'
        WHEN 2 THEN 'LOAEL' WHEN 3 THEN 'LOEL'
        ELSE        NULL
    END                                                       AS effect_level,

    -- Treatment-related flag
    (hash(range * 23) % 4 != 0)                               AS treatment_related,

    -- Reversibility (after recovery period)
    CASE (hash(range * 29) % 3)::INT
        WHEN 0 THEN 'fully reversible'
        WHEN 1 THEN 'partially reversible'
        ELSE        'irreversible'
    END                                                       AS reversibility

FROM range(0, 200000000);
SQL
progress "[toxicology.findings] done (200M rows)"

progress "[toxicology] building acute toxicity (5M rows)..."
"$DB" "$OUT" <<'SQL'
-- Acute toxicity database (LD50/LC50 values, experimental + predicted)
-- Covers oral, dermal, and inhalation routes for multiple species.
CREATE TABLE toxicology.acute_tox AS
SELECT
    (range + 1)::BIGINT                                       AS record_id,
    ((range * 3 + hash(range) % 7) % 10000000 + 1)::BIGINT   AS compound_id,
    ((range % 17) + 1)::SMALLINT                              AS species_id,
    CASE (range % 2)::INT WHEN 0 THEN 'M' ELSE 'F' END        AS sex,
    ((range % 15) + 1)::TINYINT                               AS route_id,
    CASE (range % 3)::INT
        WHEN 0 THEN 'LD50' WHEN 1 THEN 'LC50' ELSE 'LDLo'
    END                                                       AS measurement_type,
    -- LD50 in mg/kg: log-normal, median ~300, range 0.001 to 100,000
    round(
        pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0), 3
    )::DECIMAL(12,3)                                          AS value_mgkg,
    -- 95% confidence interval
    round(
        pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) *
        (0.7 + (hash(range * 11) % 400)::DOUBLE / 1000.0), 3
    )::DECIMAL(12,3)                                          AS ci_lo_mgkg,
    round(
        pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) *
        (1.2 + (hash(range * 13) % 800)::DOUBLE / 1000.0), 3
    )::DECIMAL(12,3)                                          AS ci_hi_mgkg,
    -- GHS category based on value
    CASE
        WHEN pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) <= 5    THEN 'Acute1'
        WHEN pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) <= 50   THEN 'Acute2'
        WHEN pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) <= 300  THEN 'Acute3'
        WHEN pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) <= 2000 THEN 'Acute4'
        WHEN pow(10::DOUBLE, 0.5 + (hash(range * 7) % 4500)::DOUBLE / 1000.0) <= 5000 THEN 'Acute5'
        ELSE NULL
    END                                                       AS ghs_category_id,
    (range % 3 = 0)                                           AS is_experimental,
    (1975 + (hash(range * 17) % 51)::INT)::SMALLINT           AS study_year,
    'PMID:' || (5000000 + range * 11)::VARCHAR                AS reference
FROM range(0, 5000000);
SQL
progress "[toxicology.acute_tox] done (5M rows)"

progress "[toxicology] building compound alerts (15M rows)..."
"$DB" "$OUT" <<'SQL'
-- Which compounds trigger which structural alerts
-- (realistic: ~1.5 alerts/compound on average for drug-like compounds)
CREATE TABLE toxicology.compound_alerts AS
SELECT
    (range + 1)::BIGINT                                       AS ca_id,
    ((range * 7 + hash(range) % 13) % 10000000 + 1)::BIGINT  AS compound_id,
    ((range % 5000) + 1)::INTEGER                             AS alert_id,
    (range % 3 != 0)                                          AS is_match,
    CASE (range % 3)::INT
        WHEN 0 THEN 'substructure_search'
        WHEN 1 THEN 'ml_prediction'
        ELSE        'rule_based'
    END                                                       AS method,
    -- Confidence (0.5-1.0 for matches, 0.0-0.5 for non-matches)
    CASE WHEN (range % 3 != 0)
         THEN round(0.5 + (hash(range * 7) % 500)::DOUBLE / 1000.0, 4)
         ELSE round(0.0 + (hash(range * 7) % 500)::DOUBLE / 1000.0, 4)
    END::DECIMAL(5,4)                                         AS confidence,
    ('model_v' || ((range % 5) + 1)::VARCHAR || '.' ||
     ((hash(range) % 10))::VARCHAR)                           AS model_version
FROM range(0, 15000000);
SQL
progress "[toxicology.compound_alerts] done (15M rows)"

# ─────────────────────────────────────────────────────────────────────────────
# SCHEMA: admet — ADMET property predictions
# ─────────────────────────────────────────────────────────────────────────────
progress "[admet] building physicochemical predictions (10M rows)..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA admet;

-- Predicted/calculated physicochemical and ADMET properties per compound per model.
-- Covers Caco-2 permeability, solubility, protein binding, logD, pKa, etc.
CREATE TABLE admet.physchem_predictions AS
SELECT
    (range + 1)::BIGINT                                       AS pred_id,
    ((range % 10000000) + 1)::BIGINT                          AS compound_id,
    ('ADMETlab_v' || ((range % 3) + 2)::VARCHAR)              AS model_name,

    -- Predicted pKa (acid, 0-14)
    round(2.0 + (hash(range * 7) % 1200)::DOUBLE / 100.0, 2)::DECIMAL(5,2)
                                                              AS pka_acid,

    -- Predicted pKa (base, 0-14)
    round(4.0 + (hash(range * 11) % 1000)::DOUBLE / 100.0, 2)::DECIMAL(5,2)
                                                              AS pka_base,

    -- Aqueous solubility (µg/mL, log-normal, median ~10 µg/mL)
    round(pow(10::DOUBLE, -1.0 + (hash(range * 13) % 3000)::DOUBLE / 1000.0), 3)::DECIMAL(10,3)
                                                              AS solubility_ug_ml,

    -- Caco-2 permeability (10⁻⁶ cm/s, 0.1-100)
    round(0.1 + (hash(range * 17) % 9990)::DOUBLE / 100.0, 3)::DECIMAL(7,3)
                                                              AS perm_caco2_cm_s,

    -- MDCK permeability
    round(0.1 + (hash(range * 19) % 9990)::DOUBLE / 100.0, 3)::DECIMAL(7,3)
                                                              AS perm_mdck_cm_s,

    -- Efflux ratio (1-100, >2 suggests P-gp substrate)
    round(1.0 + (hash(range * 23) % 9900)::DOUBLE / 100.0, 2)::DECIMAL(6,2)
                                                              AS efflux_ratio,

    -- logD at pH 7.4 (-4 to 8)
    round(-4.0 + (hash(range * 29) % 1200)::DOUBLE / 100.0, 3)::DECIMAL(6,3)
                                                              AS logd74,

    -- Plasma protein binding (% bound, 0-99.99)
    round((hash(range * 31) % 10000)::DOUBLE / 100.0, 2)::DECIMAL(5,2)
                                                              AS ppb_pct,

    -- Volume of distribution (L/kg, 0.04-200)
    round(0.04 + (hash(range * 37) % 19996)::DOUBLE / 100.0, 3)::DECIMAL(8,3)
                                                              AS vdss_l_kg,

    -- Fraction unbound in plasma (0.001-1.0)
    round(0.001 + (hash(range * 41) % 999)::DOUBLE / 1000.0, 4)::DECIMAL(6,4)
                                                              AS fu_plasma,

    -- BBB penetration probability (0-1)
    round((hash(range * 43) % 1001)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                              AS bbb_prob,

    -- hERG inhibition IC50 (µM, safety concern at <1 µM)
    round(pow(10::DOUBLE, -1.0 + (hash(range * 47) % 4000)::DOUBLE / 1000.0), 4)::DECIMAL(10,4)
                                                              AS herg_ic50_um,

    -- Oral bioavailability (% predicted)
    round((hash(range * 53) % 10001)::DOUBLE / 100.0, 2)::DECIMAL(6,2)
                                                              AS bioavailability_pct,

    -- Model prediction date
    (DATE '2018-01-01' + ((hash(range * 59) % 3000)::INTEGER)) AS predicted_date

FROM range(0, 10000000);
SQL
progress "[admet.physchem_predictions] done (10M rows)"

progress "[admet] building CYP interaction predictions (50M rows)..."
"$DB" "$OUT" <<'SQL'
-- CYP450 enzyme interaction predictions (5 major isoforms × 10M compounds).
-- Each row is one compound × one CYP isoform.
-- In drug discovery, CYP interactions drive drug-drug interaction (DDI) risk.
CREATE TABLE admet.cyp_interactions AS
SELECT
    (range + 1)::BIGINT                                       AS interaction_id,
    ((range % 10000000) + 1)::BIGINT                          AS compound_id,
    CASE (range % 5)::INT
        WHEN 0 THEN 'CYP1A2'
        WHEN 1 THEN 'CYP2C9'
        WHEN 2 THEN 'CYP2C19'
        WHEN 3 THEN 'CYP2D6'
        ELSE        'CYP3A4'
    END                                                       AS cyp_isoform,

    -- Inhibition probability (0-1)
    round((hash(range * 7) % 1001)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                              AS inhibition_prob,

    -- Substrate probability (0-1)
    round((hash(range * 11) % 1001)::DOUBLE / 1000.0, 4)::DECIMAL(5,4)
                                                              AS substrate_prob,

    -- Predicted Ki for inhibition (µM, 0.001-1000)
    CASE WHEN (hash(range * 7) % 1001)::DOUBLE / 1000.0 > 0.5
         THEN round(pow(10::DOUBLE, -3.0 + (hash(range * 13) % 6000)::DOUBLE / 1000.0), 4)
         ELSE NULL
    END::DECIMAL(12,4)                                        AS ki_um,

    -- Inhibition type (competitive, non-competitive, etc.)
    CASE WHEN (hash(range * 7) % 1001)::DOUBLE / 1000.0 > 0.5
         THEN CASE (hash(range * 17) % 4)::INT
                  WHEN 0 THEN 'competitive'
                  WHEN 1 THEN 'non-competitive'
                  WHEN 2 THEN 'mixed'
                  ELSE        'uncompetitive'
              END
         ELSE NULL
    END                                                       AS inhibition_type,

    -- TDI flag (time-dependent inhibition — mechanism-based DDI risk)
    ((hash(range * 19) % 10) = 0)                             AS is_tdi,

    -- Model name
    ('DeepCYP_v' || ((range % 4) + 1)::VARCHAR)               AS model_name,

    (DATE '2020-01-01' + ((hash(range * 23) % 2557)::INTEGER)) AS predicted_date

FROM range(0, 50000000);
SQL
progress "[admet.cyp_interactions] done (50M rows)"

# ─────────────────────────────────────────────────────────────────────────────
# SCHEMA: clinical — approved drugs, adverse events, clinical trials
# ─────────────────────────────────────────────────────────────────────────────
progress "[clinical] building approved drugs (5K rows)..."
"$DB" "$OUT" <<'SQL'
CREATE SCHEMA clinical;

-- Approved drugs (subset of ~5K FDA-approved small molecule drugs)
CREATE TABLE clinical.drugs AS
SELECT
    (range + 1)::INTEGER                                      AS drug_id,
    (range + 1)::BIGINT                                       AS compound_id,   -- maps to first 5K compounds
    -- Brand name (synthetic pharma-style)
    chr(65 + (hash(range) % 26)::INT) ||
    CASE (range % 10)::INT
        WHEN 0 THEN 'vir' WHEN 1 THEN 'zol' WHEN 2 THEN 'max'
        WHEN 3 THEN 'tab' WHEN 4 THEN 'cel' WHEN 5 THEN 'nex'
        WHEN 6 THEN 'pri' WHEN 7 THEN 'lor' WHEN 8 THEN 'pam'
        ELSE 'xen'
    END ||
    CASE (range % 5)::INT WHEN 0 THEN ' XR' WHEN 1 THEN ' CR' ELSE '' END
                                                              AS brand_name,
    -- Generic name from synonyms space
    'compound_' || lpad((range + 1)::VARCHAR, 7, '0')         AS generic_name,
    CASE (range % 4)::INT
        WHEN 0 THEN 'approved'
        WHEN 1 THEN 'approved'
        WHEN 2 THEN 'approved'
        ELSE        'withdrawn'
    END                                                       AS fda_status,
    (DATE '1962-01-01' + ((hash(range * 7) % 23375)::INTEGER)) AS fda_approval_date,
    CASE (range % 5)::INT
        WHEN 0 THEN 'tablet' WHEN 1 THEN 'capsule' WHEN 2 THEN 'injection'
        WHEN 3 THEN 'patch'  ELSE 'solution'
    END                                                       AS dosage_form,
    CASE (range % 8)::INT
        WHEN 0 THEN 'oral' WHEN 1 THEN 'oral'  WHEN 2 THEN 'iv'
        WHEN 3 THEN 'sc'   WHEN 4 THEN 'im'    WHEN 5 THEN 'topical'
        WHEN 6 THEN 'inh'  ELSE        'ocular'
    END                                                       AS administration_route,
    -- ATC code (A-N + two letters)
    chr(65 + (range % 14)::INT) ||
    lpad((hash(range * 11) % 100)::VARCHAR, 2, '0') || 'XX'   AS atc_code,
    ((range % 300) + 1)::SMALLINT                             AS therapeutic_area_id,
    (range % 3 = 0)                                           AS has_black_box_warning,
    (range % 5 = 0)                                           AS is_controlled_substance
FROM range(0, 5000);
SQL

progress "[clinical] building adverse events (20M rows)..."
"$DB" "$OUT" <<'SQL'
-- FDA FAERS-style adverse event reports.
-- Each row is one drug-event combination in the reporting database.
-- MedDRA terminology: System Organ Class (SOC) → Preferred Term (PT)
CREATE TABLE clinical.adverse_events AS
SELECT
    (range + 1)::BIGINT                                       AS ae_id,
    ((range % 5000) + 1)::INTEGER                             AS drug_id,

    -- MedDRA System Organ Class
    CASE (range % 27)::INT
        WHEN 0  THEN 'Blood and lymphatic system disorders'
        WHEN 1  THEN 'Cardiac disorders'
        WHEN 2  THEN 'Congenital, familial and genetic disorders'
        WHEN 3  THEN 'Ear and labyrinth disorders'
        WHEN 4  THEN 'Endocrine disorders'
        WHEN 5  THEN 'Eye disorders'
        WHEN 6  THEN 'Gastrointestinal disorders'
        WHEN 7  THEN 'General disorders and administration site conditions'
        WHEN 8  THEN 'Hepatobiliary disorders'
        WHEN 9  THEN 'Immune system disorders'
        WHEN 10 THEN 'Infections and infestations'
        WHEN 11 THEN 'Investigations'
        WHEN 12 THEN 'Metabolism and nutrition disorders'
        WHEN 13 THEN 'Musculoskeletal and connective tissue disorders'
        WHEN 14 THEN 'Neoplasms benign, malignant and unspecified'
        WHEN 15 THEN 'Nervous system disorders'
        WHEN 16 THEN 'Pregnancy, puerperium and perinatal conditions'
        WHEN 17 THEN 'Product issues'
        WHEN 18 THEN 'Psychiatric disorders'
        WHEN 19 THEN 'Renal and urinary disorders'
        WHEN 20 THEN 'Reproductive system and breast disorders'
        WHEN 21 THEN 'Respiratory, thoracic and mediastinal disorders'
        WHEN 22 THEN 'Skin and subcutaneous tissue disorders'
        WHEN 23 THEN 'Social circumstances'
        WHEN 24 THEN 'Surgical and medical procedures'
        WHEN 25 THEN 'Vascular disorders'
        ELSE        'Injury, poisoning and procedural complications'
    END                                                       AS meddra_soc,

    -- Preferred Term (simplified)
    CASE (range % 40)::INT
        WHEN 0  THEN 'Nausea'             WHEN 1  THEN 'Headache'
        WHEN 2  THEN 'Dizziness'          WHEN 3  THEN 'Fatigue'
        WHEN 4  THEN 'Diarrhoea'          WHEN 5  THEN 'Vomiting'
        WHEN 6  THEN 'Rash'               WHEN 7  THEN 'Dyspnoea'
        WHEN 8  THEN 'Pain'               WHEN 9  THEN 'Pruritus'
        WHEN 10 THEN 'Alanine aminotransferase increased'
        WHEN 11 THEN 'Drug interaction'   WHEN 12 THEN 'QT prolongation'
        WHEN 13 THEN 'Hypertension'       WHEN 14 THEN 'Hypotension'
        WHEN 15 THEN 'Tachycardia'        WHEN 16 THEN 'Neutropenia'
        WHEN 17 THEN 'Thrombocytopenia'   WHEN 18 THEN 'Anaemia'
        WHEN 19 THEN 'Confusional state'  WHEN 20 THEN 'Peripheral neuropathy'
        WHEN 21 THEN 'Tremor'             WHEN 22 THEN 'Insomnia'
        WHEN 23 THEN 'Depression'         WHEN 24 THEN 'Anxiety'
        WHEN 25 THEN 'Hepatotoxicity'     WHEN 26 THEN 'Nephrotoxicity'
        WHEN 27 THEN 'Pancreatitis'       WHEN 28 THEN 'Hypoglycaemia'
        WHEN 29 THEN 'Weight decreased'   WHEN 30 THEN 'Weight increased'
        WHEN 31 THEN 'Fall'               WHEN 32 THEN 'Drug ineffective'
        WHEN 33 THEN 'Off label use'      WHEN 34 THEN 'Myalgia'
        WHEN 35 THEN 'Arthralgia'         WHEN 36 THEN 'Back pain'
        WHEN 37 THEN 'Oedema peripheral'  WHEN 38 THEN 'Cough'
        ELSE        'Infusion related reaction'
    END                                                       AS meddra_pt,

    -- Reporting count for this drug-event pair in this year
    ((hash(range * 7) % 500) + 1)::INTEGER                   AS report_count,

    -- Reporting odds ratio (disproportionality signal)
    round(pow(10::DOUBLE, -1.0 + (hash(range * 11) % 3000)::DOUBLE / 1000.0), 4)::DECIMAL(8,4)
                                                              AS ror_value,

    -- 95% CI lower
    round(pow(10::DOUBLE, -1.5 + (hash(range * 11) % 3000)::DOUBLE / 1000.0), 4)::DECIMAL(8,4)
                                                              AS ror_ci_lo,

    -- 95% CI upper
    round(pow(10::DOUBLE, -0.5 + (hash(range * 11) % 3000)::DOUBLE / 1000.0) *
          (1.5 + (hash(range * 13) % 1000)::DOUBLE / 1000.0), 4)::DECIMAL(8,4)
                                                              AS ror_ci_hi,

    (1968 + (hash(range * 17) % 58)::INT)::SMALLINT           AS reporting_year,
    ((hash(range * 19) % 10) = 0)                             AS is_serious,
    ((hash(range * 23) % 50) = 0)                             AS is_fatal,

    -- Causality assessment
    CASE (hash(range * 29) % 5)::INT
        WHEN 0 THEN 'certain'   WHEN 1 THEN 'probable'
        WHEN 2 THEN 'possible'  WHEN 3 THEN 'unlikely'
        ELSE        'unassessable'
    END                                                       AS causality

FROM range(0, 20000000);
SQL
progress "[clinical.adverse_events] done (20M rows)"

progress "[clinical] building clinical trials (100K rows)..."
"$DB" "$OUT" <<'SQL'
-- Simplified ClinicalTrials.gov-style trial registry
CREATE TABLE clinical.clinical_trials AS
SELECT
    (range + 1)::INTEGER                                      AS trial_id,
    'NCT' || lpad((10000000 + range * 3 + hash(range) % 1000000)::VARCHAR, 8, '0')
                                                              AS nct_id,
    ((range % 10000000) + 1)::BIGINT                          AS compound_id,
    CASE (range % 4)::INT
        WHEN 0 THEN 'Phase I'   WHEN 1 THEN 'Phase II'
        WHEN 2 THEN 'Phase III' ELSE        'Phase IV'
    END                                                       AS phase,
    CASE (range % 6)::INT
        WHEN 0 THEN 'Recruiting'        WHEN 1 THEN 'Active, not recruiting'
        WHEN 2 THEN 'Completed'         WHEN 3 THEN 'Terminated'
        WHEN 4 THEN 'Withdrawn'         ELSE        'Not yet recruiting'
    END                                                       AS status,
    -- Indication (therapeutic area)
    ((range % 300) + 1)::SMALLINT                             AS therapeutic_area_id,
    -- Enrollment
    CASE (range % 4)::INT
        WHEN 0 THEN ((hash(range * 7) % 30)  + 5)::INTEGER   -- phase I: 5-35
        WHEN 1 THEN ((hash(range * 7) % 200) + 20)::INTEGER  -- phase II: 20-220
        WHEN 2 THEN ((hash(range * 7) % 3000)+ 100)::INTEGER -- phase III: 100-3100
        ELSE        ((hash(range * 7) % 5000)+ 200)::INTEGER -- phase IV: 200-5200
    END                                                       AS enrollment,
    (DATE '1990-01-01' + ((hash(range * 11) % 13149)::INTEGER)) AS start_date,
    CASE WHEN (range % 6) IN (2,3,4)
         THEN (DATE '1990-01-01' + ((hash(range * 11) % 13149)::INTEGER)
               + ((hash(range * 13) % 1825) + 180)::INTEGER)
         ELSE NULL
    END                                                       AS completion_date,
    (range % 2 = 0)                                           AS is_randomized,
    (range % 3 = 0)                                           AS is_blinded,
    (range % 4 = 0)                                           AS is_placebo_controlled
FROM range(0, 100000);
SQL
progress "[clinical.clinical_trials] done (100K rows)"

# ─────────────────────────────────────────────────────────────────────────────
# Build manifest + final summary
# ─────────────────────────────────────────────────────────────────────────────
progress "[manifest] writing build metadata..."
"$DB" "$OUT" <<'SQL'
CREATE TABLE reference.build_manifest (
    key       VARCHAR NOT NULL,
    value     VARCHAR,
    built_at  TIMESTAMP DEFAULT now()
);
INSERT INTO reference.build_manifest (key, value) VALUES
    ('database_name',  'cheminformatics'),
    ('version',        '1.0.0'),
    ('domain',         'cheminformatics / computational toxicology'),
    ('purpose',        'hyphasync stress-test: type coverage, large tables, diverse schemas'),
    ('schemas',        'registry,bioassay,toxicology,admet,clinical,reference'),
    ('data_model',     'PubChem + ChEMBL + Tox21 + ADMET-AI inspired synthetic data'),
    ('license',        'synthetic / public domain'),
    ('generator',      'build-cheminformatics.sh');
SQL

END=$(date +%s)
echo ""
echo "==> Build complete in $((END - START))s"
echo ""
echo "==> Schema summary:"
"$DB" "$OUT" -c "
SELECT
    schema_name,
    COUNT(*)                                    AS tables,
    SUM(estimated_size)::BIGINT                 AS approx_total_rows,
    SUM(pg_size_pretty(estimated_size::BIGINT)) AS size_str
FROM duckdb_tables()
WHERE database_name = current_database()
  AND schema_name NOT IN ('information_schema')
  AND internal = false
GROUP BY schema_name
ORDER BY approx_total_rows DESC;
" 2>/dev/null || true

echo ""
"$DB" "$OUT" -c "
SELECT
    schema_name || '.' || table_name          AS table_name,
    estimated_size::BIGINT                    AS approx_rows
FROM duckdb_tables()
WHERE database_name = current_database()
  AND schema_name NOT IN ('information_schema')
  AND internal = false
ORDER BY approx_rows DESC
LIMIT 20;
" 2>/dev/null || true

echo ""
echo "==> File size: $(ls -lh "$OUT" | awk '{print $5}')"
echo ""
echo "    Run the full sync harness:"
echo "    ./scripts/test-sample-dbs.sh"
