# Sparse SfM / Triangulation — Gate A

## Status and boundary

**GATE A — DECISION after contract and probe study.** This document defines the
scientific and architectural contract for the Sparse SfM layer. B2 implements
the immutable v16 persistence model and its bounded readers; it does not
implement a numerical solver, change Track Model v1, change Track Builder v1,
or add a Task Kind. `FACT`, `CANDIDATE`, `DECISION` and `FROZEN` remain explicit:
the upstream Track Model and Track Builder are FROZEN, while numerical Sparse
SfM remains deferred to later gates.

**GATE B — PASS.** The v16 persistence model, migration, corruption/lifecycle
proofs, structural comparator, representative resource validation, normal
suite and ASan/UBSan closure are complete. Numerical Sparse SfM remains
deferred to Gate C and later gates.

## Scope

Sparse SfM consumes exactly one immutable, complete Track Set. It estimates a
set of camera poses and sparse 3D landmarks from coherent 2D observations. It
does not alter the Track Set, Match Results, GVRs or Feature Store. It does not
perform dense matching, meshing, texturing, metric alignment or bundle
adjustment in Gate A.

The input is one explicit Track Set identity, never “the latest Track Set” or
an enumeration of mutable project state. A disconnected image graph is
reconstructed as independent components, each with its own similarity gauge;
no metric or spatial relation between disconnected components is invented.

## Terminology and input contract

- **Image**: an acquisition with immutable pixel dimensions and an `image_id`.
- **Calibration**: the immutable intrinsic model assigned to one image or an
  explicit calibration group.
- **Pose**: the rigid transform relating world coordinates to one camera frame.
- **Track**: the frozen coherent set of 2D observations from Track Model v1.
- **Landmark**: a Sparse SfM-owned 3D estimate derived from zero or one Track;
  it is never stored in Track Model v1.
- **Observation coordinate**: the Feature File keypoint `x,y`, not a descriptor
  vector and not a coordinate inferred from a feature index.

The Feature Store v1/v2 Feature File is the canonical coordinate source. Its
keypoint records contain binary32 `x,y`, decoded image width/height, and use a
top-left origin with +x right and +y down. A Gate B reader must page keypoint
records by index; it must not load descriptors merely to obtain coordinates.

Input validation requires the Track Set to be complete and loadable, every
referenced Feature Set and Feature File to validate, every calibration to be
present and finite, and every observation index to remain within its Feature
Set. A corrupt upstream object is a runtime/input error, not an SfM outlier.

## Camera and calibration decision

### v1 supported calibration

**DECISION: known calibration only.** Sparse SfM v1 accepts an immutable
calibration for every input image. Unknown, partially known and shared-focal
estimation are rejected until a later model gate. EXIF focal data is advisory
input for constructing a calibration, never an implicit scientific fallback.

This is deliberate for phone imagery: autofocus, digital crops, orientation,
rescaling and device variation make “all images share one perfect K” unsafe.
The calibration owner is therefore an explicit per-image or calibration-group
input whose membership and parameters are part of the reconstruction identity.

### Pinhole model

The v1 camera model is pinhole with binary64:

```text
K = [ fx  0  cx ]
    [  0 fy  cy ]
    [  0  0  1  ]
```

Skew is fixed to zero. `fx > 0`, `fy > 0`, `0 <= cx < width`, and
`0 <= cy < height`. The supported distortion candidate is OpenCV-compatible
radial `k1,k2` plus tangential `p1,p2`; all four values are either supplied as
an immutable calibrated model or the model is explicitly zero-distortion.
Higher radial coefficients, rational models and thin-prism terms are not v1.
The exact distortion model and values are scientific identity fields.

### Coordinates and pose

Pixel coordinates are continuous binary64 coordinates with origin at the
top-left pixel corner, +x right and +y down. Pixel centers therefore have the
usual half-pixel interpretation supplied by the Feature File convention. A
calibrated point is undistorted first, then normalized:

```text
xn = (u_undistorted - cx) / fx
yn = (v_undistorted - cy) / fy
ray_camera = normalize([xn, yn, 1])
```

The camera frame is right-handed with x right, y down and z forward. The world
frame is also right-handed but is otherwise a gauge choice. Pose is
**world-to-camera**:

```text
Xc = R_cw * Xw + t_cw
Cw = -transpose(R_cw) * t_cw
```

The public/persisted representation is a row-major binary64 rotation matrix
plus binary64 translation. Solver-private angle-axis or quaternion variables
are permitted later. A persisted quaternion is not required, so the `q/-q`
sign ambiguity is avoided. Rendering/FreeCAD coordinate transforms are
downstream export concerns and do not change this scientific convention.

### Gauge and scale

Monocular reconstruction has a seven-degree-of-freedom similarity ambiguity.
For each connected reconstruction component, the deterministic seed camera is
the lowest canonical image ID in the selected seed pair. Its pose is fixed to
`R=I,t=0`. The second seed camera's translation direction is selected by the
deterministic essential decomposition and its norm is fixed to one arbitrary
world unit. The remaining gauge is thereby fixed to a unit seed baseline.

This unit is not metres, millimetres or any physical scale. Metric scale,
absolute orientation and georeferencing require a future explicit alignment
stage using control distances, markers or surveyed points. No fake millimetres
are inferred from focal pixels, image resolution or baseline normalization.

## Reconstruction strategy

**DECISION: incremental SfM followed by final per-component refinement.** It
matches the expected sequential vehicle/phone capture,
allows unregistered images to remain visible as a scientific result, and keeps
the active problem bounded. Global-only rotation/translation averaging would
add a larger initialization and robustness surface without a current project
requirement. A hybrid strategy is rejected for v1 complexity.

### Seed selection and relative pose

The seed is selected from the Track Set/covisibility graph, not raw Match
Results. Candidate pairs require at least the later configured minimum of
valid shared Tracks, non-degenerate essential geometry, positive-depth support
and measurable parallax. Candidates are sorted by a deterministic tuple:

```text
(-shared_track_count, -robust_parallax_score, image_id_a, image_id_b)
```

The numerical thresholds are Gate B parameter candidates and must be
fingerprinted when frozen; this tuple is the ordering policy, not a descriptor
score. Pure rotation, near-zero baseline, planar ambiguity and insufficient
cheirality reject a seed rather than inventing a scale.

Known intrinsics convert the upstream Fundamental relation into normalized
coordinates and an Essential candidate. Relative pose uses deterministic
essential decomposition with all four hypotheses tested by cheirality and
triangulation support. An F matrix is never treated as an E matrix.

### Registration

After the seed, an unregistered image is eligible when it has enough
Track-to-landmark correspondences to registered cameras. It is selected by
descending visible landmark count, then spatial-distribution score, then image
ID. Pose estimation uses a deterministic robust PnP candidate with fixed
binary64 validation, explicit iteration/confidence parameters and a local seed;
global OpenCV RNG state is forbidden. An image that cannot register remains
`UNREGISTERED` in the future SfM result and does not make the whole result a
runtime failure. Retry is bounded to deterministic graph-growth rounds; no
infinite retry loop exists.

### Components

Every connected image component is processed independently. A component with
fewer than two registered cameras has no valid 3D reconstruction. Components
with two valid cameras are allowed. Each accepted component carries its own
unit-baseline gauge and component ID; combining components requires a future
metric/alignment stage.

## Triangulation decision

**DECISION: normalized-coordinate linear DLT initialization followed by
multi-view binary64 reprojection refinement when the acceptance checks pass.**
For a Track, all currently registered observations are used in a bounded linear
system; the result is dehomogenized only when finite and well-conditioned. A
small deterministic nonlinear point-only refinement may follow. The solver does
not mutate the Track and does not split it in v1.

Accepted points require finite coordinates, positive depth in the required
observing cameras, a non-degenerate condition estimate, and reprojection
residuals within the frozen later threshold. Low parallax, planar/collinear
ill-conditioning, behind-camera points, non-finite values and excessive
reprojection error reject the landmark while leaving the source Track intact.
No arbitrary “best descriptor” or Match score is used. Pair quality is based
only on geometry; multi-view Tracks use all valid observations rather than a
random pair. Robust observation dropping is deferred: v1 rejects the landmark
as a whole, so Track identity and observation ownership remain simple.

## Gate E v1 — Final Bundle Adjustment decision

**GATE E — PASS / FROZEN.** The synchronous CPU-only final per-component
Bundle Adjustment implementation, E01--E35 matrix, normal suite, targeted
ASan/UBSan with LeakSanitizer, full sequential ASan/UBSan suite and at least
20 fresh-process E27 comparisons are validated. Gate F project orchestration
and Gate G resource integration remain later gates.

**DECISION: Gate E v1 is a synchronous, independent final per-component Bundle
Adjustment applied as post-processing to a copy of the immutable final Gate D
result.** It consumes two caller-owned immutable views that must remain coherent
for the complete call: that final Gate D result, and the same resolved
observation/calibration view used to construct the scientific Gate D input. It
never mutates either view, never creates constraints between disconnected
components, preserves each component's independent gauge, and produces a
distinct in-memory BA result.

The Gate D result alone is authoritative for final components, registered
cameras, initial poses, landmarks and the observations associated with each
landmark. The second view only resolves an observation already published by
Gate D. Its canonical key is `(feature_set_id, feature_index)`; resolution must
return the matching `image_id`, source keypoint `x,y` and immutable calibration,
and must also agree with the published Track and image identities. Missing,
ambiguous, duplicate or inconsistent resolution is a Gate E input error. Gate E
must not use array position, proximity or another heuristic fallback, add an
observation, restore a rejected association or camera, or rerun incremental SfM.

Source keypoint coordinates are the Feature File binary32 `x,y` in decoded-image
pixels, with top-left origin, +x right and +y down. Gate E converts them to
binary64 for computation; it does not treat them as already undistorted or
normalized. Given `Xc = R_cw * Xw + t_cw`, define `xn = Xc.x / Xc.z`,
`yn = Xc.y / Xc.z`, and `r2 = xn*xn + yn*yn`. The canonical OpenCV-compatible
forward model is:

```text
radial = 1 + k1*r2 + k2*r2*r2
xd = xn*radial + 2*p1*xn*yn + p2*(r2 + 2*xn*xn)
yd = yn*radial + p1*(r2 + 2*yn*yn) + 2*p2*xn*yn
u = fx*xd + cx
v = fy*yd + cy
residual = [u - observed_x, v - observed_y]
```

The residual is therefore binary64 in source pixels and uses the complete
canonical calibration model. A private Gate D validation helper that omits
distortion does not redefine this contract and is not a precedent for Gate E.
The second immutable view is an explicit scientific input, not persistence,
Project DB integration, a loader, resolver subsystem, cache, handle or Resource
System.

### Scientific and numerical contract

Gate E processes every reconstructed Gate D component independently. It
resolves the selected observations, copies the component poses and landmarks
into a private working set, builds and solves one BA problem, validates the
complete candidate, then either publishes that candidate in the distinct Gate
E result or preserves the original Gate D component. No component constrains or
influences another component.

Gate E v1 optimizes only camera extrinsic rotations, camera centers and
landmark positions. The known `fx`, `fy`, `cx`, `cy`, `k1`, `k2`, `p1`, `p2`,
observations, Track membership, identities and observation/landmark
associations are fixed and immutable. Future intrinsic optimization requires a
separate scientific and identity decision.

The public boundary remains solver-independent and world-to-camera. The private
C++ adapter uses a unit quaternion for `R_cw`, with an appropriate quaternion
manifold, and world camera center `Cw`:

```text
Xc = R_cw * (Xw - Cw)
t_cw = -R_cw * Cw
```

Conversion to or from public rotation matrices canonicalizes quaternion sign,
so `q` and `-q` cannot produce distinct observable representations. No Ceres
type crosses the future C17 ABI.

### Gate E gauge

Gate E derives deterministic BA anchors from the final Gate D result and does
not depend on historical seed IDs. In each component, the registered camera
with the lowest `image_id` is the pose anchor; its complete initial Gate D
rotation and camera center are fixed.

Among the other registered cameras, the scale anchor is the camera whose
binary64 Euclidean distance from the pose anchor is greatest. An exact distance
tie selects the lowest `image_id`; no hidden tolerance participates. For
`delta = C_scale - C_anchor`, the coordinate with greatest absolute value is
the scale axis, with exact ties resolved X, then Y, then Z. That one initial
Gate D coordinate of `C_scale` is fixed. Its other two center coordinates and
its rotation remain variable. The fixed pose removes the six rigid degrees of
freedom and the fixed nonzero scale coordinate removes the scale degree of
freedom without fixing a second pose.

The scale anchor is degenerate when:

```text
max(abs(delta.x), abs(delta.y), abs(delta.z)) <= 1e-9
```

Gate D fixes each valid component to a unit seed baseline, so `1e-9` world
units is a numerically negligible separation in that scientific gauge. A
component with no second valid camera or a degenerate scale anchor is not
optimized; its Gate D data is retained with a gauge/degenerate diagnostic.

### Objective and solver

Every valid observation contributes one two-dimensional source-pixel residual
block `f_i = [dx, dy]`, where `dx = predicted_x - observed_x` and
`dy = predicted_y - observed_y`. Gate E uses binary64 throughout. Define:

```text
s_i = dx*dx + dy*dy
delta = 2.0
delta2 = 4.0

rho_delta(s) = s                         if s <= delta2
rho_delta(s) = 2*delta*sqrt(s) - delta2 if s > delta2

robust_cost = 0.5 * sum_i(rho_delta(s_i))
```

Thus, with `delta = 2.0` source pixels, the second branch is
`4.0*sqrt(s) - 4.0`. The Huber loss applies once to the norm squared of the
complete 2D observation, never independently to `dx` and `dy`; the factor
`0.5` is contractual. Each observation must likewise be one 2D Ceres residual
block, not two scalar blocks.

Lardon3D computes initial and final robust costs independently of the solver
using exactly this formula, and those values govern acceptance. Ceres summary
costs may only diagnose or cross-check them. With the identical problem, a
disagreement beyond the applicable numerical tolerance stops implementation
for contract review; neither value silently replaces the other. Non-finite
`dx`, `dy`, `s_i`, `rho_delta(s_i)`, accumulation, pose, landmark or projection,
and camera-frame depth invalid under the frozen camera invariants, reject the
candidate. No clamp or fallback is permitted.

The Huber kind and scale are Gate E scientific policy, not Governor parameters,
Resource parameters or a Gate E fingerprint.

Gate E v1 selects the Ceres Solver 2.2.x API, CPU-only, with these explicit
options:

```text
minimizer_type = TRUST_REGION
trust_region_strategy_type = LEVENBERG_MARQUARDT
linear_solver_type = ITERATIVE_SCHUR
preconditioner_type = SCHUR_JACOBI
num_threads = 1
max_num_iterations = 50
function_tolerance = 1e-6
gradient_tolerance = 1e-10
parameter_tolerance = 1e-8
```

Landmark parameter blocks form elimination group 0 in increasing canonical
Track/landmark identity; camera blocks form group 1 in increasing `image_id`.
Components, cameras, landmarks, observations and residual blocks are all built
in canonical identity order. Automatic Ceres ordering is not used when the API
accepts an explicit ordering.

There is exactly one solver attempt per eligible component, with no automatic
retry, wall-clock timeout or `max_solver_time`. Environment variables, hardware
profiles, Tasks, schedulers and the Governor cannot change the single-thread
reference. `ITERATIVE_SCHUR` with `SCHUR_JACOBI` provides the required
block-sparse path without a functional SuiteSparse dependency; `SPARSE_SCHUR`,
CUDA and GPU execution are not Gate E v1.

### Eligibility, bounds and acceptance

An eligible component has at least two registered cameras, at least one valid
BA landmark, exactly resolved observations and calibrations, finite inputs, a
valid gauge, overflow-safe dimensions and no manifest underconstraint after
the anchors. Gate D already guarantees multi-view support for every published
landmark, so Gate E introduces no separate support threshold.

For a component with a valid non-degenerate Gate E gauge, let `C` be its
registered camera count, `P` its optimized landmark count and `O` its retained,
resolved observation count. Camera intrinsics and distortion are fixed. The
free tangent dimension is therefore:

```text
free_dof = 6*C + 3*P - 7
scalar_residual_count = 2*O
```

The completely fixed pose anchor removes six degrees of freedom, and the fixed
scale-anchor center coordinate removes one. Gate E v1 defines **manifest
underconstraint** as at least one of these exact structural conditions:

- **UC1:** `2*O < 6*C + 3*P - 7`, using overflow-checked integer arithmetic;
- **UC2:** an optimized landmark is observed by fewer than two distinct
  registered cameras;
- **UC3:** an optimizable camera, including the scale anchor but excluding the
  completely fixed pose anchor, observes fewer than three distinct landmarks;
- **UC4:** the bipartite camera-landmark optimization graph is not one connected
  component containing the pose anchor.

E19 evaluates UC1--UC4 only after the existing structural validation and valid
anchor selection. E18 remains the existing insufficient-camera case and is not
redefined by E19. These conditions are necessary structural checks, not proof
of full numerical rank. Gate E v1 performs no numerical rank estimate, SVD,
singular-value or condition-number threshold, Jacobian/Hessian rank epsilon, or
Ceres covariance/rank heuristic for E19. Geometry that passes UC1--UC4 can
still be rejected by the existing projection, solver termination, finite-value,
cost non-regression and atomic-publication contracts.

UC1--UC4 are deterministic and solver-independent. Their implementation uses
the existing canonical flat Gate E working set and temporary storage bounded by
`O(C + P + O)` or better. It uses no hash-order dependency, dense `C * P`
storage, materialized rank matrix or new Resource subsystem.

Gate E retains the identically-scoped Gate D bounds of at most 4096 registered
cameras, 250,000 Tracks/landmarks and 1,000,000 observations. The Gate D
landmarks-per-growth-round bound is not a Gate E bound. All allocation and
dimension arithmetic is overflow-checked. The architecture is block-sparse;
no dense camera-count × landmark-count allocation or Jacobian is permitted.

Ceres `NO_CONVERGENCE` is rejection even if an intermediate candidate has
lower cost. Only a termination classified as successful convergence by the
private Ceres adapter is acceptable. The robust-cost comparison uses exactly:

```text
cost_tolerance = 1e-12 * max(1.0, abs(initial_robust_cost))
final_robust_cost <= initial_robust_cost + cost_tolerance
```

This comparison tolerance absorbs insignificant binary64 noise and is not a
Ceres convergence tolerance. A component is published only when its inputs are
coherent and eligible, termination is accepted, all candidate poses,
landmarks, required projections and robust costs are finite, both gauge anchors
are strictly preserved in their contract representations, the cost condition
holds, and no consumed frozen invariant is violated. Otherwise the original
Gate D component is preserved exactly and accompanied by a rejection
diagnostic. All optimization occurs on a private copy, so publication is atomic
per component and requires no in-place rollback.

### Result and diagnostics

The solver-independent Gate E result has these conceptual states:

- `COMPLETE`: at least one component is eligible and every eligible component
  is optimized and accepted;
- `PARTIAL`: at least one component is accepted and at least one other eligible
  component is rejected or fails;
- `FAILED`: no eligible component produces an accepted BA result, including an
  input with no eligible component.

`Lardon3DSparseBundleAdjustmentStatus` contains only these three scientific
result states. In particular, `FAILED` is not an invalid-argument,
out-of-memory or internal execution error.

The synchronous execution function returns the separate,
solver-independent `Lardon3DSparseBundleAdjustmentExecutionStatus`:

```text
LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK
LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INVALID_ARGUMENT
LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OUT_OF_MEMORY
LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR
```

`EXECUTION_OK` means the public input was structurally valid, Gate E reached a
complete scientific decision and produced the owned result. Its scientific
status may be `COMPLETE`, `PARTIAL` or `FAILED`; `EXECUTION_OK` with scientific
`FAILED` is valid and means that no eligible component was accepted.

`EXECUTION_INVALID_ARGUMENT` covers a violated public input contract, including
pointer/count, bounds, identity, finiteness, observation-resolution or
Gate-D/result-view coherence failures. `EXECUTION_OUT_OF_MEMORY` covers an
allocation failure, including `std::bad_alloc` caught at the C/C++ boundary,
that prevents production of a complete scientific result.
`EXECUTION_INTERNAL_ERROR` is reserved for an unexpected internal failure that
prevents safe completion; it is not a component-rejection fallback. Normal
component rejection for insufficient cameras, gauge degeneracy, manifest
underconstraint, invalid candidate projection, solver `NO_CONVERGENCE` or
`FAILURE`, a non-finite candidate or robust-cost regression contributes only to
the scientific `COMPLETE`/`PARTIAL`/`FAILED` result.

On every execution status other than `EXECUTION_OK`, the public result remains
in its canonical zero state: all counts are zero, all owned array and diagnostic
pointers are null, and destruction is safe. The execution function never
publishes a partial owned result and then returns an execution error.

Ineligible and rejected components retain their Gate D data. Each component
diagnostic contains at least component key, camera/landmark/observation counts,
pose-anchor and scale-anchor `image_id`, scale axis X/Y/Z, initial and final
robust cost, initial and final reprojection RMSE, iteration count, solver
termination class, accepted/rejected state and rejection reason. It exposes no
Ceres pointer or type.

Diagnostic reprojection RMSE is non-robust:

```text
sqrt(sum(dx*dx + dy*dy) / observation_count)
```

Acceptance remains based on robust cost and all contract invariants. Raw RMSE
is not required to improve universally in the presence of outliers.

### Reproducibility

For identical input, executable, build, dependency versions and machine with
one solver thread, component order, anchors, parameter/residual ordering,
states, accept/reject decisions and structural diagnostics are deterministic.
Comparable binary64 geometric scalars satisfy:

```text
abs(a - b) <= 1e-12 * max(1.0, abs(a), abs(b))
```

Rotations are compared geometrically rather than by raw quaternion sign. If
fresh-process tests in an identical environment cannot meet this tolerance,
implementation stops for contract review; tests must not widen it silently.

### Canonical Gate E validation matrix

| Case | Contract evidence |
|---|---|
| E01 Null/invalid input | Safe rejection; no exception crosses C |
| E02 Empty/non-eligible result | Deterministic `FAILED` with diagnostics |
| E03 Clean synthetic component | Finite accepted result, gauge held, cost non-regression |
| E04 Perturbed poses | Fixture-defined measurable improvement |
| E05 Perturbed landmarks | Fixture-defined measurable improvement |
| E06 Perturbed poses and landmarks | Convergence and fixture-defined improvement |
| E07 Noise 0.5 px | Finite accepted result or contractually justified rejection |
| E08 Noise 1.0 px | Finite accepted result or contractually justified rejection |
| E09 Noise 2.0 px | Finite accepted result or contractually justified rejection |
| E10 Outliers 10% | Huber active; finite result or clean rejection; no invariant violation |
| E11 Outliers 20% | Huber active; finite result or clean rejection; no invariant violation |
| E12 Outliers 40% | Huber active; finite result or clean rejection; no invariant violation |
| E13 Disconnected components | Independent optimization and gauges |
| E14 One success, one failure | Global `PARTIAL` |
| E15 Pose anchor | Initial rotation and center strictly preserved |
| E16 Scale anchor | Selected center coordinate strictly preserved |
| E17 Deterministic anchors | Exact distance/ID and X/Y/Z ties; `1e-9` degeneracy boundary |
| E18 Insufficient cameras | No solve; Gate D data retained |
| E19 Underconstrained geometry | No solve; Gate D data retained |
| E20 Non-finite input | Input rejection |
| E21 Non-finite projection candidate | Atomic candidate rejection |
| E22 Forced non-convergence | Private summary interpreter rejects `NO_CONVERGENCE` |
| E23 Candidate regression | Cost condition prevents publication |
| E24 Atomic rejection | Original component preserved exactly |
| E25 Canonical ordering | Explicit groups and parameter/residual order |
| E26 Same-process repeats | Structural equality and numeric tolerance |
| E27 Fresh-process repeats | At least 20 processes in one identical environment |
| E28 Ownership/destruction | Caller inputs retained; owned result safely destroyed |
| E29 Null/repeated destroy | Required only if E1 adopts the existing null-safe convention |
| E30 Allocation/overflow | Checked rejection before allocation |
| E31 Maximum boundary guards | Exact documented limits without a giant solve where isolatable |
| E32 Sparse architecture | No dense camera-count × landmark-count allocation |
| E33 Calibration immutability | Before/after identical |
| E34 Track/observation immutability | Before/after identical |
| E35 Gate D immutability | Input unchanged after success and every failure path |

E22 tests the private solver-summary-to-decision interpreter directly. It does
not expose an iteration override, add a production behavior for testing or
change `max_num_iterations = 50`. Synthetic ground-truth fixtures measure
pre/post geometric error and robust cost. Fixtures intended to improve define
their own scientifically measurable improvement; no universal pose or landmark
threshold is invented.

The complete E01--E35 matrix is implemented and validated. E27 passed at least
20 fresh processes using exact structural comparison, the frozen binary64
tolerance and geometric rotation comparison.

Local BA after registration is deferred. Gate D exposes no intermediate
scientific seam or complete registration history, and an interleaved BA could
change its subsequent growth. Introducing that policy requires a future
explicit scientific seam/version architecture decision; Gate E v1 does not
create or name such a version.

Gate E v1 remains independent of Project DB, Task Runtime, the Resource
Governor and any Resource System. It neither computes nor carries a parameter
fingerprint, defines no persistent identity, and publishes nothing. Gate F
retains project/task orchestration and persistence; Gate G retains Resource
Governor integration and final resource validation.

Ceres availability on a host remains distinct from Lardon3D dependency
declaration. Gate E declares Ceres Solver `>=2.2.0,<2.3.0` through Meson CMake
discovery and uses its 2.2.x CPU API. CUDA is not required and Lardon3D has no
functional direct SuiteSparse dependency.

## Determinism and scientific identity

Canonical order is: component image IDs, seed tuple, registration candidates,
Track IDs, observation positions, and output landmarks by `(component_id,
track_id)`. No unordered container iteration, wall-clock value, queue position,
RAM state or task ID may affect science. Binary64 is the default for geometry,
residuals and persisted values; all accepted values must be finite.

The candidate reconstruction identity is:

```text
(input_track_set_identity,
 calibration_scope_identity,
 sfm_kind="incremental",
 sfm_version,
 parameter_fingerprint)
```

Runtime task IDs, worker count, Governor state, pause timing and resource
observations are excluded. Any output-changing threshold, camera model,
initialization policy, triangulation policy, PnP policy, BA policy, precision or
loss parameter belongs in the parameter fingerprint materialized by the Gate F
persistent-identity seam. Gate D and Gate E neither receive, compute, serialize
nor transport it. Project DB stores it but does not own its meaning; Task Runtime
and the Resource System are also excluded. This seam is a Gate F orchestration
responsibility, not a new subsystem or scientific solver gate.

### Sparse SfM parameter fingerprint v1

The parameter fingerprint is `SHA-256(record_v1)`, with one hash operation over
the exact fixed record below. Its output is the complete 32-byte digest. The
record begins with the eight ASCII bytes `L3DSFMFP` (`4c 33 44 53 46 4d 46 50`),
without a NUL byte, followed by `fingerprint_encoding_version=1`. Every
multi-byte scalar is little-endian. `f64` means the exact finite IEEE-754
binary64 bit pattern, with both signed zeros encoded as positive zero; NaN and
infinity are invalid. Categorical values are the explicit `u32` policy IDs
defined here, never native enum ordinals. No native struct, padding, pointer,
`size_t`, host-endian value, JSON or locale-dependent text is hashed. A boolean,
if a future encoding uses one, is `u8`, with false `0` and true `1`; v1 has no
boolean field and no padding or reserved bytes.

| Offset | Width | Field | Canonical type | Source/value |
|---:|---:|---|---|---|
| 0 | 8 | `domain` | ASCII bytes | `L3DSFMFP` |
| 8 | 4 | `fingerprint_encoding_version` | `u32` | `1` |
| 12 | 4 | `minimum_seed_tracks` | `u32` | effective Gate D parameter |
| 16 | 4 | `minimum_seed_landmarks` | `u32` | effective Gate D parameter |
| 20 | 4 | `minimum_pnp_correspondences` | `u32` | effective Gate D parameter |
| 24 | 4 | `maximum_seed_candidates` | `u32` | effective Gate D parameter |
| 28 | 4 | `maximum_registration_rounds` | `u32` | effective Gate D parameter |
| 32 | 4 | `maximum_landmarks_per_round` | `u32` | effective Gate D parameter |
| 36 | 4 | `maximum_images` | `u32` | effective Gate D parameter |
| 40 | 8 | `maximum_observations` | `u64` | effective Gate D parameter |
| 48 | 8 | `maximum_tracks` | `u64` | effective Gate D parameter |
| 56 | 8 | `reprojection_threshold_px` | `f64` | effective Gate D parameter |
| 64 | 8 | `minimum_track_parallax_rad` | `f64` | effective Gate D parameter |
| 72 | 8 | `relative_pose.robust_threshold_px` | `f64` | effective Gate D parameter |
| 80 | 8 | `relative_pose.confidence` | `f64` | effective Gate D parameter |
| 88 | 4 | `relative_pose.max_iterations` | `u32` | effective Gate D parameter |
| 92 | 4 | `relative_pose.minimum_inliers` | `u32` | effective Gate D parameter |
| 96 | 8 | `relative_pose.minimum_inlier_ratio` | `f64` | effective Gate D parameter |
| 104 | 8 | `relative_pose.minimum_parallax_rad` | `f64` | effective Gate D parameter |
| 112 | 8 | `relative_pose.minimum_cheirality_ratio` | `f64` | effective Gate D parameter |
| 120 | 8 | `relative_pose.deterministic_seed` | `u64` | effective Gate D parameter |
| 128 | 8 | `pnp.reprojection_threshold_px` | `f64` | effective Gate D parameter |
| 136 | 8 | `pnp.confidence` | `f64` | effective Gate D parameter |
| 144 | 4 | `pnp.max_iterations` | `u32` | effective Gate D parameter |
| 148 | 4 | `pnp.minimum_inliers` | `u32` | effective Gate D parameter |
| 152 | 8 | `pnp.minimum_inlier_ratio` | `f64` | effective Gate D parameter |
| 160 | 8 | `pnp.deterministic_seed` | `u64` | effective Gate D parameter |
| 168 | 4 | `refinement.max_iterations` | `u32` | effective Gate D parameter |
| 172 | 8 | `refinement.convergence_tolerance` | `f64` | effective Gate D parameter |
| 180 | 4 | `camera_model_policy` | `u32` | `PINHOLE_K1_K2_P1_P2_V1=1` |
| 184 | 4 | `calibration_policy` | `u32` | `KNOWN_FIXED_CALIBRATION_V1=1` |
| 188 | 4 | `source_pixel_policy` | `u32` | `SOURCE_PIXEL_TOP_LEFT_X_RIGHT_Y_DOWN_V1=1` |
| 192 | 4 | `pose_policy` | `u32` | `WORLD_TO_CAMERA_R_CW_CW_V1=1` |
| 196 | 4 | `seed_ranking_policy` | `u32` | `SHARED_PARALLAX_IMAGE_ID_V1=1` |
| 200 | 4 | `robust_seed_policy` | `u32` | `LOCAL_DETERMINISTIC_SEED_V1=1` |
| 204 | 4 | `next_image_policy` | `u32` | `VISIBLE_SUPPORT_THEN_IMAGE_ID_V1=1` |
| 208 | 4 | `track_order_policy` | `u32` | `CANONICAL_TRACK_OBSERVATION_V1=1` |
| 212 | 4 | `component_policy` | `u32` | `DISCONNECTED_INDEPENDENT_COMPONENTS_V1=1` |
| 216 | 4 | `triangulation_policy` | `u32` | `NORMALIZED_DLT_POINT_REFINE_V1=1` |
| 220 | 4 | `landmark_rejection_policy` | `u32` | `WHOLE_LANDMARK_ACCEPT_OR_REJECT_V1=1` |
| 224 | 4 | `cheirality_policy` | `u32` | `POSITIVE_DEPTH_THRESHOLD_V1=1` |
| 228 | 4 | `gate_d_numeric_policy` | `u32` | `BINARY64_DETERMINISTIC_V1=1` |
| 232 | 4 | `ba_mode` | `u32` | `FINAL_PER_COMPONENT_POSTPROCESS_V1=1` |
| 236 | 4 | `local_ba_policy` | `u32` | `LOCAL_BA_DISABLED_V1=1` |
| 240 | 4 | `ba_optimized_variables` | `u32` | `ROTATION_CENTER_LANDMARK_XYZ_V1=1` |
| 244 | 4 | `ba_fixed_inputs` | `u32` | `INTRINSICS_DISTORTION_OBSERVATIONS_IDENTITIES_V1=1` |
| 248 | 4 | `ba_pose_representation` | `u32` | `UNIT_QUATERNION_R_CW_PLUS_CW_V1=1` |
| 252 | 4 | `ba_gauge_policy` | `u32` | `MIN_IMAGE_FARTHEST_CENTER_ONE_AXIS_V1=1` |
| 256 | 8 | `ba_degenerate_scale_threshold` | `f64` | `1e-9` |
| 264 | 4 | `ba_residual_policy` | `u32` | `ONE_FULL_2D_BLOCK_PER_OBSERVATION_V1=1` |
| 268 | 4 | `ba_projection_policy` | `u32` | `SOURCE_PIXEL_PINHOLE_K1_K2_P1_P2_V1=1` |
| 272 | 8 | `ba_minimum_camera_depth` | `f64` | `1e-9` |
| 280 | 4 | `ba_robust_loss` | `u32` | `HUBER_FULL_2D_NORM_V1=1` |
| 284 | 8 | `ba_huber_delta_px` | `f64` | `2.0` |
| 292 | 4 | `ba_robust_cost_policy` | `u32` | `HALF_SUM_RHO_SQUARED_NORM_V1=1` |
| 296 | 4 | `ba_solver_contract` | `u32` | `CERES_2_2_CONTRACT_V1=1` |
| 300 | 4 | `ba_minimizer` | `u32` | `TRUST_REGION_V1=1` |
| 304 | 4 | `ba_trust_region_strategy` | `u32` | `LEVENBERG_MARQUARDT_V1=1` |
| 308 | 4 | `ba_linear_solver` | `u32` | `ITERATIVE_SCHUR_V1=1` |
| 312 | 4 | `ba_preconditioner` | `u32` | `SCHUR_JACOBI_V1=1` |
| 316 | 4 | `ba_num_threads` | `u32` | `1` |
| 320 | 4 | `ba_max_num_iterations` | `u32` | `50` |
| 324 | 8 | `ba_function_tolerance` | `f64` | `1e-6` |
| 332 | 8 | `ba_gradient_tolerance` | `f64` | `1e-10` |
| 340 | 8 | `ba_parameter_tolerance` | `f64` | `1e-8` |
| 348 | 4 | `ba_retry_count` | `u32` | `0` |
| 352 | 4 | `ba_convergence_acceptance` | `u32` | `CONVERGENCE_ONLY_V1=1` |
| 356 | 8 | `ba_cost_non_regression_factor` | `f64` | `1e-12` |
| 364 | 4 | `ba_underconstraint_policy` | `u32` | `MANIFEST_UC1_UC2_UC3_UC4_V1=1` |
| 368 | 4 | `ba_numeric_policy` | `u32` | `BINARY64_SINGLE_THREAD_V1=1` |

The exact v1 record length is 372 bytes. Gate D field semantics and source
translations are exhaustive:

| Field | Source type | Encoding/normalization | Scientific meaning |
|---|---|---|---|
| `minimum_seed_tracks` | `uint32_t` | `u32` | minimum shared Tracks for a seed |
| `minimum_seed_landmarks` | `uint32_t` | `u32` | minimum accepted seed landmarks |
| `minimum_pnp_correspondences` | `uint32_t` | `u32` | minimum correspondences for registration |
| `maximum_seed_candidates` | `uint32_t` | `u32` | bound on seed attempts |
| `maximum_registration_rounds` | `uint32_t` | `u32` | bound on growth rounds |
| `maximum_landmarks_per_round` | `uint32_t` | `u32` | bound on new landmarks per round |
| `maximum_images` | `uint32_t` | `u32` | accepted input image bound |
| `maximum_observations` | `uint64_t` | `u64` | accepted observation bound |
| `maximum_tracks` | `uint64_t` | `u64` | accepted Track bound |
| `reprojection_threshold_px` | `double` | finite canonical `f64` | landmark reprojection acceptance |
| `minimum_track_parallax_rad` | `double` | finite canonical `f64` | landmark parallax acceptance |
| `relative_pose.robust_threshold_px` | `double` | finite canonical `f64` | essential robust residual threshold |
| `relative_pose.confidence` | `double` | finite canonical `f64` | essential robust confidence |
| `relative_pose.max_iterations` | `uint32_t` | `u32` | essential robust iteration bound |
| `relative_pose.minimum_inliers` | `uint32_t` | `u32` | essential minimum inlier count |
| `relative_pose.minimum_inlier_ratio` | `double` | finite canonical `f64` | essential minimum inlier fraction |
| `relative_pose.minimum_parallax_rad` | `double` | finite canonical `f64` | relative-pose minimum parallax |
| `relative_pose.minimum_cheirality_ratio` | `double` | finite canonical `f64` | relative-pose positive-depth fraction |
| `relative_pose.deterministic_seed` | `uint64_t` | `u64` | local essential robust-estimator seed |
| `pnp.reprojection_threshold_px` | `double` | finite canonical `f64` | PnP robust residual threshold |
| `pnp.confidence` | `double` | finite canonical `f64` | PnP robust confidence |
| `pnp.max_iterations` | `uint32_t` | `u32` | PnP robust iteration bound |
| `pnp.minimum_inliers` | `uint32_t` | `u32` | PnP minimum inlier count |
| `pnp.minimum_inlier_ratio` | `double` | finite canonical `f64` | PnP minimum inlier fraction |
| `pnp.deterministic_seed` | `uint64_t` | `u64` | local PnP robust-estimator seed |
| `refinement.max_iterations` | `uint32_t` | `u32` | point-refinement iteration bound |
| `refinement.convergence_tolerance` | `double` | finite canonical `f64` | point-refinement stopping tolerance |

Every row is fingerprinted. Integer fields require no normalization beyond
their fixed-width little-endian translation; every floating field uses the
canonical-zero rule above.

The policy IDs above freeze the full named Gate D and Gate E v1 semantics,
including canonical order and tie breaks,
whole-landmark rejection, positive-depth cheirality, the pose anchor chosen by
smallest image ID, the farthest-center scale anchor with exact image-ID tie,
X/Y/Z axis tie order and exactly one fixed center coordinate. The Gate E
projection is `R_cw + Cw`; intrinsics and `k1/k2/p1/p2` distortion are fixed.
The robust cost policy is exactly `0.5 * sum rho(dx^2+dy^2)` with one Huber loss
on each full two-dimensional residual. `CERES_2_2_CONTRACT_V1` denotes the
accepted Lardon3D Ceres contract `>=2.2.0,<2.3.0`, not package, build, linker or
transitive SuiteSparse metadata.

For Gate D, `WORLD_TO_CAMERA_R_CW_CW_V1` includes the frozen SO(3) residual
limit `1e-6`;
`POSITIVE_DEPTH_THRESHOLD_V1` means strict camera depth greater than `1e-9`;
and `NORMALIZED_DLT_POINT_REFINE_V1` includes the frozen homogeneous-scale
epsilon `1e-12` and collinearity covariance-determinant limit `1e-10`. These
constants are not runtime members, so their stable policy IDs, rather than
duplicate floating fields or implementation enum ordinals, own their exact
v1 semantics.

All 27 effective scalar members, including nested members, of
`Lardon3DSparseIncrementalParameters` occur exactly once. Serialization uses
the validated effective values actually passed to Gate D, so an omitted default
and the same explicitly supplied value produce identical bytes. Actual Track
Set identity, Track/Feature IDs, calibration-scope identity and individual
calibration IDs or numeric values, `sfm_kind`, `sfm_version`, project/task/
transaction/reconstruction IDs, timestamps, resource state, result values and
metrics are excluded. The separate calibration scope hash binds sorted image
IDs to calibration hashes, and each calibration hash binds dimensions,
`fx/fy/cx/cy/k1/k2/p1/p2`, model/version and provenance; the parameter record
therefore records calibration semantics without duplicating calibration
instances.

Changing only a parameter value retains encoding version 1 and naturally
changes the digest. Adding a fingerprint-owned field or changing byte layout
requires a new encoding version; changing the Sparse SfM algorithmic contract
may separately require a new `sfm_version`. Neither version substitutes for the
other. Equal complete candidate tuples therefore identify the same scientific
candidate regardless of runtime metadata.

Gate F implementation must prefer an internal, solver-independent helper unless
a separate public C17 API decision is made. It must reuse the project's SHA-256
implementation, use bounded constant-size storage without cache, scheduling,
Governor interaction or reservation, and add a golden 372-byte default record,
its expected SHA-256 digest, and mutations proving every fingerprint-owned
category changes the digest. This contract authorizes no public symbol.

Exact byte identity is not promised for a future multi-threaded floating-point
solver until measured. The v1 target is deterministic ordering and numerical
reproducibility within documented tolerances; single-threaded reductions are
the initial reference.

## Future persistence and API candidates

No Project DB v16 is created in Gate A. A later model gate may define immutable
entities such as `sparse_reconstructions`, registered camera poses, landmarks,
and landmark observations. The reconstruction must reference exactly one Track
Set and calibration identity, publish atomically, and never expose half-solved
cameras or points. Upstream Track Set deletion policy requires an explicit
future ownership decision; silent CASCADE of a published reconstruction is not
assumed.

The future public boundary remains C17-safe and solver-independent. Candidate
opaque APIs accept immutable Track Set/calibration inputs and return owned
opaque result pages with explicit free functions. No `cv::Mat`, Eigen, Ceres,
STL, callback or C++ exception crosses the boundary. Numeric kernels operate on
pure in-memory structures and never open SQLite or query the Governor.

## Resource envelope

Let `C` be registered cameras, `T` Tracks, `P` active landmarks, `O`
observations and `E_covis` sparse image-graph edges. The architecture requires
`O(C + T + P + O + E_covis)` memory for graph/index structures plus the solver
working set. It forbids a dense `C×P`, `C×C` or co-visibility matrix. Track
length has no arbitrary 256 cap; long Tracks are iterated through checked
bounded storage.

Triangulation/registration are light CPU units and can be batched. Gate E v1
uses local scientific limits for its final per-component BA and does not query
the Governor. Future Gate G admission may use `C`, `P`, `O`, solver mode and
calibration-variable count without changing scientific results. The existing
Resource Governor owns RAM/PSI/swap policy; Sparse SfM adds no system-pressure
thresholds. Swap is never normal working memory, and UMA RAM must preserve
several GiB of desktop/iGPU headroom.

## Hardware and probe study

Gate A preflight measured 16 logical CPUs, `MemTotal=15597716 KiB`,
`MemAvailable=8245288 KiB` at the study point, an 8 GiB swapfile, a 6 GiB
zram device, and zero current memory/IO PSI average. The host is the Ryzen 7
8845HS/Radeon 780M UMA target described by the performance document.

The project already links OpenCV 5.0.0. Host-installed libraries and their
pkg-config or CMake discovery metadata are capabilities, not Lardon3D
production dependencies. Gate E now declares Ceres 2.2.x through Meson CMake
discovery. No system setting, swap device or GPU mode was changed.

Gate A probes use deterministic synthetic camera arcs, controlled noise and
degenerate planar/pure-rotation cases. Every RSS probe is a separate normal
optimized child process; fixture arrays, solver structures and peak RSS are
reported separately. Thread probes are limited to 1/2/4/8 threads and stop if
MemAvailable, swap, PSI or desktop responsiveness becomes unhealthy. No
production Sparse SfM code is created by this gate.

## Future gate plan

- **Gate B — Sparse Reconstruction Model:** immutable in-memory model, result
  states, calibration ownership and candidate persistence contract; no DB v16
  until this contract is reviewed.
- **Gate C — Geometry primitives:** normalized camera model, relative pose,
  deterministic seed, triangulation and PnP with synthetic ground truth.
- **Gate D — Incremental core:** registration ordering, components,
  unregistered-image policy and deterministic reconstruction output.
- **Gate E — Final Bundle Adjustment:** synchronous final per-component BA on a
  copy of the immutable Gate D result, with its scientific and numerical
  contract frozen here; interleaved local BA is deferred.
- **Gate F — Project orchestration:** explicit Track Set/calibration input,
  atomic publication and durable runtime integration.
- **Gate G — Resource/freeze:** Governor admission, sustained hardware safety,
  recovery, full validation and final freeze.

## Algorithm comparison and Gate A evidence

### Incremental SfM

Seed/order risk is controlled by deterministic policy. It is robust for
sequential capture, has canonical queues and seeds, moderate complexity, and
sparse `C,T,O` scaling followed by final per-component BA. **SELECTED v1.**

### Global SfM

Global averaging can spread weak geometry. It is sensitive to disconnected or
weak-baseline graphs, needs several global tie policies, and requires a larger
sparse solve. **Rejected for v1.**

### Hybrid

Hybrid design combines both failure surfaces, is hard to specify minimally and
harder to reproduce. **Rejected for v1.**

Triangulation candidates:

- Midpoint/ray only: fragile with noise and awkward beyond two views. Rejected.
- Linear normalized DLT: good initialization with explicit checks and all
  registered observations. **Selected initialization.**
- DLT plus point-only refinement: better residual with bounded per-point work
  and fixed termination. **Selected v1 candidate.**

| BA candidate | Sparse support | Dependency status | Decision |
|---|---|---|---|
| Dense normal equations | Prohibited for serious `C×P` problems | No | Rejected |
| OpenCV generic optimization | Not a sparse BA contract | Present, wrong abstraction | Rejected |
| Ceres 2.2.x iterative Schur | Block-sparse | Declared through Meson CMake discovery | **Implemented Gate E v1** |

### Synthetic geometry probe

The normal OpenCV 5.0.0 installation was exercised in a fresh Python process on
100 deterministic points, binary64 K (`fx=fy=800`, `cx=640`, `cy=480`), a one-unit
baseline and a four-degree rotation. `recoverPose` retained 100 inliers with
zero measured rotation error and translation direction absolute dot product
`0.997564`; two-view DLT triangulation had median position error
`2.73e-15`; iterative PnP retained 100 inliers with camera-center error
`9.02e-8` and zero measured rotation error. This validates the candidate
primitive boundary, not production SfM correctness.

The same probe deliberately tested pure rotation and planar points. OpenCV can
still return an Essential matrix with 100 nominal inliers in both cases; this
is why `findEssentialMat` success is not an acceptance criterion. Seed
selection must apply parallax, conditioning, cheirality and model-ambiguity
checks before accepting a component.

### Dependency and hardware evidence

The project already links OpenCV 5.0.0. Host probes found Eigen 5.0.1, BLAS
3.12.0, LAPACK 3.12.0 and TBB 2023.1 as host capabilities or transitive
facilities rather than current Lardon3D production dependencies. Ceres may use
CMake discovery, so pkg-config alone does not establish host availability.
Ceres 2.2.x is the implemented Gate E scientific API and is declared through
Meson CMake discovery. The measured machine has 16 logical CPUs,
`MemTotal=15597716 KiB`, `MemAvailable=8245288 KiB` at preflight, an 8 GiB
swapfile, 6 GiB zram and zero memory/IO PSI averages at the probe start. Gate E
uses one solver thread; future Gate G resource admission cannot change that
scientific setting.

## Gate A unresolved boundaries

The following remain deliberately deferred rather than hidden: Ceres
licensing/dependency integration, metric alignment, persistent orchestration
and durable SfM checkpoints. Gate E freezes its own robust loss, convergence,
ordering and acceptance policy here without introducing persistence or a
fingerprint.

## Gate C — pure calibrated geometry

**GATE C — PASS.** Pure calibrated geometry primitives, synthetic ground truth,
degeneracy rejection, determinism, normal suite and ASan/UBSan validation are
complete. Incremental orchestration, BA and persistent geometry integration
remain later gates.

Gate C keeps geometry outside Project DB and exposes a C17-safe, synchronous
pure-primitive boundary. Inputs are binary64 calibrated pixels, fixed
world-to-camera poses, and caller-owned correspondence arrays; no primitive
opens SQLite, reads Feature Files, loads images or invokes the Task Runtime.
The v1 candidate uses OpenCV 5.0.0 `calib3d` operations with every scientific
parameter supplied by an explicit configuration structure. Public outputs use
row-major binary64 `R_cw` and `t_cw`; relative translation has unit norm and no
metric interpretation.

The candidate contract requires deterministic caller ordering, finite inputs,
explicit robust-estimator thresholds/confidence/iteration limits and a local
seed. Essential hypotheses are accepted only after explicit positive-depth
support, rotation validation, parallax and reprojection checks. Pure rotation,
low parallax, weak conditioning and non-finite results are failures. Two-view
and multi-view points use normalized-coordinate linear DLT followed by bounded
point-only binary64 refinement; PnP returns world-to-camera pose with explicit
cheirality and inlier diagnostics. The tested v1 parameter set is frozen by the
Gate C ground-truth and degeneracy evidence; future orchestration may choose
other explicitly fingerprinted configurations.

### Gate C tested threshold set

The pure API has no hidden defaults; callers provide all acceptance settings.
The Gate C reference matrix uses the following reproducible set:

| Parameter | Value | Unit/purpose |
|---|---:|---|
| Relative robust threshold | 1.0 px clean; 1.5 px matrix | pixel residual |
| Relative confidence | 0.999 | RANSAC confidence |
| Relative iterations | 1000 clean; 1500 matrix | iterations |
| Relative minimum inliers | 6 clean; 24 matrix | correspondences |
| Relative minimum ratio | 0.75 clean; 0.5 matrix | fraction |
| Minimum parallax | `1e-4` rad | seed geometry |
| Minimum cheirality ratio | 0.5 | positive depth |
| PnP threshold | 1.0 px clean; 1.5 px matrix | pixel residual |
| PnP confidence | 0.999 | RANSAC confidence |
| PnP iterations | 1000 | iterations |
| PnP minimum inliers | 6 clean; 12 matrix | correspondences |
| PnP minimum ratio | 0.75 clean; 0.5 matrix | fraction |
| Point refinement tolerance | `1e-12` | normalized residual |
| Point refinement iterations | 30 | iterations |

Degeneracy checks use finite values, positive depth, rotation SO(3) residual
`1e-6`, depth epsilon `1e-9`, homogeneous scale epsilon `1e-12`, and
collinearity covariance determinant `1e-10`. These are pure-geometry
parameters and do not alter Project DB identity.

## Gate D — incremental Sparse SfM core

**GATE D — PASS / FROZEN.** Gate D is the first executable link
between the immutable Track/Calibration contracts and the Gate C primitives.
The reference implementation is synchronous, deterministic, CPU-only,
in-memory, bounded and independent of Project DB, Task Runtime, Resource
Governor and persistence publication.

### Inputs

Gate D consumes exactly one immutable Track Set, one immutable calibration
scope, finite calibration values for participating images, bounded keypoint
coordinates addressed by `(feature_set_id, feature_index)`, and explicit
parameters immutable during execution. Gate D neither computes nor carries the
parameter fingerprint materialized later at the Gate F persistent-identity
seam. The Track Set is never mutated.

### Algorithm

The core sorts image and Track identities, builds sparse connected components,
orders seed candidates by shared Track count and image IDs, and tries a bounded
number of seeds. Each candidate uses the Gate C relative-pose, cheirality,
parallax and two-view triangulation contracts. A valid seed establishes a
component-local unit gauge.

Unregistered images are then ordered by visible accepted-landmark count and
image ID. Gate C calibrated PnP registers at most one selected image per
bounded round. Failed registration leaves the image explicitly unregistered.
New landmarks use all currently registered observations, Gate C multi-view DLT
and bounded point-only refinement. A landmark is accepted or rejected as a
whole; Track observations are never dropped or rewritten.

After each successful camera registration, an existing landmark whose Track
has gained registered observations is reconsidered in canonical image-ID
order. Gate C multi-view triangulation and point refinement use the complete
eligible observation set. The replacement is published in memory only after
finite-value, positive-depth and reprojection validation; otherwise the prior
valid landmark and its observations remain unchanged.

The Gate D reference bounds are 4096 input images, 250,000 Tracks, 1,000,000
observations, 32 seed candidates, 32 registration rounds and 4096 new
landmarks per growth round. The defaults use 1.5 px relative-pose/PnP robust
thresholds, a 2.0 px landmark reprojection threshold, 0.5 minimum inlier
ratios, `1e-4` rad minimum parallax, 6 minimum seed/PnP inliers and 30
point-refinement iterations. These are Gate D policy defaults; changing them
changes the explicit parameter configuration.

### Output and failure semantics

The in-memory result contains deterministic components, registered cameras,
accepted landmarks, landmark observations, reprojection diagnostics and
explicit unregistered images. Results are `COMPLETE`, `PARTIAL` or `FAILED`.
Invalid input fails before computation. A rejected seed, camera or landmark
does not corrupt an accepted model. No partial result is persisted.

Growth stops immediately when a complete registration round cannot register
an image. It also stops exactly at the configured registration-round bound.
Both paths retain valid cameras and landmarks, list every remaining image as
unregistered and produce `PARTIAL` when usable geometry exists.

Components with fewer than two registered cameras are not valid 3D components.
Disconnected valid components retain independent unit gauges and are never
globally aligned by Gate D.

### Gate D limits

Gate D does not implement BA, persistence adapters, Task Runtime, checkpoints,
Governor integration, a Resource System, GPU execution, dense reconstruction,
metric alignment, viewer integration or any Project DB change. BA remains the
separate PASS / FROZEN Gate E post-processing stage; project/task orchestration
remains Gate F and resource/freeze integration remains Gate G.

### Canonical Gate D functional matrix

This table freezes the complete numbered validation contract. Evidence is the
minimum dedicated observation required; an earlier rejection never substitutes
for the named path.

| Case | Purpose | Required path and evidence | Expected result |
|---|---|---|---|
| 01 Minimal two-view | Smallest valid reconstruction | One seed, two cameras, finite landmarks | `COMPLETE` |
| 02 Deterministic seed | Canonical seed identity | Same selected pair and pose on repeat | `COMPLETE` |
| 03 Multiple seed candidates | Candidate ordering | Multiple eligible pairs, canonical first pair | `COMPLETE` |
| 04 Rejected first seed / later seed | Seed fallback | At least two attempts, later pair selected | `COMPLETE` |
| 05 Camera-addition order | Registration ordering | Highest support then image ID, one per round | `COMPLETE` |
| 06 Clean PnP | Nominal registration | PnP attempted and succeeds with clean support | `COMPLETE` |
| 07 Noisy PnP | Bounded noise | PnP succeeds with finite pose | `COMPLETE` |
| 08 Deterministic PnP outliers | Robust registration | Stable inlier count and pose | `COMPLETE` |
| 09 Failed PnP | Registration rejection | Failure counted and image listed | `PARTIAL` |
| 10 Insufficient PnP support | Eligibility bound | Solver not called and image listed | `PARTIAL` |
| 11 Low-parallax rejection | Seed guard | Gate C low-parallax/degenerate status | `FAILED` |
| 12 Pure rotation | Translation degeneracy | Relative pose rejected, no camera | `FAILED` |
| 13 Planar degeneracy | Ambiguous seed | Gate C degeneracy, no camera | `FAILED` |
| 14 Far scene | Finite distant geometry | Seed and finite landmarks accepted | `COMPLETE` |
| 15 Disconnected graph | Component discovery | Valid component plus explicit singleton | `PARTIAL` |
| 16 Multiple valid components | Isolation | Two reconstructed components | `COMPLETE` |
| 17 Independent gauges | Per-component gauge | Each seed camera is identity | `COMPLETE` |
| 18 Unregistered images | Explicit output | Remaining image and component key listed | `PARTIAL` |
| 19 Behind-camera landmark | Cheirality | Exact Gate C status and distinct counter | `COMPLETE` model |
| 20 High reprojection error | Residual policy | Finite triangulation then residual rejection | `COMPLETE` model |
| 21 Failed triangulation | Geometry failure | Finite input calls triangulation and fails | `COMPLETE` model |
| 22 Repeated observations | Track coherence | Duplicate image or feature reference rejected | `INVALID_ARGUMENT` |
| 23 Many-camera Track | Landmark lifecycle | One landmark, six ordered observations | `COMPLETE` |
| 24 New landmark after registration | Incremental growth | Ineligible Track accepted after PnP | `COMPLETE` |
| 25 Multi-view growth | All eligible views | New landmark uses at least three views | `COMPLETE` |
| 26 Point refinement | Bounded refinement | Attempt and finite accepted point | `COMPLETE` |
| 27 No-growth termination | Progress bound | One zero-progress round and diagnostic | `PARTIAL` |
| 28 All-images termination | Natural completion | All images registered, no stop diagnostic | `COMPLETE` |
| 29 Seed exhaustion | Candidate bound | Every available candidate attempted | `FAILED` |
| 30 Registration-round exhaustion | Round bound | Exact rounds and remaining images | `PARTIAL` |
| 31 Component ordering | Canonical components | Increasing component keys | success |
| 32 Camera ordering | Canonical cameras | Increasing image IDs | success |
| 33 Landmark ordering | Canonical landmarks | Increasing `(component_key, track_id)` | success |
| 34 In-process repeatability | Local determinism | Complete scientific result equality | same status |
| 35 Fresh-process repeatability | Process determinism | 20 runs emit one signature | same status |

### Gate D validation responsibility

`GATE_D_REQUIRED` covers pointer/count coherence, identities carried by this
API, finite calibration/keypoints, feature-index bounds, Track observation
coherence, geometry failures, atomic result ownership and cleanup. Store-level
Feature Set/File existence is `UPSTREAM_RESPONSIBILITY`: Gate D receives
flattened validated coordinates and never opens a store. Two separate Track
objects with the same ID are `UNREPRESENTABLE_BY_API` because rows are grouped
by `track_id`; duplicate image observations and feature references remain
representable and are rejected. Allocation-failure injection is
`NOT_APPLICABLE_WITH_PROOF`: no allocator injection boundary exists, production
catches allocation failure at the C ABI, and global test allocator state would
violate the architecture.

| Condition | Classification |
|---|---|
| Null parameters, missing arrays, empty input | `GATE_D_REQUIRED` |
| Zero Track/calibration/image/feature identity | `GATE_D_REQUIRED` |
| Missing per-image calibration coverage | `GATE_D_REQUIRED` |
| Zero/non-finite focal or distortion, invalid principal point | `GATE_D_REQUIRED` |
| Invalid feature index or non-finite keypoint | `GATE_D_REQUIRED` |
| Duplicate image/feature observation or singleton Track | `GATE_D_REQUIRED` |
| Seed/PnP/landmark failures and update rollback | `GATE_D_REQUIRED` |
| Missing Feature Set/File in persistent storage | `UPSTREAM_RESPONSIBILITY` |
| Two distinct Track objects sharing one ID | `UNREPRESENTABLE_BY_API` |
| Deterministic allocation-failure injection | `NOT_APPLICABLE_WITH_PROOF` |

The caller retains all input allocations for the synchronous call. The result
owns its arrays; `lardon3d_sparse_incremental_result_destroy()` releases them
and accepts an empty result or null pointer. No C++ exception crosses the C17
boundary.

Count-limit validation uses structurally sufficient fixtures at a lowered
explicit configured limit and proves `LIMIT-1`, `LIMIT`, and `LIMIT+1` without
materializing the public hard maxima. Scientific scale is validated separately
by the small, medium and large resource workloads. Policy tests prove exact
seed-candidate, registration-round and new-landmark-per-round admission; no
policy loop performs a `limit + 1` attempt.

## Out of scope

Beyond the Gate D incremental core, no BA, Project DB integration, metric
alignment, control-point scale, dense/MVS, mesh, texturing, Vulkan SfM, GPU BA,
network/distributed scheduling or UI workflow is implemented here.

## Gate B — model and persistence contract

This section is the **DECISION** contract for the v16 SQL/API work.
It preserves every Gate A decision and supplies only durable vocabulary; no
numeric geometry is introduced.

### Calibration definition and identity

A calibration is an immutable known pinhole model with `width`, `height`,
`fx`, `fy`, `cx`, `cy`, zero skew, `k1`, `k2`, `p1`, and `p2`, all binary64 and
finite. `width` and `height` belong to scientific identity. A calibration's
provenance is an explicit enum (`USER_EXPLICIT` or `IMPORTED_TRUSTED` in v1)
plus a 32-byte provenance fingerprint supplied by the caller. EXIF is never a
calibration origin. Two equal numeric models with different provenance
fingerprints are distinct scientific calibrations because their trust scope is
different; equal content and equal provenance are reused.

The scientific calibration hash is SHA-256 over explicit little-endian fields:

```text
ASCII "L3D3DCP1" (8 bytes)
format_version=1 (uint32)
model_kind (uint32), model_version (uint32)
width (uint32), height (uint32)
fx, fy, cx, cy, k1, k2, p1, p2 (8 canonical binary64 values)
provenance_kind (uint32), provenance_fingerprint (32 bytes)
```

NaN and infinities are rejected. Negative zero is canonicalized to positive
zero before hashing and storage. No native struct, padding or locale text is
serialized. SQLite row IDs remain DB-local references; the hash is the
scientific calibration identity.

### Calibration scope

A scope is immutable and assigns exactly one calibration to each relevant
image. Groups are allowed only when dimensions, crop/orientation coordinate
frame, model/version, numeric parameters and provenance identity are equal;
device or EXIF model names are insufficient. Scope identity is project-local
and content-addressed by SHA-256 over:

```text
ASCII "L3D3DSC1" (8 bytes)
format_version=1 (uint32)
member_count (uint64)
for members sorted by image_id:
    image_id (uint64), calibration_hash (32 bytes)
```

The member count is consistency metadata and is also encoded in the digest.
There is no latest-calibration lookup and no silent K rescaling. Feature File
dimensions must match the calibration dimensions exactly.

### Reconstruction identity and components

The immutable reconstruction identity is:

```text
(track_set_id,
 calibration_scope_id,
 sfm_kind=INCREMENTAL,
 sfm_version=1,
 parameter_fingerprint[32])
```

`track_set_id` and `calibration_scope_id` are project-local immutable database
references, consistent with the existing Track Model identity convention. The
parameter fingerprint is the raw 32-byte SHA-256 digest of the 372-byte Sparse
SfM parameter record v1 defined above. Gate F materializes it at its
persistent-identity seam; Project DB stores it without owning or recomputing
it. Runtime IDs, timestamps, worker count, resource state and metrics are
excluded.

Component identity is the minimum registered `image_id` in that component. It
is deterministic, project-local, unique because an image belongs to at most one
component, independent of DFS/hash order, and compact. A component always has
at least one registered image. Its coordinates use an independent unit-baseline
gauge and are never comparable to another component without later alignment.

### Persisted model

The minimum v16 model is:

- `sparse_calibrations`: immutable calibration content and hash;
- `sparse_calibration_scopes`: immutable scope hash/member count;
- `sparse_calibration_scope_images`: one image-to-calibration assignment;
- `sparse_reconstructions`: immutable identity and pixel reprojection metrics;
- `sparse_reconstruction_components`: component key and counts;
- `sparse_registered_images`: one world-to-camera pose per image;
- `sparse_landmarks`: one component-local binary64 point per Track;
- `sparse_landmark_observations`: minimal references to the upstream Feature Set
  and feature index, without duplicated descriptors or x/y coordinates.

Track ID is globally unique in the existing Track Model table, so one landmark
per reconstruction is uniquely keyed by `(reconstruction_id, track_id)`;
component key remains an attribute/consistency relation rather than redundant
landmark identity. Landmark publication validates that the Track belongs to the
reconstruction's exact Track Set and that every observation belongs to that
Track and its component. Track splitting is impossible in v1.

Observation references are persisted because future BA needs bounded indexed
access from landmark to registered observations without repeatedly reopening
Feature Files. Only `feature_set_id`, `feature_index` and canonical track
position are stored; image ID and x/y remain derivable from immutable upstream
models. This is a deliberate normalization/resource trade-off, not a second
copy of Feature data.

Persisted reprojection metrics use explicit pixel units and names:
`reprojection_rmse_px` and `reprojection_median_px`. They are diagnostics, not
identity. No metric scale or `_mm` field exists.

### Constraints and publication

Publication requires at least two registered images, one component and one
landmark. A result with no usable geometry is rejected rather than represented
by a meaningless empty scientific row. Two-camera reconstruction is valid.
There are no READY/FAILED/PARTIAL scientific states: row existence means a
complete immutable publication. A failed transaction leaves no visible row.

SQL enforces one pose per image, one component per registered image, one
landmark per Track, exact reconstruction uniqueness, scope member uniqueness,
and child foreign keys. The API additionally validates Track Set ownership,
component consistency, calibration coverage, finite values and rotation
orthonormality/determinant. Rotation matrices are never repaired.

The publication transaction inserts the reconstruction and all children using
prepared statements in bounded loops. It contains no Feature File I/O, Track
paging, solver work or Governor wait. Child pages use cursor order and bounded
capacity (64 cameras, 64 landmarks, 64 observations); total scientific counts
have no arbitrary cap beyond checked 64-bit/SQLite limits.

### v16 migration intent

Project DB v15 remains immutable. Gate B adds one transactional v15→v16
migration containing only the eight Sparse SfM model tables and their required
indexes. A true historical v15 fixture, injected rollback, retry, fresh-schema
equivalence and close/reopen are mandatory. No Task, Governor, triangulation,
PnP, BA, GPU or Project DB v17 is introduced.
