# 22 — Coverage Analysis

## Status

```text
COVERAGE_ANALYSIS=PLANNED
COVERAGE_SCIENCE=NOT_YET_FROZEN
```

## Authority

`docs/product/product_definition.md` defines the product need; a separate versioned scientific contract is required before implementation.

## REQUIRED_PRODUCT_TARGET

Minimum classification:

```text
UNKNOWN
UNSEEN
WEAK
ADEQUATE
```

Candidate evidence may include observation/view count, distinct camera count, angular diversity, parallax, incidence angle, camera distance, projected resolution, image quality, Feature/Track support, reprojection quality, triangulation quality, visibility/occlusion, dense/mesh evidence, hole/boundary evidence and campaign provenance.

Coverage may operate on the complete meaningful reconstructed target surface or an explicit user-selected region of interest. A global `complete` claim is invalid when no meaningful target surface/ROI exists.

Sparse-only coverage analysis is allowed only with an explicit lower-confidence/support boundary. Dense/mesh evidence may strengthen visibility and hole reasoning but must not retroactively falsify sparse uncertainty.

## STOP

Thresholds, weights, confidence computation and classification rules are science. Freeze and validate them before implementation.

Never fabricate a closed surface from sparse points merely to claim that an area is covered.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
