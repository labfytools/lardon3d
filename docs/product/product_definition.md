# Lardon3D — Product Definition v1

## Status

```text
PRODUCT_DEFINITION_V1=PASS/FROZEN
PRODUCT_NAME=Lardon3D
PRIMARY_PLATFORM=Linux
PRIMARY_CONTROL_SURFACE=TUI
OPTIONAL_GRAPHICAL_VIEWER=REQUIRED
IMPLEMENTATION_AUTHORIZATION=NO
PROMPT_TREE=NEXT
```

This document defines the target product.

It is product authority for intended user-visible capability and end-to-end workflow. It does not
reopen existing FROZEN scientific contracts, authorize a schema migration, authorize a new Task Kind,
or authorize implementation by itself.

Existing specialized architecture documents remain authoritative for already implemented/FROZEN
scientific, persistence, runtime and resource contracts.

When a future implementation choice conflicts with this product definition, the implementation must
stop for explicit contract reconciliation rather than silently weakening the product requirement.

## Product statement

Lardon3D is a persistent, incremental, resource-aware photogrammetry system that lets a user build,
inspect, improve and export a 3D reconstruction from photographs, future video keyframes and
device-assisted supplementary captures without losing scientific provenance or previously validated
work.

The final user workflow is not merely:

```text
folder of photos -> model
```

It is:

```text
project
-> explicit acquisition / provenance
-> optics and calibration readiness
-> quality selection
-> deterministic scientific representation
-> Features
-> Visual Index
-> Candidate Pairs
-> Matcher
-> Geometric Verification
-> Tracks
-> Sparse SfM
-> incremental / multi-campaign registration where required
-> Dense / MVS
-> mesh
-> refinement
-> texture
-> coverage analysis
-> viewer / capture guidance
-> export
```

The system must remain restartable and inspectable throughout that lifecycle.

## Product principles

```text
SCIENTIFIC_TRACEABILITY=REQUIRED
DETERMINISTIC_IDENTITIES=REQUIRED
PERSISTENT_PROGRESS=REQUIRED
ATOMIC_PUBLICATION=REQUIRED
BOUNDED_EXECUTION=REQUIRED
MAXIMUM_SAFE_USEFUL_THROUGHPUT=REQUIRED
SERIALISM_REQUIRES_PROOF=REQUIRED
NO_SILENT_SCIENTIFIC_SUBSTITUTION=REQUIRED
NO_DESTRUCTIVE_AUTOMATION=REQUIRED
```

The user should not have to understand internal CPU widths, batch sizes, task cursors or backend
details to obtain correct execution.

The product must make uncertainty visible rather than replacing it with a guess.

## Lifecycle vocabulary

Product and implementation work use these states:

```text
IDEA
RESEARCH
PLANNED
AUTHORIZED
IMPLEMENTING
VALIDATED
PASS/FROZEN
REJECTED
```

`PASS/FROZEN` means the defined boundary is acquired and is not reopened without explicit evidence.

`PLANNED` means the capability belongs to the target product but implementation is not yet authorized.

`AUTHORIZED` means the human has explicitly authorized implementation of that scoped capability.

This document may freeze a product requirement while its implementation remains only `PLANNED`.

## Current acquired foundation

The following existing boundaries are consumed as-is:

```text
Project DB head                         v27
Production Task kinds                   16
Capture / Asset Provenance              PASS/FROZEN
Acquisition / campaign execution        PASS/FROZEN
Photo Quality Triage                    PASS/FROZEN
Selected Scientific Execution           PASS/FROZEN
Feature Store v1/v2                     IMPLEMENTED
Visual Index v1                         IMPLEMENTED
Candidate Pair                          IMPLEMENTED
Matcher v1                              IMPLEMENTED
Geometric Verifier v3                   PASS/FROZEN
Track Model / Track Builder v1          PASS/FROZEN
Sparse SfM Gates A-G                    PASS/FROZEN
Phase H v1 incremental reconstruction   PASS/FROZEN
MVS-M1 external OpenMVS boundary        PASS/FROZEN
Calibration Science v1                  PASS/FROZEN
Calibration Tooling v1                  PASS/FROZEN
Calibration Bootstrap v1                PASS/FROZEN
Calibration Solver Preflight v1         PASS
Resource Governor / Compute Governor    PASS/FROZEN
Bounded internal parallelism            PASS/FROZEN
ORB Vulkan backend                      PASS/FROZEN
TUI runtime observatory                 VALIDATED
External SSD controller                 VALIDATED
```

The final product extends these boundaries; it does not replace them merely for architectural
uniformity.

## Real-data reference checkpoints

Current retained real evidence includes:

```text
REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The retained A6000 pre-SfM checkpoint contains:

```text
Selected images        689
Feature Sets           689
Candidate Pairs        38,420
Match Results          38,420
Applicable GVRs        37,805
Verified GVRs          10,952
Rejected GVRs          26,853
Track Sets             1
Tracks                 130,714
Track observations     318,944
Sparse SfM Tasks       0
Sparse Reconstructions 0
Dense/MVS              0
```

These campaigns are evidence, not product-size limits.

They remain blocked from real known-calibration Sparse SfM by their historical calibration status.

## Project model

### Product requirement

A project is the durable user-owned reconstruction workspace.

The project must contain or reference all scientific identities required to understand what has been
done, what is reusable, what is blocked and what is still pending.

Reopening a project after a crash or reboot must not require the user to remember hidden transient
state.

### Portability

The durable project must not depend on:

- absolute temporary paths;
- current CPU topology;
- current GPU device;
- current external-scratch mount path;
- a live camera connection;
- a previous process ID;
- a specific Task worker instance.

Hardware-specific operational state is rediscovered.

Scientific identities remain stable.

### Non-destructive history

Valid immutable generations remain available unless explicitly deleted through a future supported
cleanup operation.

A new scientific result does not silently overwrite a previous result with a different identity.

## Primary user interface

```text
TUI=PRIMARY_CONTROL_SURFACE
GUI_CONTROL_REPLACEMENT=NO
```

The ncurses TUI remains the primary control plane.

It owns workflow navigation and explicit user actions.

The graphical viewer is an optional companion visualization surface, not a replacement for project
control, resource ownership or durable task orchestration.

Final TUI language:

```text
USER_INTERFACE_LANGUAGE=ENGLISH
```

Remaining legacy non-English executable strings must converge to English in an explicitly scoped
implementation pass.

## Required TUI product areas

The final TUI must provide coherent access to:

- project creation/open/close;
- acquisition/import;
- camera/lens/optical configuration;
- calibration readiness and calibration workflow;
- quality triage and selection;
- pipeline stages and durable progress;
- Tasks;
- Resource Governor state;
- external SSD/scratch state;
- reconstruction generations;
- viewer launch/control;
- coverage analysis;
- capture guidance state;
- export;
- diagnostics/help.

Exact key bindings may evolve, but displayed actions and actual handlers must remain consistent.

## Optics onboarding

Status:

```text
OPTICS_ONBOARDING=PLANNED

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

### Data-driven equipment

Adding a normal supported camera body or lens must be a data operation.

Source-code modification is required only when the equipment introduces a genuinely unsupported
transport, file format, camera model or scientific model.

A new brand/model name is not itself a reason for source changes.

### Camera body profile

The user can create a body profile from:

- exact electronic metadata when available;
- manual make/model entry when unavailable;
- explicit aliases for metadata variants.

Aliases must be exact and reviewable.

No fuzzy match may silently create scientific identity.

### Lens profile

The user can create a lens profile with electronic metadata or manually.

A manual lens without EXIF is a normal product path.

The user must be able to assign the exact lens explicitly when metadata cannot identify it.

### Zoom lenses and focal configurations

A zoom lens may own multiple optical configurations.

The active scientific configuration includes the relevant focal configuration.

Electronic focal metadata may select an exact known configuration only when the compatibility
contract permits it.

Manual focal selection is supported.

Calibration must not be silently interpolated between focal configurations unless a future scientific
contract explicitly validates such interpolation.

### Onboarding speed

Target:

```text
OPTICS_PROFILE_ONBOARDING_TARGET<=5_MINUTES
```

This target excludes the time required to perform a physical calibration acquisition.

It includes creating/selecting body, lens, aliases and optical configuration.

### Profile import/export

The final product must provide bounded, versioned profile import/export suitable for moving equipment
profiles between Lardon3D installations.

The portable representation must:

- carry an explicit format version;
- validate all bounds before database mutation;
- preserve scientific/profile identity fields;
- reject unknown required semantics;
- never be a raw SQLite dump;
- never silently merge conflicting profiles;
- provide a dry-run/preview before import commit.

The exact file encoding is an implementation-contract decision in the prompt tree.

## Calibration user experience

Status:

```text
CALIBRATION_WORKFLOW=PLANNED
CALIBRATION_SCIENCE_V1=PASS/FROZEN
```

Sparse SfM v1 remains known-calibration only.

### User-visible readiness

The optics/calibration workflow must expose at least:

```text
READY
CALIBRATION_REQUIRED
SELECTION_REQUIRED
```

Meaning:

- `READY`: one exact compatible calibration is explicitly selected and valid;
- `CALIBRATION_REQUIRED`: no valid compatible calibration exists;
- `SELECTION_REQUIRED`: multiple or otherwise unresolved valid choices require explicit user choice.

A diagnostic may further explain incompatibility/corruption, but these core states must remain clear.

### No silent fallback

The product must never silently use:

- EXIF focal length as scientific calibration;
- another lens's calibration;
- a nearby focal calibration;
- a calibration from another optical configuration;
- an inferred "unknown lens";
- an interpolated calibration not authorized by science.

### Physical calibration assistant

The final TUI must guide the user through the physical acquisition required by Calibration Science v1.

The assistant must tell the user:

- what target/setup is required;
- which optical configuration is being calibrated;
- which captures are accepted/rejected for evidence;
- whether the evidence bundle is sufficient;
- what remains to capture.

It must not weaken Calibration Science v1 to make a session pass.

### External solver

The selected external OpenCV 5.x solver remains outside the reconstruction scientific core.

The final product should automate its invocation from the calibration workflow so the user does not
need to manually construct solver evidence.

Its output must still pass:

```text
external solver
-> Calibration Tooling validation
-> deterministic L3DCALB1 v1
-> Calibration Bootstrap import
-> explicit optical compatibility/selection
```

A solver success code alone is not calibration acceptance.

## Acquisition sources

The final product accepts multiple acquisition-source classes through the same Capture/provenance
model.

### Still-image import

Status:

```text
STILL_IMAGE_IMPORT=PASS/FROZEN_FOUNDATION
```

Supported product paths include:

- standalone JPEG/PNG/TIFF-like decoded still images supported by the current decoder stack;
- RAW sources through explicit RAW representation policy;
- RAW+JPEG paired acquisitions;
- mixed campaigns when their identities remain explicit.

The selected scientific representation is never silently changed after selection.

### Live camera source

Status:

```text
LIVE_CAMERA_SOURCE=PLANNED
```

Live transport is a device-adapter concern at the acquisition boundary.

It must not change Feature, Match, GVR, Track or reconstruction identity rules.

Live preview frames are ephemeral observation frames unless explicitly promoted through a supported
Capture/import path.

### Video source

Status:

```text
VIDEO_INGESTION=PLANNED
```

Video is an acquisition source, not a second reconstruction pipeline.

Required flow:

```text
SOURCE video asset
-> exact timeline/frame identity
-> deterministic bounded keyframe extraction
-> quality / blur / redundancy analysis
-> explicit selected frame representations
-> normal Capture / provenance path
-> existing scientific pipeline
```

Every retained keyframe must be traceable to:

- source video asset identity;
- exact frame/timestamp identity;
- extraction algorithm/version;
- extraction parameter fingerprint.

No retained frame may exist only as an anonymous temporary bitmap.

### Keyframe science

Keyframe scoring/threshold science is not frozen by this product document.

The prompt/implementation process must create a versioned deterministic contract before implementation.

## Sony A6000 product integration

Status:

```text
A6000_LIVE_INTEGRATION=PLANNED
A6000_FIRMWARE_MODIFICATION=REJECTED
A6000_HARDWARE_MODIFICATION=REJECTED
```

The Sony A6000 must remain stock.

No PMCA modification, custom firmware, camera-side hack or hardware modification belongs to the product.

### Live view

Primary live path:

```text
A6000 native HDMI
-> external capture device
-> Linux V4L2/UVC-class adapter where available
-> Lardon3D live acquisition adapter
-> viewer / localization / guidance
```

The product should discover capture-device capabilities rather than hard-code one USB card model.

### Camera control and transfer

USB may be used for shutter/control/metadata/file transfer only through capabilities supported by the
camera and verified by the adapter.

USB control is not required for live preview correctness.

If remote shutter is unavailable, the operator may capture on-camera and Lardon3D can ingest the
resulting files through the normal acquisition path.

### Compute location

```text
A6000_HEAVY_COMPUTE=PC_SIDE_ONLY
```

The camera is not a compute node.

## Samsung S21 product integration

Status:

```text
S21_LIVE_INTEGRATION=PLANNED
S21_ROOT_REQUIRED=NO
```

The S21/mobile path uses a device-specific acquisition adapter while reusing the same Capture,
provenance, optics, quality and scientific pipeline.

The transport may differ from A6000.

The product contract requires:

- no root requirement;
- no scientific core fork;
- explicit full-resolution Capture ingestion;
- live-preview frames treated as ephemeral until promoted;
- graceful disconnect/reconnect;
- exact optical/calibration configuration for scientific use.

The exact Android transport mechanism remains an implementation decision.

## Device adapter boundary

Device-specific code belongs at the acquisition/control edge.

A device adapter may own:

- discovery;
- connection;
- preview transport;
- shutter/control when supported;
- metadata retrieval;
- file transfer;
- reconnect semantics.

It may not redefine:

- Capture identity;
- image scientific identity;
- calibration science;
- Feature identity;
- Track identity;
- reconstruction identity;
- resource accounting.

## Quality workflow

Automated quality analysis remains explainable and non-destructive.

The product displays:

```text
GOOD
SUSPECT
REJECT
```

as recommendations.

The user may explicitly override a recommendation where the existing selection contract permits it.

An override must be durable and visible; it must not rewrite the measured quality evidence.

Live capture guidance should reuse the same quality concepts where practical so the operator can see
blur/exposure/quality issues before relying on a capture.

## Sparse reconstruction

Status:

```text
SPARSE_SFM_CAPABILITY=PASS/FROZEN
REAL_KNOWN_CALIBRATION_EXECUTION=PLANNED
```

The product must expose Sparse SfM as a normal pipeline stage once optics/calibration state is `READY`.

A blocked known-calibration requirement must be shown explicitly.

The user must never be offered a "continue anyway with guessed calibration" path.

### Sparse viewer evidence

Sparse results should expose:

- registered/unregistered cameras;
- camera frustums;
- sparse landmarks;
- Track support;
- reprojection diagnostics;
- component structure;
- arbitrary-vs-metric scale status.

Physical distance tools must not label arbitrary monocular gauge units as millimetres/metres.

## Incremental reconstruction

Status:

```text
PHASE_H_V1=PASS/FROZEN
```

The final product uses existing incremental reconstruction when its lineage prerequisites are
satisfied.

Phase H is not a generic multi-campaign fusion mechanism.

A scientifically unrelated or independently reconstructed campaign must not be forced through Phase H
merely because it belongs to the same user project.

## Multi-campaign reconstruction

Status:

```text
MULTI_CAMPAIGN_REGISTRATION=PLANNED
MULTI_CAMPAIGN_FUSION=PLANNED
RAW_PROJECT_MERGE_WITHOUT_REGISTRATION=REJECTED
```

Multiple campaigns may exist in one logical project.

Each campaign is reconstructed under its own exact optics/calibration context.

Independent reconstructions are combined only after an explicit registration stage establishes their
relationship.

### Registration product contract

Registration must produce an explicit durable transform with provenance and quality evidence.

Depending on scale knowledge, the accepted transform may be:

- rigid when metric scales are compatible;
- similarity when relative scale must be solved.

The product may use automatic overlap evidence and may offer manual control-point assistance as a
fallback.

The exact registration algorithm is a future scientific contract.

### Fusion rule

Fusion occurs only after registration is accepted.

The product must preserve per-campaign provenance so a user can inspect which campaign contributed to a
region/result.

The S21 and A6000 historical projects must not be casually merged at raw Feature/Track level.

## Dense / MVS

Status:

```text
MVS_M1_EXTERNAL_BOUNDARY=PASS/FROZEN
DURABLE_DENSE_PIPELINE=PLANNED
INITIAL_DENSE_BACKEND=OpenMVS
GENERIC_BACKEND_FRAMEWORK=REJECTED_FOR_V1
```

The first final-product dense path should build on the validated OpenMVS boundary rather than invent a
generic backend framework first.

### Dense Task requirements

Dense execution must become:

- durable;
- restartable at explicit boundaries;
- resource-governed;
- scratch-aware;
- failure-atomic for published scientific outputs;
- inspectable through TUI progress/diagnostics.

External process exit status is not sufficient by itself; expected output assets must validate before
publication.

### Dense failure behavior

OOM, process crash, invalid output or missing asset must fail the Task cleanly.

The project must remain reopenable.

A failed dense attempt must not corrupt or invalidate the upstream Sparse Reconstruction.

## External SSD, scratch and swap

Status:

```text
EXTERNAL_SSD_CONTROLLER=VALIDATED
TASK_SCRATCH_CONSUMPTION=PLANNED
PROJECT_SCRATCH_OPT_IN=REQUIRED
SWAP_OPT_IN=REQUIRED
SCRATCH_COUNTS_AS_RAM=NO
SWAP_COUNTS_AS_RAM=NO
```

### Discovery

The TUI should detect the validated external SSD pairing and show:

- physical identity;
- mount state;
- scratch state;
- swap state;
- capacity/usage when known;
- active leases;
- drain/safe-to-unplug state.

### Project scratch

The user explicitly chooses whether an eligible external scratch volume may be used by the project.

No project silently adopts a newly connected disk.

Future dense/mesh/refine/texture Tasks may acquire scratch only through the established Governor lease
boundary.

### Swap

Swap is a host safety mechanism.

The product may offer explicit enable/disable control when supported by the validated SSD controller.

Swap never increases RAM admission.

### Destructive operations

The product must not automatically:

- repartition;
- format;
- fsck destructively;
- overwrite an unknown filesystem;
- force-unmount an active lease.

## Mesh

Status:

```text
MESH_PIPELINE=PLANNED
```

The product must support generation of a surface mesh from a validated dense result.

The mesh is a distinct immutable result with exact provenance.

A failed mesh attempt does not alter the dense source.

## Mesh refinement

Status:

```text
MESH_REFINEMENT=PLANNED
```

Refinement is an explicit stage and result identity.

It must not silently mutate the source mesh.

Resource-heavy refinement may use external scratch through a Task-owned lease.

## Texturing

Status:

```text
TEXTURING=PLANNED
```

Texturing consumes an explicit mesh/reconstruction/image scope.

Texture provenance must retain the source image/campaign relationship.

A user must be able to inspect an untextured mesh even if texturing fails.

## Consolidation

Status:

```text
FINAL_CONSOLIDATION=PLANNED
```

Consolidation creates a final user-facing selected result from explicit upstream generations.

It does not delete those upstream generations.

Selection is explicit and reversible until the user deliberately performs cleanup.

## Export

Status:

```text
EXPORT_WORKFLOW=PLANNED
```

The final product must support practical interoperable outputs.

Minimum target formats:

```text
sparse/dense point cloud: PLY
mesh:                    PLY and OBJ
textured mesh:           OBJ + MTL + texture assets
portable viewer asset:   GLB/GLTF
geometry-only exchange:  STL optional
```

The exact exporter set may grow, but proprietary-format lock-in is not acceptable.

Exports must carry a manifest or sidecar metadata sufficient to identify the source reconstruction,
mesh/texture generation and scale status.

If scale is arbitrary, the export must not claim a metric unit.

## Graphical viewer

Status:

```text
VIEWER=PLANNED
VIEWER_IS_PASSIVE_CONSUMER=REQUIRED
VIEWER_CAN_BE_DISABLED=REQUIRED
VIEWER_BLOCKS_ENGINE=NO
```

The TUI remains the control surface.

The viewer is a graphical companion that can be launched/closed without stopping reconstruction.

Functional isolation is mandatory; process isolation is optional.

### Snapshot boundary

The viewer consumes coherent validated snapshots.

It must not borrow mutable engine internals.

If it falls behind, it drops obsolete visual snapshots rather than creating an unbounded backlog.

### Required visualization modes

The final viewer must support, when the corresponding data exists:

- sparse landmarks;
- dense point cloud;
- mesh;
- textured mesh;
- registered cameras/frustums;
- reconstruction components;
- selected ScanSet/campaign contribution;
- Track/observation support;
- reprojection/quality diagnostics;
- coverage heatmap;
- weak regions;
- unseen regions;
- likely holes;
- live camera pose/frustum;
- suggested capture targets.

### Interaction

Required interaction includes:

- orbit/pan/zoom;
- reset/focus;
- element/region selection;
- camera selection;
- visibility toggles;
- diagnostic inspection;
- measurement when scale is valid;
- region-of-interest selection for coverage/guidance.

Viewer annotation must not silently become scientific input unless the user explicitly invokes a
supported operation such as future control-point registration.

## Coverage Analysis

Status:

```text
OFFLINE_COVERAGE_ANALYSIS=PLANNED
```

Coverage Analysis answers:

```text
Where is the reconstruction well supported?
Where is evidence weak?
Where is the object likely unseen?
Where would another photograph provide useful information?
```

### Coverage evidence

The product must be able to use, where available:

- observation/view count;
- distinct camera count;
- angular diversity;
- incidence/viewing angle;
- parallax;
- camera distance;
- projected resolution;
- image quality;
- Feature/Track support;
- reprojection/triangulation quality;
- visibility/occlusion;
- mesh/dense support;
- hole/boundary evidence;
- ScanSet/campaign provenance.

### Coverage states

The UI must distinguish at least:

```text
UNKNOWN
UNSEEN
WEAK
ADEQUATE
```

`UNKNOWN` is required when the available reconstruction cannot support a truthful classification.

Thresholds/weights are scientific policy and must be versioned before implementation.

### Region of interest

Coverage can operate over:

- the complete current reconstructed surface;
- an explicit user-selected region of interest.

A global "complete" claim is invalid when no meaningful target surface/ROI is defined.

### Sparse versus mesh coverage

Sparse-only analysis is allowed but must identify its lower-confidence/support boundary.

Mesh/dense availability may provide stronger visibility/hole reasoning.

The product must not fabricate a closed surface from sparse points merely to claim coverage.

## Suggested supplementary viewpoints

Status:

```text
VIEWPOINT_SUGGESTION=PLANNED
```

Coverage weaknesses can be converted into ranked capture suggestions.

Each suggestion must expose:

- target region;
- suggested viewing direction;
- suggested camera-position zone or relative viewpoint;
- distance/range guidance;
- angle/incidence guidance;
- baseline/parallax relationship to existing views;
- expected coverage/evidence improvement;
- confidence/feasibility;
- reason.

The user should receive an actionable instruction, not only a red heatmap.

The exact optimization/scoring policy must be versioned and validated before implementation.

## Live camera localization

Status:

```text
LIVE_CAMERA_LOCALIZATION=PLANNED
```

The live camera may be localized against an existing compatible reconstruction.

Required high-level path:

```text
live frame
-> bounded Features
-> correspondence to existing reconstruction
-> calibrated pose
-> confidence / diagnostics
-> viewer snapshot
```

### Preconditions

Precise localization requires:

- compatible exact camera calibration;
- a reconstruction with usable 3D reference;
- enough valid correspondence support;
- acceptable geometric confidence.

When those prerequisites are not satisfied, the product reports the reason.

### Tracking loss

The live system must have explicit states such as:

```text
UNAVAILABLE
SEARCHING
LOCALIZED
LOW_CONFIDENCE
LOST
```

A stale last-good pose must not be displayed as current localization without a visible stale/lost
indicator.

## Live Coverage / Capture Guidance

Status:

```text
LIVE_COVERAGE_OVERLAY=PLANNED
CAPTURE_GUIDANCE=PLANNED
AUTO_CAPTURE=IDEA
```

Target loop:

```text
existing reconstruction
-> offline/current coverage model
-> weak/unseen target
-> live camera localization
-> project target into live view
-> guide operator
-> acquire full-resolution photograph
-> normal ingestion/quality/scientific pipeline
-> incremental/re-registration update
-> coverage refresh
```

### Overlay

The live viewer should make weak/unseen regions visually obvious.

Exact color theme is UI policy, but the semantic distinction must be readable without relying solely on
color.

### Guidance

Guidance should tell the operator how to move:

- left/right/up/down;
- closer/farther;
- rotate toward/away from target;
- increase/decrease incidence angle;
- increase/decrease baseline where appropriate.

The product should display the reason and confidence.

### Safety against false certainty

When localization or coverage confidence is insufficient:

- no precise overlay is asserted;
- no "capture here" instruction is presented as certain;
- the UI falls back to a clear diagnostic/search state.

### Capture action

Initial product requirement is operator-confirmed capture.

Automatic shutter release is not required for v1 capture guidance.

`AUTO_CAPTURE=IDEA` may be revisited only after live localization, guidance and device control are
validated.

## Reconstruction update after guided capture

A guided photograph becomes normal project evidence only after full-resolution ingestion and normal
validation.

The live preview bitmap itself is not silently promoted.

The project must reuse already valid upstream work and process only the scientifically affected
delta whenever the existing frozen contracts permit it.

## Resource behavior

The canonical objective remains:

```text
MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF
```

### Automatic execution

Normal product operation automatically chooses:

- CPU width;
- batch/window;
- GPU backend when validated/useful;
- inflight depth;
- internal participant count;
- scratch use for Tasks that own a scratch contract.

The user may observe these choices but is not required to tune them.

### Interactive reserve

The Resource Governor preserves the defined interactive host reserve and then uses the maximum
remaining safe useful resources.

Reference-host values are not product constants.

### GPU

A validated useful GPU backend is preferred automatically.

A GPU backend is not required when profiling says it is not useful.

Backend failure must have a defined failure/fallback contract where the scientific stage supports one.

### Memory

No Task treats swap, zram or scratch as RAM.

UMA GPU memory is charged once.

No unbounded whole-project load is allowed merely for convenience.

## Persistence and recovery product requirement

Every user-visible long-running stage must have an explicit durable/restart contract before it is
considered final-product complete.

The product must support clean recovery from:

- application close;
- system reboot;
- Task cancellation;
- process crash;
- external-process failure;
- external scratch disconnect after safe drain;
- partial physical asset publication where the existing orphan semantics allow it.

Restart must never require guessing the scientific input from a filename or timestamp.

## Determinism

Scientific identity is independent of:

- CPU thread count;
- worker scheduling;
- host model;
- GPU model unless the backend is scientifically non-transparent;
- temporary path;
- wall-clock duration.

Where exact bit identity cannot be promised across dependency/hardware versions, the relevant
scientific contract must define the reproducibility boundary honestly.

## Diagnostics

The product must distinguish:

- scientific rejection;
- invalid input;
- missing prerequisite;
- resource wait/throttle;
- runtime failure;
- corruption;
- unsupported version/backend;
- user cancellation.

A generic "failed" message without the owning layer/reason is insufficient for final-product
workflows.

## No silent inference

Across the product, the following are forbidden unless a specific scientific contract explicitly says
otherwise:

- selecting "latest" scientific result by timestamp;
- calibration substitution;
- lens identity guessing;
- focal interpolation;
- physical scale invention;
- cross-campaign alignment by filename/time alone;
- treating a preview frame as a scientific capture;
- treating a GPU/CPU implementation choice as scientific identity without reason;
- pretending unknown coverage is adequate.

## Product security and privacy boundary

The core reconstruction workflow is local-first.

No cloud service is required for:

- project operation;
- reconstruction;
- calibration processing;
- viewer;
- coverage analysis;
- capture guidance;
- export.

Future optional network/device adapters must make network use explicit.

User project imagery is not uploaded merely to operate the product.

## Cleanup and storage management

Project cleanup is explicit.

The product may identify:

- orphan temporary assets;
- superseded immutable generations;
- export caches;
- stale scratch.

It must show what will be removed before destructive cleanup.

FROZEN/historical evidence is never deleted automatically because a newer generation exists.

## Performance product requirement

Performance is judged by useful end-to-end throughput while maintaining:

- scientific correctness;
- deterministic publication;
- memory bounds;
- host responsiveness;
- restartability.

A stage that leaves safe useful CPU/GPU capacity idle without a measured/contractual reason is a
performance defect.

A stage that saturates the host beyond its safety reserve is also a defect.

## Final end-to-end user journey

A normal future still-image project should support:

```text
1. Create/open project.
2. Select/create camera body and lens profile.
3. Resolve exact optical configuration.
4. If needed, complete calibration workflow until READY.
5. Import/capture images.
6. Review quality recommendations and explicit selection.
7. Run selected scientific pipeline through Tracks.
8. Run Sparse SfM.
9. Inspect sparse reconstruction in viewer.
10. Add another campaign or supplementary images if useful.
11. Register/enrich according to the correct scientific relationship.
12. Run Dense/MVS.
13. Build/refine mesh.
14. Texture.
15. Run coverage analysis.
16. Inspect weak/unseen regions.
17. Optionally use live capture guidance to acquire missing evidence.
18. Reprocess only the affected delta.
19. Consolidate a desired result.
20. Export interoperable outputs.
```

The user can stop/restart between long stages without invalidating completed durable work.

## Final A6000 guided-capture journey

Target:

```text
stock Sony A6000
-> native HDMI live view
-> capture card
-> Lardon3D viewer
-> calibrated live localization
-> weak/unseen overlay
-> actionable viewpoint guidance
-> operator capture
-> RAW+JPEG/full-resolution transfer/import
-> quality/selection
-> normal incremental scientific update
-> refreshed coverage
```

No camera modification is required.

## Final S21 guided-capture journey

Target:

```text
stock/non-rooted S21
-> device acquisition adapter
-> live preview
-> calibrated live localization
-> coverage/guidance
-> full-resolution capture/import
-> normal scientific update
```

The exact mobile transport is not part of the scientific core.

## Definition of Done — product

Lardon3D is product-complete for this definition only when all `PLANNED` required capabilities in this
document have reached at least `VALIDATED`, with the relevant scientific boundaries `PASS/FROZEN`
where appropriate.

Minimum end-product proof requires all of the following:

### Core workflow

- new project can reach calibrated real Sparse SfM;
- durable Dense/MVS can run and restart;
- mesh/refine/texture can publish validated immutable results;
- exports are usable and traceable.

### Optics

- a new normal camera profile requires no source change;
- a new normal lens profile requires no source change;
- manual lens works without EXIF;
- zoom/multiple focal configurations work explicitly;
- profile import/export works;
- calibration ambiguity blocks rather than guesses.

### Viewer

- sparse, dense, mesh and textured result visualization works;
- cameras and diagnostics are inspectable;
- viewer can close/crash/lag without corrupting or blocking the engine.

### Coverage and guidance

- offline weak/unseen analysis works with explicit confidence;
- a recommended supplementary viewpoint is actionable;
- live localization reports loss/uncertainty truthfully;
- A6000 native-HDMI guidance path is demonstrated without modifying the camera;
- captured supplementary evidence returns through the normal durable pipeline.

### Recovery/resources

- representative long stages survive restart at their documented durable boundary;
- no unsafe use of swap/scratch as RAM;
- external scratch can be drained and safely disconnected;
- safe useful CPU/GPU capacity is used automatically.

### Scientific integrity

- no silent calibration/lens/scale/campaign inference;
- FROZEN historical identities remain interpretable;
- result provenance is sufficient to explain which inputs/configuration produced an output.

## Explicitly not required for Product Definition v1

The following are not prerequisites for product completion unless later explicitly promoted:

```text
AUTO_CAPTURE
MULTI_GPU
DISTRIBUTED_COMPUTE
GENERAL_INTER_TASK_DAG
GENERAL_BACKEND_FRAMEWORK
CAMERA_FIRMWARE_MODIFICATION
CAMERA_HARDWARE_MODIFICATION
CLOUD_RECONSTRUCTION
```

## Implementation sequencing constraint

This document does not choose file-by-file implementation order.

The next phase is the canonical `prompt.md` plus numbered `prompt/` execution tree.

That tree must:

- preserve all existing FROZEN contracts;
- encode this product definition without weakening it;
- separate current facts from future requirements;
- define explicit STOP conditions;
- define allowed implementation order;
- define schema/Task/scientific authorization boundaries;
- require delta-based validation;
- keep Git closure under human ownership unless explicitly delegated.

Until that tree is frozen:

```text
IMPLEMENTATION_AUTHORIZATION=NO
PROMPT_TREE=NEXT
```
