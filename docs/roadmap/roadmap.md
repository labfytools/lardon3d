# Lardon3D — Canonical Roadmap

This roadmap defines the current dependency order and lifecycle of Lardon3D.

Detailed scientific, persistence and runtime contracts remain owned by the canonical documents under
`docs/architecture/`. This file records what is acquired, what is current, what is next, and what is
deliberately deferred.

It must not be used to reinterpret FROZEN scientific evidence.

## Current repository state

```text
Project DB current schema             v25
Production Task kinds                 16
GLOBAL_MAINTENANCE_AUDIT              PASS/FROZEN
REAL_S21_TRACKS                       PASS/FROZEN
REAL_A6000_PRE_SFM                    PASS/FROZEN
Sparse SfM capability Gates A-G       PASS/FROZEN
Real A6000 Sparse SfM                 NOT EXECUTED
Real A6000 Dense/MVS                  NOT EXECUTED
DOCUMENTATION_FINDING_REMEDIATION     PASS
SOURCE_COMMENT_AUDIT                  PASS
PRODUCT_DEFINITION                    PASS/FROZEN
PROMPT_TREE                           CURRENT
USER_FACING_UI_LANGUAGE_NORMALIZATION PASS
CURRENT_NEXT                          CALIBRATION_V2_HETEROGENEOUS_OPTICS_FOUNDATION
```

The current Project DB head is additive:

```text
v22  selected scientific execution foundation
v23  generic optical-context overlay
v24  raw.develop.batch/1 persistence
v25  features.extract.batch/1 persistence
```

Historical references to earlier versions remain valid when they describe the state of their own
checkpoint. They are stale only when they claim to describe the current head.

## Canonical resource policy — HUMAN AUTHORITY

Production execution follows:

```text
MAXIMUM SAFE USEFUL THROUGHPUT
SERIALISM_REQUIRES_PROOF
```

Lardon3D preserves the interactive host reserve first, then uses the maximum remaining resources that
are both safe and useful for the admitted workload.

The interactive reserve exists to preserve normal use of:

- Arch Linux / Sway and normal desktop services;
- Firefox;
- normal audio playback;
- lightweight interactive workstation use.

On the current validation host, the normal observed outcome is approximately:

```text
16 logical CPUs total
4 logical CPUs reserved for interactive host use
12 logical CPUs available to the compute pool
~3 GiB MemAvailable preserved as the hard RAM reserve
Radeon 780M UMA available to validated and useful GPU backends
```

These values are reference-host evidence, not portable product constants.

A future host with more compute capacity must not inherit the current 12-thread result as a ceiling.
The Resource Governor derives admission from the actual affinity, topology, memory state, pressure,
backend capability and Task envelope.

Per-item atomicity does not imply cross-item serialization.

Owner-only or ordered durable publication does not imply serial preparation.

A long-running `CPU1` or `batch1` path with independent work and safe idle capacity is an operational
defect unless concrete evidence establishes a real limit such as:

- scientific or dependency serialism;
- a measured useful-scaling knee;
- memory pressure or a truthful memory bound;
- I/O saturation;
- a validated GPU path that makes additional CPU work useless;
- a deterministic publication constraint that cannot safely be separated from preparation;
- another explicit measured resource limitation.

The Resource Governor remains the sole production resource authority.

Normal users do not select CPU count, worker count, batch size, inflight depth, GPU backend, scratch
mode or RAM budget for authoritative execution.

Validated and useful GPU backends are preferred automatically when eligible. A GPU backend must not
be invented or promoted merely to create GPU activity.

Swap, zram and scratch are pressure/safety or storage mechanisms. They are never admitted RAM. UMA
GPU memory is charged exactly once against host RAM.

Pressure may reduce admission. When pressure clears, safe and useful resources must be admitted again
rather than remaining permanently throttled.

The same philosophy applies to engineering work: independent builds, tests, benchmarks and real-data
proofs should use safe host-aware parallelism. Historical fixed values such as `-j8` or
`--num-processes 1` are not universal policy.

## Acquired FROZEN foundations

The following major boundaries are acquired at the scope defined by their canonical documents:

- Sparse SfM Gates A-G — PASS/FROZEN;
- F0 — PASS/FROZEN;
- Track Model — PASS/FROZEN;
- Track Builder scientific contract — PASS/FROZEN;
- Phase H v1 incremental multi-snapshot enrichment — PASS/FROZEN;
- MVS-M1 external OpenMVS v2.4.0 boundary — PASS/FROZEN;
- Capture / Asset Provenance — PASS/FROZEN;
- Capture-safe Standard Ingestion — PASS/FROZEN;
- Capture / Acquisition Ingestion — PASS/FROZEN;
- Durable Acquisition-Campaign Execution — PASS/FROZEN;
- Selected Scientific Execution — PASS/FROZEN;
- Photo Quality Triage / Acquisition Selection — PASS/FROZEN;
- Calibration Bootstrap v1 — PASS/FROZEN;
- Calibration Science v1 — PASS/FROZEN;
- Calibration Tooling v1 — PASS/FROZEN;
- Internal Parallelism + Compute Resources v1 — PASS/FROZEN;
- Compute Governor v2 — PASS/FROZEN;
- ORB Vulkan asynchronous execution — PASS/FROZEN;
- Global Maintenance Audit — PASS/FROZEN;
- Real S21 Tracks — PASS/FROZEN;
- Real A6000 pre-SfM — PASS/FROZEN.

A FROZEN boundary is not reopened merely because a later operational document, benchmark or roadmap
section is updated.

## Capture and acquisition foundation

The acquisition model preserves distinct identities:

```text
Capture != file
Capture != asset
Capture != image_id
Capture != SHA-256
Capture != basename
Capture != Task ID
Capture != campaign group ID
```

`capture_id` identifies the physical acquisition representation recorded in Project DB.

`image_id` identifies a scientific image representation.

SHA-256 identifies immutable asset bytes.

Task and campaign-group IDs are operational identities and never redefine the scientific Capture.

The acquisition pipeline remains conceptually:

```text
explicit acquisition sources
-> bounded discovery / metadata
-> acquisition candidates and explicit confirmations
-> Photo Quality Triage
-> explicit acquisition selection / human override
-> durable campaign execution
-> Capture / Asset provenance
-> selected scientific representation
-> downstream scientific pipeline
```

A6000 RAW+JPEG pairs confirmed by the caller are `CALLER_EXPLICIT`, never silently upgraded to
scientifically `STRONG` evidence.

Historical S3 and Project DB v19/v20 evidence remains valid for the lifecycle it recorded. Its
historical crash-window limitations must not be rewritten as if later durable execution semantics
already existed at that checkpoint.

## Photo Quality Triage / Acquisition Selection — PASS/FROZEN

Photo Quality Triage operates on existing acquisition groups before normal campaign materialization
and before scientific representation, Feature extraction, matching, SfM or MVS.

It produces explainable recommendations:

```text
GOOD
SUSPECT
REJECT
```

These are operational recommendations, not identities and not source deletion.

A6000 RAW+JPEG acquisition pairs are one triage unit. A valid camera JPEG is the preferred fast
quality proxy, while the FROZEN A6000 geometry representation for the retained selected execution
remains the deterministic RAW-derived PNG.

A S21 singleton JPEG is naturally usable as its quality-analysis representation.

Quality selection never permits silent scientific calibration inference or silent replacement of the
selected geometry representation.

## Selected scientific execution — PASS/FROZEN

Selected scientific execution freezes the exact ordered set of selected acquisition items and their
scientific representation policy.

For the retained A6000 Engine Bay execution:

```text
quality selection
-> durable selected execution
-> deterministic RAW development
-> Feature extraction
-> Visual Index
-> Candidate Pair generation
-> Matcher
-> Geometric Verification
-> Tracks
-> STOP before real Sparse SfM
```

The paired camera JPEG remains a valid SOURCE asset and fast quality proxy.

The geometry representation remains the deterministic RAW-derived PNG identified by the established
RAW policy and `L3DRAWD1`.

Replacing that geometry representation with camera JPEG requires a separately authorized scientific
and calibration equivalence proof.

## Project DB evolution

### v22 — selected scientific execution foundation

Project DB v22 owns the selected scientific execution foundation and associated persistence semantics.

It remains PASS/FROZEN.

### v23 — generic optical-context overlay

Project DB v23 adds generic optical context without rewriting historical scientific rows.

It supports explicit camera-body profiles, lens profiles, optical configurations, campaign/Capture
assignments and exactly compatible calibration selection.

Manual lenses without EXIF are normal supported equipment.

No silent lens identity inference, calibration substitution, interpolation or backfill is allowed.

### v24 — RAW batch persistence

Project DB v24 adds typed persistence for:

```text
raw.develop.batch/1
raw_development_batch_tasks
```

The production pattern is:

```text
one Governor-admitted owner Task
    -> bounded independent RAW participants
    -> join all participants
    -> deterministic owner-only publication
    -> ordered selected-execution cursor advancement
```

Per-Capture atomicity remains preserved while independent Captures may be prepared concurrently.

The retained real A6000 proof completed the RAW-batch path. Project DB v24 is therefore no longer in validation.

### v25 — Feature batch persistence

Project DB v25 is the current schema head.

It adds typed persistence for:

```text
features.extract.batch/1
feature_extract_batch_tasks
```

The historical `features.extract/1` path remains valid.

The batch path preserves per-image scientific atomicity while allowing bounded cross-image
preparation. Durable publication, cursor advancement and checkpoint ordering remain owner-controlled
and deterministic.

The retained real A6000 proof completed the Feature-batch path. v25 is therefore no longer
"validation in progress".

## Historical A6000 v24 cursor checkpoint — SUPERSEDED CURRENT STATE

The following state is retained only as historical progress evidence:

```text
Project
/home/fy59/Documents/Lardon/.real-pre-sfm-2026-09-01/a6000-pre-sfm-v23-final

Schema                         v24
Captures                       953
GOOD                           689
SUSPECT                        206
REJECT                         58
selected_execution             1
selected items                 689 RAW_ASSET
published representations      259
next_item_index                259
remaining RAW representations  430
Features                       0
Visual Index                   0
Candidate                      0
Matcher                        0
GV                             0
Tracks                         0
Sparse SfM                     0
Dense/MVS                      0
```

This snapshot must never be presented as CURRENT NEXT.

The remaining 430 RAW representations were later completed, followed by Feature extraction, Visual
Index, Candidate Pair generation and Matcher. The project subsequently advanced to the final
`REAL_A6000_PRE_SFM=PASS/FROZEN` checkpoint below.

The historical source-inventory digest remains:

```text
fa725d8a82b529521134dd600b5cf42ed58ad523108636741a1431441e17b029
```

## Global Maintenance Audit — PASS/FROZEN

Canonical maintenance checkpoint:

```text
tag     global-maintenance-2026-09-01
commit  b84f860d868c66d9ee84b85ceb1bc6480b95aca5
```

Detailed evidence is retained in
[Global Maintenance Audit](../architecture/global_maintenance_audit.md).

Future reviews are delta-based from this checkpoint for unchanged FROZEN systems:

```sh
git diff global-maintenance-2026-09-01...HEAD
```

A later checkpoint may add evidence without erasing this maintenance authority.

The maintenance audit must not be repeated A-to-Z without concrete evidence that an unchanged
boundary needs to be reopened.

## Real S21 Tracks — PASS/FROZEN

```text
REAL_S21_TRACKS=PASS/FROZEN
```

Retained source project:

```text
/home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-31/s21-gv-v3
```

The retained real proof contains:

```text
Feature Sets             2,826
Candidate Pairs          172,741
Match Results            172,741
GVR v3                   172,275
GEOMETRIC_VERIFIED        24,065
GEOMETRIC_REJECTED       148,210
Track Sets                     1
Tracks                    912,447
Track observations      2,495,768
Track length min/max         2/42
```

Canonical persistent Track digest:

```text
c30eba192627bf73eaf21ff30d81038d8cc6bbf36a69226f88cdc8c37f7d74a1
```

The historical 18.204 GiB Track Builder envelope is retained only as rejected historical evidence.
The validated compact memory model supersedes it operationally.

The proof used no scratch lease.

Sparse SfM and Dense remained unexecuted in this retained real S21 Tracks checkpoint.

## Real A6000 pre-SfM — PASS/FROZEN

Canonical later checkpoint:

```text
tag  real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Authoritative durable project:

```text
/home/fy59/Documents/Lardon/.real-pre-sfm-2026-09-01/a6000-pre-sfm-v23-final
```

The directory name is historical. The current Project DB inside the retained final project is v25.

The continuation reused the already acquired upstream results:

```text
ACQUISITION_REPLAY=0
RAW_REPLAY=0
FEATURE_REPLAY=0
VISUAL_INDEX_REPLAY=0
CANDIDATE_REPLAY=0
MATCHER_REPLAY=0
```

Final retained counts:

```text
Selected images         689
Feature Sets            689
Candidate Pairs         38,420
Match Results           38,420
Applicable GVR          37,805
GEOMETRIC_VERIFIED      10,952
GEOMETRIC_REJECTED      26,853
Track Sets                   1
Tracks                  130,714
Track observations      318,944
```

Geometric Verification v3 fingerprint:

```text
6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

The GV continuation completed its durable Match Result cursor and produced zero duplicate GVR
mappings.

Track Builder published one atomic Track Set. SQL audit established:

```text
duplicate Track observations       0
repeated-image Tracks              0
orphan Track observations          0
```

The restart continuation traversed the already completed GVR cursor, produced zero new GVRs and
reused Track Set 1 without creating duplicate Tracks.

The retained Governor evidence includes:

```text
admissions                     2,714
compute-pool CPUs                  12
interactive CPU reserve             4
final GV window                    16
swap-in delta                       0
swap-out delta                      0
```

The reference-host CPU counts are evidence, not portable constants.

This checkpoint explicitly stopped with:

```text
sparse_sfm_tasks             0
sparse_reconstructions       0
Dense/MVS                    0
multi-campaign fusion        0
```

Therefore this checkpoint proves the A6000 pre-SfM boundary only.

It does not authorize inferred calibration or imply that real Sparse SfM has run.

## Calibration status

Historical S21 and A6000 Engine Bay campaigns remain `CALIBRATION_UNAVAILABLE` for the known-
calibration Sparse SfM contract.

This status does not mean source failure, image-quality rejection or software failure.

It forbids bypassing the scientific requirement through:

- pseudo-calibration;
- EXIF-as-calibration;
- silent metadata interpolation;
- silent lens identity inference;
- silent calibration substitution.

The resulting historical-campaign state remains:

```text
BLOCKED_BY_KNOWN_CALIBRATION_DATA
```

S21 Engine Bay is permanently non-retro-calibrable under Calibration Science v1.

Calibration Science v1 defines the frozen physical protocol for its compatibility path.

`CALIBRATION_SCIENCE_V2=PASS/FROZEN`: the generic additive contract now establishes
heterogeneous optical states, adaptive photometric controls and evidence-bound autofocus
applicability without changing Science v1. Real A6000 engine-bay acquisition evidence established
that normal operation must support autofocus and heterogeneous optical states without forcing the
operator to maintain one locked focus or one optical configuration for the whole project.

Science v2 is additive and must preserve these product invariants:

```text
one project may mix camera bodies, lenses and focal configurations
one selected execution may use different calibrations per image
unknown calibration state retains the Capture and reports CALIBRATION_REQUIRED
shutter/ISO/white-balance values are adaptive, not image-validity constants
image quality is judged from actual decoded-image evidence
autofocus is a normal supported mode when its applicability is physically validated
silent calibration substitution remains forbidden
```

The current Sparse calibration scope already models membership as `image_id -> calibration_id`; the
next dependency is the additive durable heterogeneous-optics/applicability foundation, which must
reuse that capability where it proves sufficient. Device-specific autofocus envelopes remain blocked
until physical evidence validates them.

Calibration Tooling v1 consumes an already acquired Science v1 evidence bundle, validates the bounded
contract and produces deterministic `L3DCALB1` v1.

Calibration Bootstrap v1 imports the explicit artifact.

Calibration Evidence Solver v1 implements the external OpenCV 5.x evidence path selected by
Calibration Solver Preflight v1. It remains outside the Lardon3D runtime and Project DB and its
deterministic synthetic CPU1 self-test is validated.

The solver session contract now also requires explicit measured `white_border >= 30 mm` evidence. This closes the previous gap between the physical target requirement and the future coordinator input; no default border width is inferred.

`CALIBRATION_SOLVER_PRODUCER_IDENTITY_V1=PASS/FROZEN`: the solver bundle now includes deterministic `producer.json` evidence binding the exact solver executable, canonical solver configuration, session manifest, OpenCV build, CPU1 policy and optical-state SHA-256.

`CALIBRATION_SOLVER_PER_VIEW_EVIDENCE_V1=PASS/FROZEN`: `detection.json` now retains the exact per-view Science v1 classifications and residual-support evidence required by the future workflow coordinator; downstream code must consume rather than recompute them.

`CALIBRATION_SOLVER_BUNDLE_REPAIR_V1=PASS/FROZEN`: retained per-view fields are now present in the actual solver output, `producer.json` is valid canonical JSON, solver-configuration records contain real line feeds, and the deterministic self-test checks these contents rather than byte identity alone.

`CALIBRATION_WORKFLOW_INPUT_BOUNDARY_V1=PASS/FROZEN`: the workflow now has a bounded non-mutating input boundary for session, solver bundle and campaign optical-state evidence. It rejects special/symlink/oversize files, invalid JSON, provenance digest mismatches and incompatible optical state. The next boundary is Evidence Materialization v1.

`CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION_V1=PASS/FROZEN`: the validated external calibration inputs are now materialized into bounded Science v1 target, per-view, coordinate, repeated-solve, fit, residual, hold-out and provenance evidence without Project DB access or mutation. The next boundary is exact Selected Execution Binding v1 before any Tooling/Bootstrap import.

`CALIBRATION_WORKFLOW_SELECTED_EXECUTION_BINDING_V1=PASS/FROZEN`: a read-only coordinator boundary now proves the exact selected execution and Capture mapping, explicit optical configuration, managed representation size/SHA-256, safe project-relative file containment, decoded geometry dimensions, and deterministic ToolingEntry construction. It never invokes Tooling/Bootstrap or changes selected-execution state. The next boundary is the FROZEN Tooling -> Bootstrap -> truthful READY composition.

`CALIBRATION_WORKFLOW_TOOLING_BOOTSTRAP_READY_V1=PASS/FROZEN`: valid retained external evidence now composes through the three workflow checkpoints and only the FROZEN Tooling/Bootstrap importer to truthful READY. This software proof does not acquire a physical calibration; historical S21/A6000 campaigns remain CALIBRATION_UNAVAILABLE.

A bounded Tooling correction aligned planarity handling with Calibration Science v1: Science v1
defines a categorical physical planarity attestation, not a numeric flatness threshold. Tooling
therefore rejects invented finite `target_flatness_mm` values. `L3DCALB1` v1 and Calibration
Bootstrap remain unchanged.

The current implementation dependency is the calibration workflow coordinator:

```text
immutable session.l3dcal
+ detection.json
+ solve.json
+ evidence.json
+ exact selected execution
+ exact optical state
-> bounded Calibration Tooling evidence
-> L3DCALB1 v1
-> Bootstrap
-> READY
```

No calibration component silently turns metadata into scientific calibration.

## Current engineering work

The current repository-maintenance sequence is:

```text
Documentation Inventory Audit       PASS_WITH_FINDINGS
Documentation Finding Remediation   PASS
Source Comment Audit                PASS
Source Comment Remediation          PASS
Product Definition v1               PASS/FROZEN
Prompt execution contract tree      CURRENT
User-facing UI/control normalization PASS
-> final usable calibration workflow CURRENT NEXT
-> each implementation tranche requires explicit human authorization
```

The repository contract phase is complete and the implementation cursor has moved to the final usable calibration workflow.

This cursor does not itself authorize:

- scientific threshold changes;
- new Project DB schema versions;
- new Task kinds;
- real Sparse SfM execution;
- Dense/MVS execution;
- Viewer implementation;
- live-capture implementation;
- Capture Guidance implementation.

Those boundaries require their own explicitly authorized tranche when reached.

## Scientific next dependency

The current implementation dependency is the final usable calibration workflow around the already acquired Calibration Science / Tooling / Bootstrap boundaries.

The first new real scientific evidence after that workflow exists is a dedicated physical calibration acquisition that satisfies Calibration Science v1.

The dependency order is:

```text
final usable calibration workflow
-> dedicated physical calibration acquisition
-> external solver evidence
-> Calibration Tooling v1
-> L3DCALB1 v1
-> Calibration Bootstrap v1
-> explicit compatible optical assignment
-> READY
-> real Sparse SfM
-> reconstruction validation
-> multi-campaign registration / Phase H where applicable
-> Dense / MVS
-> mesh / refinement / texturing
-> consolidation / export
```

Historical S21/A6000 Engine Bay datasets must not be silently promoted through this sequence if they
cannot satisfy the frozen calibration contract.

## Optional external SSD / scratch

The existing external SSD controller is a validated physical boundary.

It uses the exact UDisks2 Drive/label/UUID model and the established
`LARDON_SWAP` / `LARDON_SCRATCH` pairing contract.

The controller and Governor integration are:

```text
CURRENT / VALIDATED OPERATIONAL
```

SSD availability does not create Task scratch consumption by itself.

Future scratch consumers must have explicit Task ownership and acquire/release scratch only through
the validated Governor-owned lease contract.

Scratch is especially relevant to future dense, mesh, refinement and texturing workloads.

Swap remains a safety mechanism and never expands admitted RAM.

No automatic formatting, repartitioning, fsck, force-drain or destructive storage action is implied
by the roadmap.

## TUI

The TUI is the current operational control center.

It owns project workflow, Task observation, durable progress, Resource Governor observation, optical
configuration and optional SSD controls.

The canonical repository and UI language is English.

The direct TUI/control surface and user-facing project/import/catalog/runtime-session messages have completed their scoped English normalization and validation pass.

Historical persisted labels, path names and deliberate UTF-8 fixtures remain unchanged where translation would alter historical meaning or test purpose. Persistence-sensitive/internal diagnostics are changed only in an explicitly owned scope rather than by mechanical repository-wide replacement.

The TUI remains ncurses/main-thread owned according to its canonical contract.

Rich geometry visualization is a separate future boundary.

## Mixed devices and multiple ScanSets

Sony A6000, Samsung S21 and future cameras reuse the same Capture / Asset / scientific-image model.

No camera-specific fork of scientific identity is allowed.

New cameras and lenses are product onboarding concerns, not reasons to modify scientific source code.

Multiple ScanSets and selected representations may feed the existing Phase H enrichment model when
their scientific prerequisites are satisfied.

Raw S21 and A6000 projects must not be casually merged. Each campaign is reconstructed under its own
valid optical/calibration context, then registered or fused at the scientifically correct stage.

## Future product areas

The following areas remain future implementation work. Their product requirements are now frozen in [Product Definition v1](../product/product_definition.md); subsystem implementation/scientific contracts remain to be authorized and acquired.

### Dense / MVS / mesh / texture / export

MVS-M1 freezes the current external OpenMVS boundary.

Future work includes:

```text
durable dense execution
-> restart / resource / scratch ownership
-> atomic dense publication
-> mesh
-> refinement
-> texturing
-> consolidation
-> export
```

The exact publication, identity and recovery contracts must be defined before implementation.

### Viewer

The future Viewer is a passive consumer of validated project/reconstruction snapshots.

Potential visualization includes:

- sparse points;
- dense geometry;
- mesh;
- cameras and frustums;
- Track support;
- reprojection quality;
- coverage evidence;
- weak or unseen regions;
- holes;
- per-ScanSet contribution.

The final Viewer architecture and UI contract are not yet frozen.

### Offline Coverage Analysis

Coverage Analysis will estimate where additional photographic evidence is useful.

Potential evidence includes:

- observation count;
- angular diversity;
- parallax;
- projected resolution;
- image quality;
- Feature/Track support;
- reprojection / triangulation quality;
- reconstruction confidence;
- visibility and occlusion;
- ScanSet provenance.

No coverage score, weighting or threshold is currently a scientific contract.

### Suggested supplementary viewpoints

A future guidance layer may convert weak coverage into recommendations such as:

```text
weak region
+ current reconstruction evidence
-> suggested direction / angle / distance / baseline
```

Recommendations must derive from geometric evidence, not filename counts, basename similarity or
unrelated heuristics.

The optimization algorithm is not yet defined.

### Live camera localization

A live image may eventually be localized against an existing reconstruction through a bounded
camera-localization path.

Likely prerequisites include:

- valid camera calibration;
- an existing sparse reconstruction;
- live-frame Features;
- 2D-to-3D correspondences;
- calibrated pose estimation;
- pose confidence;
- graceful tracking loss and reacquisition.

No live-localization contract is currently frozen.

### Live Coverage / Capture Guidance

The target loop is:

```text
existing reconstruction
-> coverage analysis
-> weak or missing regions
-> current camera localization
-> projection into Live View
-> operator acquires supplementary images
-> normal Lardon3D ingestion
-> reconstruction update
-> coverage update
```

The operator should ultimately receive actionable guidance about viewpoint, direction, angle,
distance or baseline rather than only a colored weak region.

This remains future product intent, not implemented behavior.

### Sony A6000 acquisition boundary

The Sony A6000 must remain unmodified.

Its native outputs are the intended integration boundary:

```text
A6000 -- HDMI --> capture device --> Live View
     \-- USB  --> control / shutter / metadata / RAW+JPEG transfer where supported
```

Heavy reconstruction and analysis remain on the PC.

The device-specific adapter may handle transport and control differences, but camera-specific
transport must not redefine Capture identity or scientific contracts.

No firmware hack or hardware modification is part of the product direction.

### Samsung S21 acquisition boundary

Samsung S21/mobile acquisition will use a device-specific adapter at the acquisition boundary while
reusing the same Capture, provenance, quality, optics and scientific pipeline.

Its exact live transport/control contract remains to be defined.

### Video and deterministic keyframes

Video is a future acquisition source, not a separate SfM pipeline.

The intended shape is:

```text
SOURCE video asset
-> deterministic timeline / metadata
-> bounded deterministic keyframe extraction
-> quality / blur / redundancy filtering
-> selected frame representations / Capture candidates
-> existing Lardon3D scientific pipeline
```

Every retained frame must remain traceable to the source video and exact extraction identity.

No keyframe selection science is currently frozen.

### Optics onboarding

The final product must support new camera/lens equipment without source-code changes.

Established product intent includes:

```text
NEW_CAMERA_REQUIRES_CODE_CHANGE=NO
NEW_LENS_REQUIRES_CODE_CHANGE=NO
ELECTRONIC_LENS_WITH_METADATA=SUPPORTED
MANUAL_LENS_WITHOUT_EXIF=SUPPORTED
MULTIPLE_LENSES_PER_CAMERA=SUPPORTED
ZOOM_MULTIPLE_FOCALS=SUPPORTED
MULTIPLE_OPTICAL_CONFIGURATIONS_PER_PROJECT=SUPPORTED
SILENT_CALIBRATION_SUBSTITUTION=FORBIDDEN
SILENT_LENS_IDENTITY_INFERENCE=FORBIDDEN
OPTICS_TUI_WORKFLOW=REQUIRED
PROFILE_IMPORT_EXPORT=REQUIRED
```

The final onboarding UX, aliases, profile import/export and calibration assistant remain part of the
upcoming PRODUCT_DEFINITION phase.

## Deferred / optional architecture

The following remain deliberately deferred unless later evidence and product definition justify
them:

- multiple CPU/GPU/I/O pools;
- general inter-Task parallelism;
- multi-GPU;
- a general dependency DAG and complex priorities;
- a generic backend framework;
- distributed compute;
- ALIKED until model provenance, ONNX export and an authoritative reproducible oracle are available.

Deferring inter-Task parallelism does not permit accidental serialism inside an active Task.

The established Queue/Governor plus bounded internal fan-out are sufficient to enforce
`SERIALISM_REQUIRES_PROOF` without introducing a second runtime.

## Sequencing principles

1. Scientific integrity and the interactive host reserve are inviolable.
2. Within those boundaries, maximum safe useful throughput is mandatory.
3. `SERIALISM_REQUIRES_PROOF`.
4. Per-item atomicity does not imply cross-item serialization.
5. Deterministic owner-only publication may coexist with bounded parallel preparation.
6. Durable atomic results and truthful recovery precede concurrency convenience.
7. Memory, files, threads, processes, temporary storage and scratch ownership remain bounded.
8. Operational hardware bounds must not silently become scientific dataset-size limits.
9. One Task Runtime / Queue ownership model and one Resource Governor remain authoritative.
10. Validated and useful GPU backends are preferred automatically.
11. Scratch is optional storage, never RAM.
12. Real-data proofs stop at explicit scientific boundaries.
13. Historical checkpoint evidence is preserved rather than silently modernized.
14. Current-state documentation must not present superseded progress snapshots as current work.
15. Repository documentation, agent contracts, source comments and UI target English.
16. Future product capabilities must remain clearly marked as future until implemented and validated.
17. Reviews remain delta-based from the relevant acquired checkpoint unless concrete evidence requires
    reopening an unchanged FROZEN boundary.

## Current next

The repository execution contract is active:

```text
DOCUMENTATION_FINDING_REMEDIATION   PASS
SOURCE_COMMENT_AUDIT                PASS
SOURCE_COMMENT_REMEDIATION          PASS
PRODUCT_DEFINITION_V1               PASS/FROZEN
PROMPT_TREE                         CURRENT
CALIBRATION_EVIDENCE_SOLVER_V1      IMPLEMENTED/VALIDATED
CALIBRATION_TOOLING_ALIGNMENT       PASS/FROZEN
CURRENT_NEXT                        DEDICATED_PHYSICAL_CALIBRATED_REAL_CAMPAIGN
```

Implementation proceeds only through explicitly human-authorized tranches under `prompt.md` and the
numbered `prompt/` execution contract. The current authorized dependency is the final usable
calibration workflow; its next missing sub-boundary composes the FROZEN Tooling
and Bootstrap importer to reach truthful READY from the validated binding.
