# 12 — Calibration

## Status

```text
CALIBRATION_SCIENCE_V1=PASS/FROZEN
CALIBRATION_TOOLING_V1=PASS/FROZEN
CALIBRATION_BOOTSTRAP_V1=PASS/FROZEN
CALIBRATION_WORKFLOW=PLANNED
```

## Authority

`docs/architecture/calibration_science_v1.md`, `docs/architecture/calibration_bootstrap.md` and `docs/architecture/calibration_solver_preflight_v1.md` are the current specialized calibration documents.

At this checkpoint there is no `docs/architecture/calibration_tooling.md`. Calibration Tooling v1 is nevertheless an acquired PASS/FROZEN implementation boundary. For its exact current API/constant behavior, inspect the executable/public implementation authority, beginning with `include/lardon3d/calibration_tooling.h`, together with the lifecycle declarations in `README.md`, `docs/roadmap/roadmap.md` and `docs/product/product_definition.md`. Do not invent a missing documentation file or treat this prompt file as a replacement architecture specification.

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
-> external OpenCV 5.x solver
-> Calibration Tooling v1
-> deterministic L3DCALB1 v1
-> Calibration Bootstrap v1
-> exact optical assignment
-> READY
-> real Sparse SfM
```

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
