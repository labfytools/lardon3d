# Calibration Workflow

## Status

```text
CALIBRATION_WORKFLOW=IN_PROGRESS
CALIBRATION_WORKFLOW_INPUT_BOUNDARY_V1=PASS/FROZEN
CURRENT_WORKFLOW_NEXT=EVIDENCE_MATERIALIZATION_V1
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
CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION_V1
```

The next stage parses the already validated bundle into bounded in-memory
Science v1 evidence:

- exact per-view evidence;
- exact repeated full-solve parameters;
- fit parameters;
- global and hold-out validation metrics;
- coordinate-equivalence evidence;
- immutable provenance digests.

It still performs no Project DB mutation.

Only after that boundary passes may the workflow bind the exact selected
execution, campaign representations and optical assignments and invoke the
FROZEN Tooling/Bootstrap path.
