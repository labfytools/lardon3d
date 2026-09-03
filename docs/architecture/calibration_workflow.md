# Calibration Workflow

## Status

```text
CALIBRATION_WORKFLOW=IN_PROGRESS
CALIBRATION_WORKFLOW_INPUT_BOUNDARY_V1=PASS/FROZEN
CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION_V1=PASS/FROZEN
CURRENT_WORKFLOW_NEXT=SELECTED_EXECUTION_BINDING_V1
```

## Authority

Calibration Science v1, Calibration Tooling v1 and Calibration Bootstrap v1
remain FROZEN scientific and import authorities.

This workflow is bounded orchestration only. It does not introduce a solver,
Project DB schema version, Task kind, Sparse SfM execution or reconstructed
scientific evidence.

## Frozen flow

```text
physical calibration acquisition
-> session.l3dcal
-> external Calibration Evidence Solver v1
-> immutable solver bundle
-> campaign-state evidence
-> Calibration Workflow
-> Calibration Tooling v1
-> L3DCALB1 v1
-> Calibration Bootstrap v1
-> selected execution READY
```

## Input Boundary v1

`CALIBRATION_WORKFLOW_INPUT_BOUNDARY_V1=PASS/FROZEN`.

The implementation is exposed through:

```text
include/lardon3d/calibration_workflow.h
src/calibration_workflow.cpp
```

Input Boundary v1 performs no Project DB mutation.

It accepts only bounded regular files and rejects special files and symlinks
before potentially blocking reads. File access follows the nonblocking,
close-on-exec regular-file discipline.

The bounded input set is:

```text
session.l3dcal
session.l3dcal.bundle/detection.json
session.l3dcal.bundle/solve.json
session.l3dcal.bundle/evidence.json
session.l3dcal.bundle/producer.json
L3DCAL_CAMPAIGN_STATE_V1
```

It verifies:

- regular bounded files;
- SHA-256 identities;
- strict session syntax;
- structurally valid canonical JSON bundle members;
- exact session SHA binding through `producer.json`;
- decoder/version consistency;
- exact optical-state SHA equality;
- exact optical-state token equality;
- campaign-state identity consistency.

Malformed JSON, oversize files, symlinks, FIFOs, session digest mismatch and
optical-state mismatch are rejected.

Input Boundary v1 does not:

- open or mutate Project DB;
- construct `Lardon3DCalibrationToolingEvidence`;
- call Calibration Tooling;
- produce `L3DCALB1`;
- invoke Calibration Bootstrap;
- change selected-execution state.

## Evidence Materialization v1

`CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION_V1=PASS/FROZEN`.

The implementation is additive:

```text
src/calibration_workflow_materialize.cpp
tests/test_calibration_workflow_materialize.cpp
```

It consumes only inputs that first pass Input Boundary v1 and performs no
Project DB access or mutation.

The caller owns bounded arrays for materialized views and coordinate checks.
On success the output borrows those arrays and retains:

- exact target generator SHA-256 and physical target measurements;
- measured white border and categorical planarity evidence;
- exact optical-state SHA-256;
- exact solver executable and configuration SHA-256;
- exact accepted/rejected per-view classifications and rejection reasons;
- hold-out assignment, frame region, distance band and target coverage;
- retained per-view residual counts and metrics;
- coordinate-equivalence checks derived from the retained session points;
- all three exact full-solve parameter vectors;
- exact fit parameter vector;
- support image/observation counts;
- global RMSE, maximum residual and high-residual fraction;
- hold-out RMSE and maximum residual;
- maximum parameter delta and `validation_flags=0x0f`.

The stage consumes published solver evidence; it does not reclassify views,
rerun calibration, average repeated solves or manufacture missing values.

`initialization_evidence_sha256` is the exact `session.l3dcal` SHA-256.

`validation_evidence_sha256` is deterministic and domain separated:

```text
SHA256(
  ASCII("L3DCAL_WORKFLOW_VALIDATION_V1\n")
  || detection_sha256_raw32
  || solve_sha256_raw32
  || evidence_sha256_raw32
  || producer_sha256_raw32
)
```

This boundary deliberately does not construct per-campaign
`Lardon3DCalibrationToolingEntry` rows. Those rows require Project DB proof of
the selected image identities, representation bytes/dimensions and explicit
Capture optical assignments, which belongs to Selected Execution Binding v1.

## Campaign optical-state evidence

Project DB v23 retains exact explicit optical configuration identity, including
body, objective and focal state, but Calibration Science v1 requires a broader
scientific key including focus, stabilization and processing/decode state.

No equality may be inferred between those domains.

`L3DCAL_CAMPAIGN_STATE_V1` therefore provides immutable external evidence for
the complete Science v1 optical state. A later coordinator stage must verify
this evidence against both the calibration session and each selected Capture's
explicit Project DB optical configuration.

Absence or disagreement remains `CALIBRATION_UNAVAILABLE`.

## Current next boundary

```text
CALIBRATION_WORKFLOW_SELECTED_EXECUTION_BINDING_V1
```

The next stage may read Project DB, but must not mutate it. It must:

- load the exact selected execution and require the calibration/ready stage
  appropriate to the existing FROZEN contract;
- require exact selected item count and order;
- match every campaign-state Capture row to the selected item;
- load each Capture's explicit v23 optical assignment and require the exact
  declared optical configuration;
- load every selected image/asset identity;
- read the exact managed representation as a bounded regular file;
- require asset byte size and SHA-256 equality;
- decode the exact geometric representation and require oriented dimensions
  compatible with the calibration evidence;
- construct the per-image `Lardon3DCalibrationToolingEntry` rows without
  changing any scientific value.

Only after that read-only binding passes may a final workflow boundary invoke
the FROZEN Tooling/Bootstrap path and transition the selected execution to
truthful `READY`.
