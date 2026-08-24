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

**DECISION: incremental SfM with bounded local refinement and optional final
global refinement.** It matches the expected sequential vehicle/phone capture,
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

## Bundle Adjustment decision

**DECISION: BA is required for a useful final reconstruction but is not part of
the first pure-geometry gate.** The later BA gate will use a block-sparse
camera/landmark problem with binary64 poses and points, fixed or explicitly
fingerprinted calibration variables, and a robust loss whose kind/scale belong
to scientific identity. Dense camera×landmark Jacobians are forbidden.

Ceres is not available in the current host pkg-config environment and is not a
Lardon3D production dependency. It is a **NEW_CANDIDATE**, not silently added.
Its sparse Schur solvers and block structure make it the leading BA study
candidate; Eigen is host-available, while SuiteSparse is not detected. A later
gate must prove license, reproducibility, thread behavior, memory scaling and
fallback before adding Ceres. OpenCV remains appropriate for small relative
pose/PnP/triangulation primitives, not as an implicit BA architecture.

The first BA implementation should be local-window BA after registration,
followed by at most one explicitly admitted global BA at finalization. No two
heavy global BAs run concurrently. Global BA is allowed to be deferred by the
Resource Governor. The trigger, window selection, robust loss, convergence and
thread policy are configuration fields, not runtime identity.

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
loss parameter belongs in the future fingerprint.

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

Triangulation/registration are light CPU units and can be batched. Local BA is
bounded by an active camera/landmark window and needs a Governor reservation.
Global BA is one heavy job at a time, with explicit admission and a conservative
thread cap. Future reservation inputs are `C`, `P`, `O`, active window size,
solver mode and calibration-variable count. The existing Resource Governor
owns RAM/PSI/swap policy; Sparse SfM adds no thresholds. Swap is never normal
working memory, and UMA RAM must preserve several GiB of desktop/iGPU headroom.

## Hardware and probe study

Gate A preflight measured 16 logical CPUs, `MemTotal=15597716 KiB`,
`MemAvailable=8245288 KiB` at the study point, an 8 GiB swapfile, a 6 GiB
zram device, and zero current memory/IO PSI average. The host is the Ryzen 7
8845HS/Radeon 780M UMA target described by the performance document.

The project already links OpenCV 5.0.0. Eigen 5.0.1 and Ceres 3.12.0 are
available through host pkg-config but are not current production dependencies;
SuiteSparse/BLAS/LAPACK availability is host capability only. TBB 2023.1 is
present through the existing OpenCV stack. No package, system setting, swap
device or GPU mode was changed.

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
- **Gate E — BA integration:** sparse BA candidate, robust loss, local/global
  policy, numerical reproducibility and solver dependency decision.
- **Gate F — Project orchestration:** explicit Track Set/calibration input,
  atomic publication and durable runtime integration.
- **Gate G — Resource/freeze:** Governor admission, sustained hardware safety,
  recovery, full validation and final freeze.

## Algorithm comparison and Gate A evidence

### Incremental SfM

Seed/order risk is controlled by deterministic policy. It is robust for
sequential capture, has canonical queues and seeds, moderate complexity, and
sparse `C,T,O` scaling with local BA. **SELECTED v1.**

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
| Ceres sparse Schur | Appropriate block structure | New candidate dependency | **Later-gate candidate** |

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
facilities rather than current Lardon3D production dependencies. Ceres and
SuiteSparse are not available through the current host pkg-config environment;
Ceres remains a new dependency candidate, not an installed fact. No new
dependency is added by Gate A. The measured machine has 16 logical CPUs,
`MemTotal=15597716 KiB`, `MemAvailable=8245288 KiB` at preflight, an 8 GiB
swapfile, 6 GiB zram and zero memory/IO PSI averages at the probe start. A
single heavy BA and a future solver thread cap of 4 are the conservative
resource candidates; these are not yet Governor settings.

## Gate A unresolved boundaries

The following remain deliberately deferred to later gates rather than hidden:
exact numeric parallax/reprojection thresholds, Ceres licensing/dependency
adoption, robust-loss scale, local-window selection, BA convergence criteria,
metric alignment, persistent reconstruction schema and durable SfM checkpoints.
Their semantic ownership is decided here; their final numeric values require
the synthetic ground-truth and sparse-solver gates.

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

**GATE D — PASS.** Gate D is the first executable link
between the immutable Track/Calibration contracts and the Gate C primitives.
The reference implementation is synchronous, deterministic, CPU-only,
in-memory, bounded and independent of Project DB, Task Runtime, Resource
Governor and persistence publication.

### Inputs

Gate D consumes exactly one immutable Track Set, one immutable calibration
scope, finite calibration values for participating images, bounded keypoint
coordinates addressed by `(feature_set_id, feature_index)`, and explicit
parameters immutable during execution. Gate D neither computes nor carries the
future parameter fingerprint. The Track Set is never mutated.

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
later Gate E; project/task orchestration remains Gate F; resource/freeze
integration remains Gate G.

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
parameter fingerprint is required to be a 32-byte SHA-256 value but its final
byte encoding remains a later geometry-gate decision. Runtime IDs, timestamps,
worker count, resource state and metrics are excluded.

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
