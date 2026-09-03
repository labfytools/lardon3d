# Lardon3D

Lardon3D is a generic, persistent, incremental, resource-aware photogrammetry engine for Linux,
controlled through an ncursesw TUI.

## Vision

Lardon3D is designed around the following principles:

- **Scientific traceability**: results, identities, parameters and provenance are explicit.
- **Determinism**: equivalent inputs and contracts produce reproducible, auditable outputs.
- **Persistent progress**: long-running work is checkpointed and restartable.
- **Bounded execution**: memory, CPU, GPU, I/O and temporary-storage use are explicitly bounded.
- **Maximum safe useful throughput**: after preserving the interactive host reserve, available
  resources should be used whenever they provide useful throughput.
- **Incremental reconstruction**: new observations can extend previous results without silently
  rewriting validated history.
- **Atomic publication**: partially produced scientific outputs never masquerade as complete ones.

Lardon3D is not simply a "folder of photos -> 3D object" tool. Its target model is:

```text
progressive observations and constraints
    -> persistent geometric reconstruction
    -> validated incremental enrichment
    -> dense geometry / mesh / texture / export
```

## Current repository state

### Current Project Database

The current Project DB schema is **v26**.

The current head is additive:

```text
v22  Selected scientific execution foundation
v23  Generic optical-context overlay
v24  raw.develop.batch/1 persistence
v25  features.extract.batch/1 persistence
v26  Capture geometric state and exact calibration applicability
```

Earlier schema versions remain valid historical contracts where their own documentation says so.
No migration silently reinterprets historical scientific identities.

### Current production task inventory

The production registry currently contains **16 Task kinds**.

All production Tasks pass through the existing Task -> Queue -> Resource Governor execution model.
The Queue has one active callback at a time; Tasks may use bounded internal participants when their
contract and measured scaling justify it.

### Resource policy

The canonical operational objective is:

```text
MAXIMUM SAFE USEFUL THROUGHPUT
SERIALISM_REQUIRES_PROOF
```

Lardon3D first preserves the interactive host reserve required for the desktop, Firefox, audio and
light interactive use. Safe and useful resources beyond that reserve belong to the active workload.

On the current validation host, the normal observed outcome is approximately:

```text
16 logical CPUs total
4 logical CPUs reserved for interactive host use
12 logical CPUs available to the compute pool
~3 GiB MemAvailable preserved as the hard RAM reserve
Radeon 780M UMA available to validated and useful GPU backends
```

These are **reference-host observations, not portable product constants**. The Resource Governor
derives usable capacity from the current host, affinity, topology, memory and pressure state.

A long-running CPU1 or batch1 path is acceptable only when serialism, a measured scaling knee,
memory, I/O, GPU execution or another concrete constraint justifies it. Per-item atomicity does not
imply cross-item serialization.

## Validated foundations

The following major foundations are implemented and validated at their documented boundaries:

- **Project / persistent lifecycle**
- **Import and ScanSet / Image Catalog**
- **Capture / Asset Provenance v1 — PASS / FROZEN**
- **Bounded acquisition discovery and campaign execution — PASS / FROZEN**
- **Photo Quality Triage / Acquisition Selection — PASS / FROZEN**
- **Selected Scientific Execution — PASS / FROZEN**
- **Feature Store v1/v2**
  - ORB U8x32
  - SIFT / RootSIFT F32x128
  - bounded typed readers
- **Visual Index v1**
- **Candidate Pair generation**
- **Matcher v1**
  - ORB CPU / validated Vulkan hot path
  - SIFT / RootSIFT CPU
- **Geometric Verification Model**
- **Geometric Verifier v3**
- **Track Model / Track Builder v1 — PASS / FROZEN**
- **Sparse SfM Gates C/D/E/F/G — PASS / FROZEN**
- **Phase H v1 incremental reconstruction — PASS / FROZEN**
- **MVS-M1 external OpenMVS boundary — PASS / FROZEN**
- **Task Runtime / checkpoints / recovery**
- **Task Queue**
- **Task Kind Registry**
- **Resource Governor / Compute Governor v2**
- **Bounded internal parallelism — PASS / FROZEN**
- **ORB Vulkan asynchronous execution — PASS / FROZEN**
- **TUI runtime observatory / control center — CURRENT / VALIDATED OPERATIONAL**
- **Optional external SSD controller — CURRENT / VALIDATED OPERATIONAL**
- **Calibration Bootstrap v1 — PASS / FROZEN**
- **Calibration Science v1 — PASS / FROZEN**
- **Calibration Tooling v1 — PASS / FROZEN**
- **Calibration Solver Preflight v1 — PASS**
- **Calibration Evidence Solver v1 — IMPLEMENTED / VALIDATED**
- **Calibration Tooling planarity alignment — PASS / FROZEN**
- **Project DB v24/v25/v26 operational overlays — IMPLEMENTED / VALIDATED**
 - raw.develop.batch/1 durable selected-execution path
 - features.extract.batch/1 durable selected-execution path

## Real-data validation

Sony A6000 and Samsung S21 FE campaigns are validation evidence for the generic pipeline. They are
not product identities, hardcoded camera profiles, CPU limits or dataset-size limits.

### Real S21 Tracks

```text
REAL_S21_TRACKS=PASS/FROZEN
```

The retained real S21 proof validated the complete pre-SfM chain through Track Builder with the
compact Track memory model and deterministic restart semantics.

### Real A6000 pre-SfM

```text
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The retained real A6000 proof uses the selected RAW-derived deterministic representation and
completed the pipeline through Geometric Verification and Tracks without replaying already acquired
upstream work.

Final retained counts:

```text
Selected images       689
Feature Sets          689
Candidate Pairs       38,420
Match Results         38,420
Applicable GVRs       37,805
Verified GVRs         10,952
Rejected GVRs         26,853
Track Sets            1
Tracks                130,714
Track observations    318,944
```

The restart proof reused the existing Track Set without duplicating GVR mappings, Track observations
or Tracks.

The real A6000 proof intentionally stopped before:

```text
Sparse SfM
Sparse reconstruction
Dense / MVS
multi-campaign fusion
```

### Calibration status of the historical real campaigns

The historical S21 and A6000 Engine Bay campaigns currently remain `CALIBRATION_UNAVAILABLE` for
the known-calibration Sparse SfM contract.

This is not a source failure, a quality rejection or permission to infer calibration from metadata.
No pseudo-calibration, silent interpolation or inferred calibration identity is allowed.

Therefore real Sparse SfM for those historical campaigns remains:

```text
BLOCKED_BY_KNOWN_CALIBRATION_DATA
```

Calibration Science v1 defines the protocol for future physically controlled calibration
acquisitions. The external Calibration Evidence Solver v1 implements the qualified OpenCV 5.x
evidence path. Calibration Tooling v1 validates bounded Science v1 evidence and produces the
L3DCALB1 artifact; Calibration Bootstrap v1 imports that artifact. None of these stages turns EXIF
into scientific calibration.

The current missing product boundary is the workflow coordinator that binds the immutable physical
session and solver bundle to one exact selected execution and drives Tooling/Bootstrap to `READY`.

## Architecture

```text
TUI / Project
    |
    v
bounded Task Queue
(one active callback)
    |
    v
Resource Governor
(admission and reservation)
    |
    v
admitted Task callback
(bounded internal participants when justified)
    |
    v
atomic / persistent scientific publication
    |
    v
passive snapshot consumers
(viewer remains future work)
```

Core invariants:

- no Task callback starts without a valid active reservation;
- the Queue does not own resource policy;
- the Resource Governor is the sole production resource authority;
- ncurses remains owned by the main thread;
- Task estimates and installed sequence contracts remain immutable for their defined lifetime;
- buffers, queues, files, threads, participants and temporary work remain bounded;
- owner-only durable publication does not imply serial preparation;
- swap, zram and external scratch never become admitted RAM;
- UMA GPU memory is charged exactly once against host memory.

## Current TUI

The TUI is a validated operational observatory and control center.

It provides bounded observation of:

- Project state;
- Tasks and durable progress;
- Resource Governor state;
- CPU / RAM / swap / GPU information;
- optical profiles and explicit calibration selection;
- optional SSD state and safe control actions.

The ncurses renderer and input handling remain on the main thread. Runtime observation is bounded
and coalesced; the renderer does not scan Project DB or `/proc` extensively per frame.

The validated layout supports:

```text
full layout        >= 100x30
reference compact  72x20
minimum supported  60x15
```

Below the minimum, only the bounded terminal-too-small fallback is rendered.

The optical workflow supports electronic metadata aliases and manual lenses without EXIF. Missing,
ambiguous or incompatible calibration remains visible and is never silently guessed.

## Target pipeline

```text
acquisition
-> catalog / Capture / provenance
-> quality selection
-> selected scientific representation
-> features
-> visual index
-> candidate pairs
-> matching
-> geometric verification
-> tracks
-> Sparse SfM
-> incremental / multi-campaign reconstruction
-> dense / MVS
-> mesh
-> refinement
-> texturing
-> consolidation
-> export
```

## Planned product areas

The following areas remain future work and must not be confused with current implementation:

- durable dense / mesh publication;
- full Dense/MVS orchestration;
- mesh refinement and texturing;
- final export workflow;
- viewer;
- offline coverage analysis;
- suggested supplementary viewpoints;
- live camera localization;
- live coverage overlay;
- A6000 live acquisition integration;
- S21 live acquisition integration;
- capture guidance;
- video ingestion and deterministic keyframe extraction;
- explicit Task-owned scratch consumers;
- general DAG / dependency scheduling.

The final product contracts for these areas are being defined separately before implementation.

## Documentation

- [Documentation index](docs/README.md)

### Architecture

- [Architecture overview](docs/architecture/overview.md)
- [Runtime](docs/architecture/runtime.md)
- [Task system](docs/architecture/task_system.md)
- [Task Kind Registry](docs/architecture/task_kind_registry.md)
- [Task Queue](docs/architecture/task_queue.md)
- [Resource Governor](docs/architecture/resource_governor.md)
- [Bounded internal parallelism](docs/architecture/internal_parallelism.md)
- [Resource-aware pipeline](docs/architecture/resource_aware_pipeline.md)
- [Queue / runtime / Governor integration](docs/architecture/scheduler_resource_integration.md)
- [Reconstruction pipeline](docs/architecture/reconstruction_pipeline.md)
- [Persistence](docs/architecture/persistence.md)
- [Project Database](docs/architecture/project_database.md)
- [Feature Store](docs/architecture/feature_store.md)
- [Precision Feature Pipeline v1A](docs/architecture/precision_feature_pipeline.md)
- [Visual Index](docs/architecture/visual_index.md)
- [Candidate Pair](docs/architecture/candidate_pair.md)
- [Match Result](docs/architecture/match_result.md)
- [Matcher](docs/architecture/matcher.md)
- [Geometric Verification](docs/architecture/geometric_verification.md)
- [Geometric Verifier](docs/architecture/geometric_verifier.md)
- [Track Model](docs/architecture/tracks.md)
- [Track Builder](docs/architecture/track_builder.md)
- [Sparse SfM](docs/architecture/sparse_sfm.md)
- [Calibration Bootstrap v1](docs/architecture/calibration_bootstrap.md)
- [Calibration Publication v2](docs/architecture/calibration_publication_v2.md)
- [Calibration Science v1](docs/architecture/calibration_science_v1.md)
- [Calibration Solver Preflight v1](docs/architecture/calibration_solver_preflight_v1.md)
- [Photo Quality Triage](docs/architecture/photo_quality_triage.md)
- [Vulkan ORB Matcher](docs/architecture/vulkan_matcher.md)
- [Viewer](docs/architecture/viewer.md)
- [Resource Boundary](docs/architecture/resource_boundary.md)

### Historical audit records

- [Global Maintenance Audit](docs/architecture/global_maintenance_audit.md)
- [Foundation Review](docs/architecture/foundation_review.md)

Historical audit records preserve the state and evidence of their checkpoint. Older schema versions,
Task counts or resource measurements inside them must not be mechanically modernized.

### Concepts

- [Scan Sets](docs/concepts/scan_sets.md)
- [Visual Index](docs/concepts/visual_index.md)
- [Matching and Tracks](docs/concepts/matching_and_tracks.md)
- [Reconstruction Layers](docs/concepts/reconstruction_layers.md)
- [Geometric Constraints](docs/concepts/geometric_constraints.md)

Some concept documents are explicitly historical or superseded. Their status header determines
whether they are current authority.

### Development

- [Build](docs/development/build.md)
- [Testing](docs/development/testing.md)
- [Concurrency](docs/development/concurrency.md)
- [Validation-host performance profile](docs/performance/target_hardware.md)

### Roadmap and audits

- [Roadmap](docs/roadmap/roadmap.md)
- [Documentation Inventory Audit](docs/audits/documentation_inventory.md)

## Build

Meson and Ninja are the canonical build path.

```sh
CC=clang meson setup build
meson compile -C build
```

Build parallelism should use safe host capacity. Do not treat a historical `-j8` or the current
reference-host result of approximately `-j12` as a portable constant.

## Tests

```sh
meson test -C build --print-errorlogs
git diff --check
```

For memory-, lifetime- or concurrency-sensitive changes, use the applicable ASan/UBSan and TSan
validation described in [Testing](docs/development/testing.md) and preserve documented third-party
sanitizer qualifications.

## Repository language

The canonical language for repository documentation, agent contracts and production source comments
is English.

User-interface language is a separate product/localization concern.

## Status

Lardon3D is under active development.

The persistent pre-SfM pipeline is implemented through Tracks, the Sparse SfM C-G capability is
implemented and frozen at its documented boundaries, Phase H v1 is frozen, and MVS-M1 provides the
validated external OpenMVS boundary. Full real known-calibration Sparse SfM, dense publication,
mesh, texturing, viewer and live capture guidance remain future work.

The global maintenance checkpoint remains:

```text
global-maintenance-2026-09-01
```

The later real A6000 pre-SfM checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
```

Future reviews should preserve historical checkpoint meaning and review only the relevant delta
unless concrete evidence requires reopening an unchanged FROZEN boundary.

## License

Lardon3D is licensed under the MIT License.
