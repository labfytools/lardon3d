# Track Model v1

## Status

```text
TRACK_MODEL_V1=FROZEN
TRACK_BUILDER_V1=PASS/FROZEN

CURRENT_PRODUCTION_VERIFIER=FUNDAMENTAL_V3
SPARSE_SFM_CAPABILITY=IMPLEMENTED_THROUGH_GATE_G
REAL_S21_SPARSE_SFM=NOT_EXECUTED
REAL_A6000_SPARSE_SFM=NOT_EXECUTED

REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Track Model v1 is the persistent scientific contract for coherent multi-view 2D observation sets.

A Track is **not** a 3D point.

It contains no camera pose, triangulated coordinate, reprojection error or Bundle Adjustment state.

Track Builder v1 constructs Tracks from verified Geometric Verification Results. Sparse SfM consumes an
immutable Track Set later.

## Pipeline position

Current pipeline:

```text
Feature Set
-> Candidate Pair
-> Match Result
-> Geometric Verifier v3
-> Geometric Verification Result
-> Track Builder v1
-> Track Model v1
-> Sparse SfM capability
```

Sparse SfM Gates C through G are implemented and frozen.

The retained S21 and A6000 historical campaigns stop before real Sparse SfM because known calibration
data is unavailable for those campaigns.

Older Track Model text that called Sparse SfM "future" describes historical lifecycle, not current
implementation status.

## Track definition

A Track is a coherent set of 2D observations believed to correspond to the same physical scene point
across multiple images.

Observation identity is exactly:

```text
(feature_set_id, feature_index)
```

`feature_set_id` identifies one immutable Feature Set.

`feature_index` is the zero-based keypoint ordinal inside that immutable Feature File.

The Feature Set directly owns `image_id`; image identity is therefore derivable and is not duplicated
in Track observation identity.

## Scientific input

Track Builder consumes only completed verified geometric results selected by one exact verifier
identity.

Current production lineage:

```text
verifier_kind = FUNDAMENTAL
verifier_version = 3
verifier_fingerprint =
6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

Historical Track Sets created from Fundamental verifier v1 or v2 remain valid historical scientific
objects.

They must not be relabelled as v3.

For each selected `GEOMETRIC_VERIFIED` result, only Match File entries whose corresponding inlier-mask
bit is one contribute observation edges.

A rejected GVR contributes no Track edge.

## Verification selector

A Track Set stores the exact verifier selector:

```text
verifier_kind
verifier_version
verifier_fingerprint
```

The builder never selects verification evidence using:

- timestamp;
- "latest";
- insertion order;
- approximate fingerprint match.

The current default producer is v3, but the Track Model remains version-independent and can store valid
sets from explicitly supported historical selectors.

## Input scope

A Track Set also records the exact consumed GVR scope.

Canonical scope identity uses:

```text
domain: L3DTSIS1
items: geometric_verification_result_id
order: strictly increasing
encoding: uint64 little-endian
digest: SHA-256
```

Conceptually:

```text
SHA-256(L3DTSIS1 || id_0 || id_1 || ... || id_N)
```

The scope is Project-DB-local because SQLite GVR IDs participate directly.

`gvr_count` is retained as validation metadata.

An empty scope is invalid.

## Track membership invariants

### Minimum size

A Track has at least two observations.

### At most one observation per image

One Track cannot contain two observations derived from the same image.

This is validated through `feature_sets.image_id`.

### Observation uniqueness inside one Track Set

Within one Track Set:

```text
(feature_set_id, feature_index)
```

belongs to at most one Track.

The persistence schema enforces this using the Track Set-scoped primary key.

### Feature Set existence

Every referenced Feature Set must exist.

### Feature index bound

For every observation:

```text
0 <= feature_index < feature_count
```

The upper bound is validated against the referenced Feature Set.

### Parent consistency

The denormalized Track Set ID carried by an observation must equal the Track Set of its parent Track.

## Track identity

Persistent Track identity is the opaque SQLite:

```text
track_id
```

Track Model v1 does not define a content-derived Track hash.

Reproducibility and reuse are owned by the Track Set identity, builder configuration and exact input
scope.

## Track Set identity

A Track Set is one complete immutable generation.

Its reuse identity contains:

```text
builder_kind
builder_version
builder_parameter_fingerprint
verifier_kind
verifier_version
verifier_fingerprint
input_scope_hash
```

`gvr_count` validates the scope metadata but is not an independent reuse discriminator.

`INSERT OR REPLACE` is forbidden.

An exact existing immutable set is reused.

A scientifically different scope/configuration creates a new Track Set.

## Immutability

A published Track Set is immutable.

No production operation:

- appends to it;
- removes observations;
- merges existing Tracks;
- rewrites memberships;
- updates it to a newer verifier version.

New evidence creates a new generation.

Historical generations remain queryable until explicitly deleted.

## Persistence

Track storage was introduced by Project DB v14.

Durable Track Builder Task payload persistence was added in Project DB v15.

Later schema migrations through v25 do not reinterpret Track Model v1.

Conceptual tables:

```text
track_sets
tracks
track_observations
```

Publication is atomic for the complete Track Set under one transaction.

No Track from that set becomes visible before the complete generation validates and commits.

Rollback leaves no partial Track Set.

## Ordering

Track Builder publishes deterministic canonical order.

`position_in_track` is contiguous from zero.

The exact builder contract owns edge ordering and conflict resolution; Track Model only persists the
validated result.

No hash-table iteration order may define persistent scientific ordering.

## Deletion

Deleting a Track Set cascades to its Tracks and observations.

A Feature Set referenced by a Track observation cannot be silently removed while the reference remains
valid.

Deletion semantics do not mutate other immutable Track Sets.

## Pagination and resource bounds

Track Model storage APIs are paged.

The model does not impose an arbitrary scientific maximum Track length below the number of images that
could legitimately observe the same point.

It does not materialize a dense image-by-image covisibility matrix.

Loading one Track loads that Track's observations; project-wide traversal remains paged.

Execution-memory strategy belongs to Track Builder, not Track Model.

## Corruption handling

A loader returns corruption rather than partial best-effort data when it detects conditions such as:

- missing parent Track or Track Set;
- missing Feature Set;
- duplicate observation in one Track Set;
- repeated image inside one Track;
- out-of-range feature index;
- inconsistent observation count;
- inconsistent Track count;
- inconsistent denormalized Track Set ID;
- invalid/non-contiguous position ordering.

No loader repairs scientific identity in place.

## Provenance

Track Set provenance includes:

```text
builder identity
verifier selector
input scope hash
gvr count
```

Detailed per-edge provenance is not persisted by Track Model v1.

Adding such provenance later requires an explicit version/schema decision if persistent representation
changes.

## Current production verifier lineage

Fundamental verifier v1 and v2 are historical scientific identities.

Current new production verification uses Fundamental v3.

V3 adds bounded preflight rejection before the unchanged scientific estimator path and has its own
fingerprint.

Track Builder consumes only exact matching GVR identities.

Therefore:

```text
HISTORICAL_TRACK_SET_VERIFIER_V1=VALID
HISTORICAL_TRACK_SET_VERIFIER_V2=VALID
CURRENT_TRACK_SET_VERIFIER_V3=PRODUCTION
```

No historical Track Set is upgraded in place.

## Real S21 evidence

The retained S21 Track proof is:

```text
REAL_S21_TRACKS=PASS/FROZEN

Track Set observations = 2,495,768
Tracks                 = 912,447
minimum Track length    = 2
maximum Track length    = 42
mean Track length       = 2.7352470883240341
```

Retained digest:

```text
c30eba192627bf73eaf21ff30d81038d8cc6bbf36a69226f88cdc8c37f7d74a1
```

The compact memory model supersedes the older historical 18.204 GiB envelope.

That checkpoint did not execute real Sparse SfM.

## Real A6000 evidence

The current retained A6000 checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Track output:

```text
Track Set          1
Tracks             130,714
Track observations 318,944
duplicate obs      0
repeated-image     0
orphan obs         0
```

The upstream v3 GV scope contained:

```text
Applicable GVRs 37,805
Verified GVRs   10,952
Rejected GVRs   26,853
```

Restart traversed the retained scope and reused the same Track Set without creating a duplicate
scientific generation.

No Sparse SfM Task or Sparse Reconstruction was created.

## Sparse SfM relationship

Track Model does not perform Sparse SfM.

Sparse SfM capability is nevertheless implemented through Gate G.

Correct current statement:

```text
TRACK_MODEL_OUTPUT=AVAILABLE
SPARSE_SFM_IMPLEMENTATION=AVAILABLE
REAL_HISTORICAL_CAMPAIGN_SPARSE_SFM=BLOCKED_BY_KNOWN_CALIBRATION_DATA
```

These are separate lifecycle facts.

## Out of scope

Track Model v1 does not own:

- Track Builder union/find algorithm;
- Fundamental estimation;
- Essential estimation;
- camera pose;
- triangulation;
- 3D coordinates;
- reprojection error;
- Bundle Adjustment;
- dense reconstruction;
- metric scale;
- Track mutation/merge;
- selection by "latest".

## Summary

```text
TRACK_MODEL_V1=FROZEN
TRACK_BUILDER_V1=PASS/FROZEN

CURRENT_PRODUCTION_VERIFIER=FUNDAMENTAL_V3
CURRENT_VERIFIER_FINGERPRINT=6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c

PROJECT_DB_TRACK_MODEL=v14
PROJECT_DB_TRACK_TASK=v15
CURRENT_PROJECT_DB_SCHEMA=v25

REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN

SPARSE_SFM_CAPABILITY=IMPLEMENTED_THROUGH_GATE_G
REAL_S21_SPARSE_SFM=NOT_EXECUTED
REAL_A6000_SPARSE_SFM=NOT_EXECUTED
```
