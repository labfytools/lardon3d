# Sparse SfM / Triangulation

## Status

```text
SPARSE_SFM_V1=IMPLEMENTED
GATE_A=DECISION/HISTORICAL
GATE_B=PASS/FROZEN
GATE_C=PASS/FROZEN
GATE_D=PASS/FROZEN
GATE_E=PASS/FROZEN
GATE_F=PASS/FROZEN
GATE_G=PASS/FROZEN

REAL_S21_SPARSE_SFM=NOT_EXECUTED
REAL_A6000_SPARSE_SFM=NOT_EXECUTED
REAL_SPARSE_SFM_BLOCKER=KNOWN_CALIBRATION_DATA

CURRENT_PROJECT_DB_SCHEMA=v25
```

Sparse SfM v1 is implemented through Gate G.

The opening Gate A/Gate B design history remains valid historical evidence, but numerical Sparse SfM is
no longer deferred: Gates C through G were implemented, validated and frozen after the original
decision/persistence work.

The retained historical S21 and A6000 campaigns have **not** executed real Sparse SfM. Their current
blocker is the absence of known calibration data that satisfies the frozen calibration contract. This
is a data-eligibility boundary, not a missing Sparse SfM implementation.

Dense/MVS is also unexecuted for the retained A6000 pre-SfM checkpoint.

## Scope

Sparse SfM consumes exactly one immutable complete Track Set plus an explicit immutable known-calibration
scope.

It estimates:

- connected reconstruction components;
- registered camera poses;
- sparse 3D landmarks;
- per-component diagnostics;
- final per-component Bundle Adjustment through the frozen Gate E contract.

It does not mutate:

- Track Model v1;
- Track Builder v1;
- Geometric Verification Results;
- Match Results;
- Feature Sets;
- calibration records.

It does not perform:

- unknown-intrinsics recovery;
- silent EXIF calibration fallback;
- metric scale inference;
- dense matching;
- meshing;
- texturing;
- multi-campaign metric registration.

## Input identity

The scientific input is explicit.

It contains one exact Track Set identity and one exact compatible calibration scope.

Never select inputs by:

```text
latest Track Set
latest calibration
most recent timestamp
path similarity
lens-name guess
EXIF-only guess
```

A disconnected image graph is reconstructed as independent components. No metric or spatial relation
between disconnected components is invented.

## Calibration contract

Sparse SfM v1 is **known-calibration only**.

Every participating image must resolve to an immutable compatible calibration.

Unknown, partially known or silently substituted calibration is rejected.

EXIF focal metadata may help an explicit calibration workflow, but it is not a scientific fallback.

Historical S21 and A6000 evidence remains `CALIBRATION_UNAVAILABLE`; it must not be retroactively
declared calibrated.

The frozen future calibration path is owned by:

- `calibration_science_v1.md`;
- `calibration_tooling.md`;
- `calibration_bootstrap.md`;
- `calibration_solver_preflight_v1.md`.

## Camera model

The v1 camera model is pinhole binary64:

```text
K = [ fx  0  cx ]
    [  0 fy  cy ]
    [  0  0  1  ]
```

with:

```text
fx > 0
fy > 0
0 <= cx < width
0 <= cy < height
```

Skew is zero.

The supported distortion model is OpenCV-compatible:

```text
k1, k2, p1, p2
```

or an explicit zero-distortion calibration.

Higher radial, rational and thin-prism models are outside v1.

The exact distortion model and coefficients are scientific identity.

## Coordinates and pose

Feature File keypoint `x,y` is the canonical observation coordinate.

The source convention is decoded-image pixel coordinates:

```text
origin: top-left
+x: right
+y: down
```

The camera frame is right-handed with:

```text
x right
y down
z forward
```

Pose is world-to-camera:

```text
Xc = R_cw * Xw + t_cw
Cw = -transpose(R_cw) * t_cw
```

The public/persisted representation uses a row-major binary64 rotation matrix plus binary64 translation.

Private quaternion/angle-axis solver representations do not cross the C17 boundary.

## Gauge and scale

Monocular Sparse SfM has a similarity ambiguity.

Each connected component owns its own gauge.

The deterministic seed camera is fixed to:

```text
R = I
t = 0
```

and the second seed establishes a unit arbitrary baseline.

That unit is not millimetres, metres or any physical scale.

Metric scale and global alignment require a separate explicit downstream stage using measured control
information.

No physical scale is inferred from:

- focal pixels;
- image resolution;
- normalized baseline;
- vehicle dimensions;
- device metadata.

## Incremental reconstruction

The frozen strategy is incremental SfM followed by final per-component refinement.

The high-level flow is:

```text
Track Set + known calibration
-> build deterministic covisibility
-> choose valid deterministic seed
-> recover relative pose
-> triangulate supported Tracks
-> register additional cameras with deterministic calibrated PnP
-> grow landmarks
-> stop on bounded graph-growth convergence
-> final per-component Bundle Adjustment
-> publish immutable Sparse Reconstruction
```

Unregisterable images remain scientifically unregistered; they do not become a runtime failure by
themselves.

Disconnected components remain separate.

## Seed selection

Seed candidates come from the Track/covisibility graph rather than raw descriptor score.

Candidates are ordered deterministically by the frozen policy, including shared-track support,
geometric/parallax quality and canonical image IDs.

Pure rotation, negligible baseline, insufficient cheirality and other frozen degeneracy conditions
reject a seed.

A Fundamental matrix is never treated as an Essential matrix. Known intrinsics are used explicitly.

## Camera registration

An unregistered image becomes eligible only from explicit Track-to-landmark correspondences.

Selection is deterministic.

Pose estimation uses the frozen calibrated robust PnP path with bounded attempts and local deterministic
randomness.

Global OpenCV RNG mutation is forbidden.

A failed image remains `UNREGISTERED`.

## Triangulation

Triangulation uses normalized-coordinate linear DLT initialization and the frozen bounded refinement
path.

All valid registered observations of a Track participate.

The source Track is immutable and is not split by Sparse SfM v1.

A landmark is rejected on frozen invalid conditions such as:

- non-finite values;
- invalid depth;
- insufficient parallax;
- ill-conditioning;
- excessive reprojection error.

Track identity remains unchanged.

## Gate lifecycle

### Gate A — historical decision

Gate A established the scientific architecture, known-calibration requirement, camera convention,
gauge, deterministic incremental strategy and triangulation direction.

The original text that said numerical Sparse SfM would be implemented later is historical lifecycle
language.

### Gate B — persistence

Gate B froze the immutable Project DB persistence model and bounded readers.

Project DB v16 is the Sparse Reconstruction persistence foundation.

Later Project DB migrations through v25 are additive and do not reinterpret Sparse SfM identity.

### Gate C — geometry

Gate C implemented and validated the calibrated geometry primitives required by the Sparse SfM core,
including deterministic relative pose, triangulation, calibrated PnP and degeneracy rejection.

Status:

```text
GATE_C=PASS/FROZEN
```

### Gate D — incremental core

Gate D implemented the synchronous in-memory incremental Sparse SfM core.

It consumes explicit immutable inputs and produces deterministic per-component sparse reconstruction
state without persistence side effects inside the scientific core.

Status:

```text
GATE_D=PASS/FROZEN
```

### Gate E — final Bundle Adjustment

Gate E is a synchronous CPU-only final per-component Bundle Adjustment over a private copy of the
final Gate D result.

It never mutates Gate D input.

Known intrinsics/distortion, Track membership and observations remain fixed.

Status:

```text
GATE_E=PASS/FROZEN
```

### Gate F — project/task orchestration

Gate F connected the frozen scientific path to the Project/Task persistence boundary.

The production Task Kind is:

```text
sparse_sfm.run/1
```

The Task restores explicit scientific inputs from its typed payload and recomputes from those immutable
inputs rather than persisting transient solver state.

Status:

```text
GATE_F=PASS/FROZEN
```

### Gate G — Resource Governor integration

Gate G integrated Sparse SfM with the existing Resource Governor.

Its frozen resource contract remains conservative and atomic.

The historical fixed CPU1/batch1 estimate is a property of this frozen Task, not a global rule for
other Task Kinds.

Status:

```text
GATE_G=PASS/FROZEN
```

## Gate E Bundle Adjustment contract

Gate E works independently per reconstructed component.

It resolves observations by the canonical key:

```text
(feature_set_id, feature_index)
```

Resolution must agree with image, Track, source keypoint and calibration identity.

No heuristic fallback is allowed.

The canonical distorted forward projection is:

```text
xn = Xc.x / Xc.z
yn = Xc.y / Xc.z
r2 = xn*xn + yn*yn

radial = 1 + k1*r2 + k2*r2*r2
xd = xn*radial + 2*p1*xn*yn + p2*(r2 + 2*xn*xn)
yd = yn*radial + p1*(r2 + 2*yn*yn) + 2*p2*xn*yn

u = fx*xd + cx
v = fy*yd + cy
```

Residuals are binary64 source-pixel errors.

### BA variables

Gate E optimizes only:

- camera extrinsic rotation;
- camera center;
- landmark position.

It does not optimize:

- `fx`, `fy`, `cx`, `cy`;
- `k1`, `k2`, `p1`, `p2`;
- Track membership;
- observation identity;
- calibration identity.

### Gauge anchors

The lowest `image_id` registered camera is the fixed pose anchor.

The deterministic farthest camera supplies the scale anchor, with exact tie-breaks.

One coordinate of the scale-anchor center is fixed according to the frozen rule.

Degenerate scale anchor:

```text
max(abs(delta.x), abs(delta.y), abs(delta.z)) <= 1e-9
```

Such a component is not optimized.

### Robust objective

Each observation is one 2D residual block.

With Huber delta `2.0` pixels:

```text
s = dx*dx + dy*dy

rho(s) = s                    if s <= 4
rho(s) = 4*sqrt(s) - 4        otherwise

robust_cost = 0.5 * sum(rho(s))
```

The Huber loss applies to the complete 2D residual norm, not independently to `dx` and `dy`.

### Solver

Gate E uses the frozen Ceres 2.2.x CPU path with explicit deterministic ordering and one solver thread.

Key frozen choices include:

```text
TRUST_REGION
LEVENBERG_MARQUARDT
ITERATIVE_SCHUR
SCHUR_JACOBI
num_threads = 1
max_num_iterations = 50
```

No GPU/CUDA Bundle Adjustment belongs to Gate E v1.

### Structural eligibility

The frozen manifest-underconstraint checks include the explicit camera/landmark/observation degrees of
freedom and graph-support checks defined by Gate E.

They are structural checks, not a numerical rank claim.

No dense camera-by-landmark matrix is permitted.

## Persistence

Sparse Reconstruction persistence was introduced by the frozen Sparse SfM Project DB lineage.

The typed Sparse SfM Task payload was added separately for deterministic restart.

Current schema:

```text
CURRENT_PROJECT_DB_SCHEMA=v25
```

Later v22/v23/v24/v25 additions do not alter Sparse SfM scientific identity.

## Restart

`sparse_sfm.run/1` is reconstructed from explicit durable references.

Restart does not persist or revive:

- Ceres internal state;
- OpenCV internal state;
- temporary triangulation buffers;
- partial in-memory reconstruction;
- random engine state outside the frozen deterministic derivation.

The atomic frozen execution recomputes from the immutable inputs.

## Resource policy

Sparse SfM's frozen execution remains CPU1/batch1.

That serialism is explicitly justified by its frozen scientific/numerical validation boundary.

It must not be used as evidence that unrelated stages should run serially.

```text
SERIALISM_REQUIRES_PROOF=CANONICAL
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
```

For Sparse SfM v1, the proof chooses the conservative atomic contract.

## Current real-execution boundary

### Historical S21

The retained S21 real proof reached a frozen Track Set.

It did not execute Sparse SfM.

The historical S21 campaign cannot be retrofitted with invented calibration.

### Historical A6000

The retained A6000 pre-SfM proof reached:

```text
Feature Sets       689
Candidate Pairs    38,420
Match Results      38,420
Applicable GVRs    37,805
Verified GVRs      10,952
Rejected GVRs      26,853
Track Set          1
Tracks             130,714
Track observations 318,944
```

Checkpoint:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

It then stopped.

The retained project contains:

```text
sparse_sfm_tasks=0
sparse_reconstructions=0
dense/mvs execution=0
```

Known calibration is unavailable for that historical campaign.

Therefore:

```text
SPARSE_SFM_IMPLEMENTATION=AVAILABLE
REAL_A6000_SPARSE_SFM=BLOCKED_BY_KNOWN_CALIBRATION_DATA
```

These statements are compatible and must not be collapsed into either "Sparse SfM is not implemented"
or "real Sparse SfM already ran".

## Summary

```text
SPARSE_SFM_V1=IMPLEMENTED
GATE_A=DECISION/HISTORICAL
GATE_B=PASS/FROZEN
GATE_C=PASS/FROZEN
GATE_D=PASS/FROZEN
GATE_E=PASS/FROZEN
GATE_F=PASS/FROZEN
GATE_G=PASS/FROZEN

SPARSE_SFM_TASK=sparse_sfm.run/1
SPARSE_SFM_RESOURCE_SHAPE=CPU1/BATCH1/FROZEN

KNOWN_CALIBRATION_REQUIRED=YES
SILENT_CALIBRATION_SUBSTITUTION=FORBIDDEN
METRIC_SCALE_INFERENCE=FORBIDDEN

REAL_S21_SPARSE_SFM=NOT_EXECUTED
REAL_A6000_SPARSE_SFM=NOT_EXECUTED
REAL_A6000_SPARSE_SFM=BLOCKED_BY_KNOWN_CALIBRATION_DATA

CURRENT_PROJECT_DB_SCHEMA=v25
REAL_A6000_PRE_SFM=PASS/FROZEN
```
