# 12 — Calibration

## Status

```text
CALIBRATION_SCIENCE_V1=PASS/FROZEN
CALIBRATION_TOOLING_V1=PASS/FROZEN
CALIBRATION_TOOLING_PLANARITY_ALIGNMENT=PASS/FROZEN
CALIBRATION_BOOTSTRAP_V1=PASS/FROZEN
CALIBRATION_EVIDENCE_SOLVER_V1=IMPLEMENTED/VALIDATED
CALIBRATION_SOLVER_WHITE_BORDER_V1=PASS/FROZEN
CALIBRATION_SOLVER_PRODUCER_IDENTITY_V1=PASS/FROZEN
CALIBRATION_SOLVER_PER_VIEW_EVIDENCE_V1=PASS/FROZEN
CALIBRATION_SOLVER_BUNDLE_REPAIR_V1=PASS/FROZEN
CALIBRATION_WORKFLOW=IN_PROGRESS
CURRENT_CALIBRATION_NEXT=WORKFLOW_COORDINATOR
```

## Authority

`docs/architecture/calibration_science_v1.md`, `docs/architecture/calibration_bootstrap.md` and `docs/architecture/calibration_solver_preflight_v1.md` are the current specialized calibration documents.

`docs/architecture/calibration_tooling.md` is the specialized Tooling authority. The public API remains `include/lardon3d/calibration_tooling.h`.

A bounded corrective review established that Calibration Science v1 defines target planarity as a categorical physical attestation, not a numeric flatness tolerance. Tooling preserves its public structure layout while requiring `target_flatness_mm` to be NaN, so callers cannot invent a millimetre measurement. The canonical session's `planarity PASS <sha256>` evidence is bound through immutable initialization evidence.

The external `tools/calibration_evidence_solver/` implementation is present and validated by its deterministic synthetic CPU1 self-test. It remains external to the Lardon3D runtime and Project DB.

Its session v1 now requires an explicit measured `white_border` of at least 30 mm. No default border width may be invented by the solver or the future coordinator.

## CURRENT

Historical S21 and A6000 Engine Bay campaigns:

```text
CALIBRATION_UNAVAILABLE
BLOCKED_BY_KNOWN_CALIBRATION_DATA
```

Do not retro-calibrate them by invention.

## FROZEN flow

```text
dedicated physical calibration acquisition
-> external OpenCV 5.x Calibration Evidence Solver v1
-> immutable session manifest + complete solver bundle
-> workflow coordinator
-> Calibration Tooling v1
-> deterministic L3DCALB1 v1
-> Calibration Bootstrap v1
-> exact selected-execution calibration scope attachment
-> READY
-> real Sparse SfM
```

The current implementation gap is the workflow coordinator. It consumes the immutable `session.l3dcal` plus `detection.json`, `solve.json` and `evidence.json`, binds them to the exact selected execution and optical state, constructs the bounded Tooling evidence and never manufactures missing physical evidence.

## REQUIRED_PRODUCT_TARGET

The TUI calibration assistant must expose:

```text
READY
CALIBRATION_REQUIRED
SELECTION_REQUIRED
```

It must guide physical evidence acquisition, exact optical configuration, accepted/rejected evidence and remaining evidence requirements.

Solver exit success is never sufficient acceptance.

## REJECTED

Pseudo-calibration, EXIF calibration, nearby focal substitution, lens substitution, silent interpolation, silent backfill, and silent identity inference.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
