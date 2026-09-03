# 31 — Implementation Order

## Status

```text
IMPLEMENTATION_ORDER=DEPENDENCY_DRIVEN
IMPLEMENTATION_AUTHORIZATION=NO
STEP_0_USER_FACING_LANGUAGE_NORMALIZATION=PASS
CURRENT_NEXT=1_CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION
```

## Authority

`docs/product/product_definition.md`, current roadmap and dependencies confirmed on current `main`.

## PLANNED ORDER

Implementation remains unauthorized until the human explicitly authorizes a tranche.

Default dependency order:

0. user-facing repository/UI language normalization where appropriate — PASS;
1. final usable calibration workflow — IN PROGRESS; Input Boundary v1 PASS/FROZEN; current next sub-boundary: Evidence Materialization v1;
2. dedicated physical calibrated real campaign;
3. real Sparse SfM proof;
4. durable Dense/OpenMVS orchestration;
5. mesh / refinement / texturing / export;
6. viewer foundation;
7. offline Coverage Analysis scientific contract and implementation;
8. multi-campaign registration / fusion;
9. generic live acquisition adapter foundation;
10. A6000 HDMI integration;
11. S21 integration;
12. live camera localization;
13. live coverage overlay;
14. actionable Capture Guidance;
15. video ingestion / deterministic keyframes;
16. final integration, UX, restart and performance proof;
17. Product Definition v1 Definition-of-Done closure.

## Adjustment rule

Change this order only when current repository inspection proves a real dependency difference. Preserve product intent and document the dependency.

Step 0 closure does not authorize mechanical rewriting of historical persisted labels, path names, deliberate UTF-8 fixtures or persistence-sensitive internal strings. Such text is changed only when its owning scope requires it.

Do not jump to A6000 live work merely because it is visually interesting. Reconstruction/viewer/coverage foundations required for truthful guidance come first.
