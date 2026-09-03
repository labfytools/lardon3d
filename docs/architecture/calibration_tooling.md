# Calibration Tooling v1

## Status

```text
CALIBRATION_TOOLING_V1=PASS/FROZEN
CALIBRATION_TOOLING_PLANARITY_ALIGNMENT=PASS/FROZEN
L3DCALB1_VERSION=1
```

## Authority

Calibration Science v1 is the scientific authority. This document specializes
the bounded operational bridge implemented by
`include/lardon3d/calibration_tooling.h` and `src/calibration_tooling.c`.

The bridge connects:

```text
Calibration Science v1 evidence
-> Calibration Tooling v1
-> L3DCALB1 v1
-> Calibration Bootstrap v1
```

It does not acquire images, run a solver, infer optical state, create
reconstruction results, add a Project DB schema version, add a Task kind, or
start Sparse SfM.

## Planarity alignment

Calibration Science v1 requires a rigid planar physical target and rejects a
warped board. It defines no numeric target-flatness tolerance.

The canonical external session records categorical physical evidence:

```text
planarity PASS <planarity-evidence-sha256>
```

The Science v1 value `0.20 mm` applies to the allowed range of the ten measured
30.000 mm squares. It is not a target-flatness threshold.

`Lardon3DCalibrationToolingEvidence.target_flatness_mm` is retained only to
preserve the existing public structure layout. In v1 it MUST be IEEE-754 NaN.
Any finite value is rejected so a caller cannot invent a physical measurement
or silently create a new scientific threshold.

The immutable session containing the categorical planarity attestation is bound
by the higher-level workflow through `initialization_evidence_sha256`.

This corrective alignment changes neither Calibration Science v1 nor
`L3DCALB1` v1 nor Calibration Bootstrap v1.

## Bounded evidence

Tooling continues to validate the frozen Science v1 requirements, including:

- exact ChArUco 9 x 7 / DICT_5X5_100 target identity;
- 30.000 mm squares and 21.000 mm markers;
- ten physical square measurements and instrument resolution;
- measured free white border of at least 30 mm, supplied explicitly by the session;
- immutable target, optical-state, solver and evidence digests;
- accepted and rejected view evidence;
- field-region, distance and angle diversity;
- corner quality, clipping, residual and hold-out evidence;
- deterministic repeated parameters;
- representative coordinate equivalence;
- exact selected-image order and representation SHA-256;
- the exact eight-parameter pinhole model;
- zero active extra distortion coefficients.

Successful solver exit alone is never sufficient acceptance.

## Artifact and Bootstrap

For valid evidence Tooling produces exactly the existing fixed-width,
little-endian `L3DCALB1` v1 artifact and may invoke only the frozen Calibration
Bootstrap importer.

A failed validation reaches neither artifact publication nor Project DB
mutation. Exact successful retries converge through the existing immutable
Bootstrap contract.

## Current next boundary

The external Calibration Evidence Solver v1 is implemented and validated.

The missing product boundary is the higher-level calibration workflow
coordinator. It must consume:

```text
session.l3dcal
session.l3dcal.bundle/detection.json
session.l3dcal.bundle/solve.json
session.l3dcal.bundle/evidence.json
session.l3dcal.bundle/producer.json
```

together with the exact selected execution and optical state, then construct
the bounded `Lardon3DCalibrationToolingEvidence`.

`producer.json` binds the exact solver executable SHA-256, canonical solver-configuration SHA-256, exact session SHA-256, OpenCV build identity, CPU1 policy and optical-state SHA-256.

The coordinator must verify regular bounded files, immutable digests,
session/bundle identity, categorical planarity PASS, selected-execution image
binding and optical-state equality. It must never manufacture missing evidence.
