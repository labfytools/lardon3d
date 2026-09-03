# 03 — End Product

## Status

```text
END_PRODUCT=REQUIRED_PRODUCT_TARGET
```

## Authority

`docs/product/product_definition.md`.

## REQUIRED_PRODUCT_TARGET

The finished product is not complete at Sparse SfM. It is complete only when the durable workflow reaches usable reconstruction, inspection, improvement and export.

Required top-level capabilities:

- data-driven optics onboarding;
- calibration assistant and accepted calibration import;
- real known-calibration Sparse SfM;
- durable restartable Dense/OpenMVS execution;
- mesh, refinement, texturing and consolidation;
- interoperable traceable export;
- passive graphical viewer;
- offline coverage analysis;
- actionable capture guidance;
- live localization and truthful loss/confidence states;
- stock A6000 HDMI integration;
- S21 integration without root;
- deterministic video keyframes;
- safe optional SSD scratch;
- recovery, diagnostics and resource-aware performance;
- local-first operation with no cloud dependency for core workflows;
- explicit preview-before-delete cleanup/storage management.

## Local-first and cleanup requirements

Core project operation, reconstruction, calibration processing, viewer, coverage analysis, capture guidance and export must not require a cloud service. Future optional network/device adapters must make network use explicit; user project imagery is not uploaded merely to operate the product.

Cleanup is explicit and reviewable. The product may identify orphan temporary assets, superseded immutable generations, export caches and stale scratch, but it must show what will be removed before destructive cleanup. FROZEN/historical evidence is never deleted automatically because a newer generation exists.

## REJECTED


The final product does not require:

- a GUI replacement for the TUI;
- camera firmware or hardware modification;
- cloud reconstruction;
- generic backend frameworks without demonstrated need;
- a second scheduler/runtime;
- invented metric scale.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
