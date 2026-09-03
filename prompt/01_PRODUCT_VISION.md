# 01 — Product Vision

## Status

```text
PRODUCT_VISION=FROZEN_TARGET
```

## Authority

`docs/product/product_definition.md` is the product authority.

## REQUIRED_PRODUCT_TARGET

Lardon3D is a persistent, incremental, resource-aware Linux photogrammetry system controlled primarily by an ncurses TUI, with a required companion graphical viewer.

The product must let a user:

- create and reopen durable projects;
- ingest traceable still, future live-device, and video-derived captures;
- manage camera/lens/optical configurations without code changes for ordinary equipment;
- reach explicit calibration readiness;
- execute the existing scientific pre-SfM and Sparse SfM pipeline without invented science;
- produce durable Dense/MVS, mesh, refinement, texturing and exports;
- inspect sparse/dense/mesh/texture and camera evidence;
- analyze coverage;
- receive actionable supplementary-capture guidance;
- use live A6000 HDMI and S21 acquisition adapters without forking the scientific pipeline;
- restart after crashes/reboots without hidden transient knowledge.

## Principles

```text
SCIENTIFIC_TRACEABILITY=REQUIRED
DETERMINISM=REQUIRED
PERSISTENT_PROGRESS=REQUIRED
ATOMIC_PUBLICATION=REQUIRED
BOUNDED_EXECUTION=REQUIRED
MAXIMUM_SAFE_USEFUL_THROUGHPUT=REQUIRED
SERIALISM_REQUIRES_PROOF=REQUIRED
NO_SILENT_SCIENTIFIC_SUBSTITUTION=REQUIRED
NO_DESTRUCTIVE_AUTOMATION=REQUIRED
```

The system must expose uncertainty rather than replace it with guesses.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
