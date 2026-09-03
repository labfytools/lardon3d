# Calibration Workflow

## Status

```text
CALIBRATION_WORKFLOW=IN_PROGRESS
CALIBRATION_WORKFLOW_INPUT_BOUNDARY_V1=PASS/FROZEN
CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION_V1=PASS/FROZEN
CALIBRATION_WORKFLOW_SELECTED_EXECUTION_BINDING_V1=PASS/FROZEN
CURRENT_WORKFLOW_NEXT=TOOLING_BOOTSTRAP_READY
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

## Selected Execution Binding v1

```text
CALIBRATION_WORKFLOW_SELECTED_EXECUTION_BINDING_V1=PASS/FROZEN
```

The implementation is additive:

```text
src/calibration_workflow_bind.cpp
tests/test_calibration_workflow_bind.cpp
```

This boundary is read-only. It may read Project DB and managed representation
bytes, but does not attach a calibration scope, invoke Calibration Tooling or
Bootstrap, or transition the selected execution to `READY`. It proves:

- exact selected-execution stage, completion, item order and Capture mapping;
- exact explicit v23 optical configuration, including campaign-origin facts
  where present;
- exact selected-image/Capture relation and READY image asset identity;
- managed representation size and SHA-256 through project-relative `openat`
  descent that rejects absolute paths, dot components, symlinks, non-directory
  components and non-regular final files;
- grayscale OpenCV decoded width/height equal to the accepted materialized
  calibration geometry; and
- deterministic selected-item-order `Lardon3DCalibrationToolingEntry`
  construction from the published solve values, without averaging or solver
  recomputation.

Entries are staged internally and published to caller storage only after every
selected item passes. Exact retries are read-only and deterministic.

## Current next boundary

The final calibration workflow boundary is:

```text
validated input -> materialized evidence -> selected-execution binding
-> FROZEN Calibration Tooling -> FROZEN Calibration Bootstrap -> READY
```

It must preserve the binding's exact provenance and use only the FROZEN
Tooling/Bootstrap import path. No failure before import may mutate Project DB.
