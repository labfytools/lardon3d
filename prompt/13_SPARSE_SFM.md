# 13 — Sparse SfM

## Status

```text
SPARSE_SFM_V1=IMPLEMENTED
GATE_A=DECISION/HISTORICAL
GATE_B=PASS/FROZEN
GATE_C=PASS/FROZEN
GATE_D=PASS/FROZEN
GATE_E=PASS/FROZEN
GATE_F=PASS/FROZEN
GATE_G=PASS/FROZEN
```

## Authority

`docs/architecture/sparse_sfm.md` is the detailed authority.

## FROZEN

Sparse SfM consumes one exact immutable Track Set and one exact compatible known-calibration scope.

It does not recover unknown intrinsics, infer metric scale, mutate Tracks, or merge unrelated campaigns.

The production Task remains:

```text
sparse_sfm.run/1
```

Its FROZEN CPU1/BATCH1 execution is a specific validated Sparse v1 choice and must not be generalized to unrelated stages.

## CURRENT

```text
REAL_S21_SPARSE_SFM=NOT_EXECUTED
REAL_A6000_SPARSE_SFM=NOT_EXECUTED
REAL_SPARSE_SFM_BLOCKER=KNOWN_CALIBRATION_DATA
```

## PLANNED

Run a dedicated physically calibrated real campaign, reach `READY`, then execute the existing Sparse SfM path as a retained real proof. Do not use historical uncalibrated campaigns to bypass the calibration contract.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
