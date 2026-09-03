# 31 — Implementation Order

## Status

```text
IMPLEMENTATION_ORDER=DEPENDENCY_DRIVEN
IMPLEMENTATION_AUTHORIZATION=NO
STEP_0_USER_FACING_LANGUAGE_NORMALIZATION=PASS
CURRENT_NEXT=CALIBRATION_V2_HETEROGENEOUS_CALIBRATION_PUBLICATION
```

## Authority

`docs/product/product_definition.md`, current roadmap and dependencies confirmed on current `main`.

## PLANNED ORDER

Implementation remains unauthorized until the human explicitly authorizes a tranche.

Default dependency order:

0. user-facing repository/UI language normalization where appropriate — PASS;
1. final usable Calibration Science/Tooling/Bootstrap/Workflow v1 compatibility path — PASS/FROZEN;
2. Calibration Science v2 design for heterogeneous cameras/lenses/focals, adaptive capture settings and autofocus — PASS/FROZEN;
3. Calibration v2 heterogeneous-optics persistence foundation — PASS/FROZEN;
4. Heterogeneous calibration publication / Tooling / Bootstrap evolution — CURRENT;
5. physical autofocus/optical applicability validation and dedicated calibrated real campaign;
6. real Sparse SfM proof;
7. durable Dense/OpenMVS orchestration;
8. mesh / refinement / texturing / export;
9. viewer foundation;
10. offline Coverage Analysis scientific contract and implementation;
11. multi-campaign registration / fusion;
12. generic live acquisition adapter foundation;
13. A6000 HDMI integration;
14. S21 integration;
15. live camera localization;
16. live coverage overlay;
17. actionable Capture Guidance;
18. video ingestion / deterministic keyframes;
19. final integration, UX, restart and performance proof;
20. Product Definition v1 Definition-of-Done closure.

## Adjustment rule

Change this order only when current repository inspection proves a real dependency difference. Preserve product intent and document the dependency.

Step 0 closure does not authorize mechanical rewriting of historical persisted labels, path names, deliberate UTF-8 fixtures or persistence-sensitive internal strings. Such text is changed only when its owning scope requires it.

Do not jump to A6000 live work merely because it is visually interesting. Reconstruction/viewer/coverage foundations required for truthful guidance come first.

The calibration-v2 insertion before the physical campaign is dependency-driven,
not feature creep: real acquisition evidence established that the v1 locked-focus
single-optical-state path would make normal engine-bay capture impractical and
would contradict the already-FROZEN product requirement for multiple optical
configurations per project. Science v2 must be defined before collecting the
physical evidence intended to validate it.
