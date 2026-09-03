# 15 — Dense / MVS

## Status

```text
MVS_M1=PASS/FROZEN
DURABLE_DENSE_EXECUTION=PLANNED
```

## Authority

`docs/architecture/reconstruction_pipeline.md` and the canonical MVS-M1 boundary documented there.

## FROZEN

OpenMVS is the first dense backend target. MVS-M1 defines the existing external OpenMVS v2.4.0 boundary.

Do not begin by inventing a generic backend framework.

## REQUIRED_PRODUCT_TARGET

Dense execution must become:

- durable;
- restartable;
- Resource-Governor controlled;
- scratch-aware;
- bounded;
- cancellable;
- failure-atomic;
- inspectable in the TUI.

Validate produced assets before publication. A zero external-process exit code is not sufficient proof.

Dense failure must never invalidate the source Sparse Reconstruction.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
