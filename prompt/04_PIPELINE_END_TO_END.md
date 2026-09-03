# 04 — End-to-End Pipeline

## Status

```text
PIPELINE_TARGET=REQUIRED
```

## Authority

`docs/product/product_definition.md` plus specialized architecture contracts for each existing stage.

## REQUIRED_PRODUCT_TARGET

```text
project
-> acquisition / Capture / provenance
-> optics assignment
-> calibration readiness
-> quality selection
-> deterministic selected scientific representation
-> Features
-> Visual Index
-> Candidate Pairs
-> Matcher
-> Geometric Verification
-> Tracks
-> Sparse SfM
-> incremental enrichment when lineage permits
-> explicit multi-campaign registration when required
-> Dense / MVS
-> mesh
-> refinement
-> texturing
-> consolidation
-> coverage analysis
-> viewer / capture guidance
-> export
```

Video and live sources enter through acquisition boundaries and converge into the same normal scientific pipeline after durable Capture promotion.

## Invariants

Every scientific stage must consume explicit durable identities and publish atomically. A downstream failure must not invalidate a valid upstream immutable generation.

## Quality boundary

Existing quality recommendations remain explainable and non-destructive:

```text
GOOD
SUSPECT
REJECT
```

Where the FROZEN selection contract permits a human override, that override is explicit, durable and visible and does not rewrite measured quality evidence. Live guidance should reuse the same quality concepts where practical without turning preview evidence into a scientific Capture.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
