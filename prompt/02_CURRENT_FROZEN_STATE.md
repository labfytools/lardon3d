# 02 — Current and Frozen State

## Status

```text
CURRENT_PROJECT_DB_SCHEMA=v25
PRODUCTION_TASK_KINDS=16
USER_FACING_UI_LANGUAGE_NORMALIZATION=PASS
CURRENT_IMPLEMENTATION_CURSOR=1_CALIBRATION_WORKFLOW_COORDINATOR
```

## Authority

`README.md`, `AGENTS.md`, `docs/roadmap/roadmap.md`, specialized architecture documents, and retained checkpoint evidence.

## CURRENT

```text
Project DB head                         v25
Production Task kinds                   16
Feature Store                           IMPLEMENTED
Visual Index                            IMPLEMENTED
Candidate Pair                          IMPLEMENTED
Matcher                                 IMPLEMENTED
TUI operational observatory            VALIDATED
User-facing TUI/control language         PASS
External SSD controller                 VALIDATED
Calibration Solver Preflight v1         PASS
Calibration Evidence Solver v1              IMPLEMENTED/VALIDATED
Calibration solver white-border evidence    PASS/FROZEN
Calibration Tooling planarity alignment     PASS/FROZEN
```

The additive schema lineage is:

```text
v22 selected scientific execution foundation
v23 generic optical-context overlay
v24 raw.develop.batch/1
v25 features.extract.batch/1
```

## FROZEN

Preserve at their documented boundaries:

- Capture / Asset Provenance;
- acquisition ingestion and durable campaign execution;
- Photo Quality Triage;
- Selected Scientific Execution;
- Geometric Verifier v3;
- Track Model / Track Builder;
- Sparse SfM capability through Gate G (detailed lifecycle: Gate A decision/historical; Gates B-G PASS/FROZEN);
- Phase H v1;
- MVS-M1 external OpenMVS 2.4.0 boundary;
- Calibration Science v1;
- Calibration Tooling v1;
- Calibration Bootstrap v1;
- Internal Parallelism / Compute Resources;
- Resource / Compute Governor;
- ORB Vulkan backend;
- Global Maintenance Audit;
- Real S21 Tracks;
- Real A6000 pre-SfM.

`FROZEN` protects meaning and boundary, not necessarily every file byte.

## Retained real S21 checkpoint

```text
REAL_S21_TRACKS=PASS/FROZEN
project=/home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-31/s21-gv-v3
Tracks=912447
Track observations=2495768
Track length min/max=2/42
digest=c30eba192627bf73eaf21ff30d81038d8cc6bbf36a69226f88cdc8c37f7d74a1
Sparse SfM=NOT EXECUTED
Dense/MVS=NOT EXECUTED
```

## Retained real A6000 checkpoint

```text
REAL_A6000_PRE_SFM=PASS/FROZEN
tag=real-a6000-pre-sfm-2026-09-02
project=/home/fy59/Documents/Lardon/.real-pre-sfm-2026-09-01/a6000-pre-sfm-v23-final
Project DB=v25
Selected images=689
Feature Sets=689
Candidate Pairs=38420
Match Results=38420
Applicable GVRs=37805
Verified GVRs=10952
Rejected GVRs=26853
Track Sets=1
Tracks=130714
Track observations=318944
Sparse SfM Tasks=0
Sparse Reconstructions=0
Dense/MVS=0
GV v3 fingerprint=6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

The project-directory name is historical and must not be mistaken for schema head.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
