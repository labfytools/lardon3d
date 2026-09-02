# Lardon3D Architecture Overview

## Purpose

Lardon3D is a persistent, incremental, resource-aware photogrammetry engine for Linux.

The TUI is the operational control center for projects, acquisition, Tasks, durable progress,
resource state, optical configuration and optional external-storage control. Rich visualization and
live acquisition remain separate product areas and must consume validated snapshots rather than
mutable worker buffers.

## Current authority

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The current Project DB head is additive:

```text
v22  Selected scientific execution foundation
v23  Generic optical-context overlay
v24  raw.develop.batch/1 persistence
v25  features.extract.batch/1 persistence
```

Earlier schema versions remain valid historical contracts where their documentation says so. Current
documentation must not present an older version as the active head merely because that version owns a
specific historical scientific or persistence boundary.

## Global execution model

```text
TUI / Project
    |
    v
Task
    |
    v
immutable resource estimate / capability
    |
    v
bounded Task Queue
(one active callback)
    |
    v
Resource Governor
(admission + reservation)
    |
    v
admitted owner callback
    |
    +--> bounded internal participants when justified
    |
    v
deterministic owner publication
    |
    v
durable checkpoint / restart boundary
    |
    v
validated snapshot consumers
```

The Queue owns bounded dispatch and backpressure. The Governor owns production resource admission.
The active Task owns its execution state and any bounded internal participants. Scientific stores own
their own identities and artifact validation.

No layer may silently absorb another layer's authority.

## Canonical resource objective

Lardon3D first preserves the interactive host reserve required for normal workstation use. Safe and
useful capacity beyond that reserve belongs to the active workload.

The objective is:

```text
MAXIMUM_SAFE_USEFUL_THROUGHPUT
```

This does not mean maximizing utilization percentages for appearance. It means using the largest
currently safe and useful CPU, RAM, I/O and validated accelerator capability while preserving
scientific identity, deterministic publication, host responsiveness and pressure limits.

The companion rule is:

```text
SERIALISM_REQUIRES_PROOF
```

Per-item atomicity does not imply cross-item serialization. Ordered or owner-only durable publication
does not imply serial preparation. Independent work should expose bounded concurrency when exact
science and persistence semantics can be preserved.

A CPU1 or batch1 path may remain when serialism, a measured scaling knee, memory, I/O, GPU execution,
exclusive state or another concrete constraint justifies it. Historical conservative descriptors are
not portable performance ceilings.

On the current validation host, the normal observed outcome is approximately:

```text
16 logical CPUs total
4 logical CPUs reserved for interactive host use
12 logical CPUs available to the compute pool
~3 GiB MemAvailable hard reserve
Radeon 780M UMA available to validated and useful GPU backends
```

These values are reference-host observations, not product constants.

## Current components

### Project

Persistent project lifecycle, stable identity, directory layout and Project DB ownership.

**Status:** IMPLEMENTED

### Project Database

SQLite owns durable logical identities, relations, typed Task payloads, scientific metadata and
references to external immutable artifacts.

The current schema head is v25. The scientific meaning of historical rows remains owned by the
versioned contracts that created them.

**Status:** CURRENT / v25

### Import and ScanSets

Import materializes managed immutable assets and logical images under explicit ScanSets. Provenance
from external source paths remains distinct from managed asset identity.

**Status:** IMPLEMENTED

### Capture / Asset Provenance

Capture identity remains distinct from file, Asset, `image_id`, SHA-256, path, Task ID and campaign
group ID. RAW and JPEG siblings may belong to one physical Capture without becoming one file or one
scientific image identity.

**Status:** PASS / FROZEN

### Bounded acquisition discovery and campaign execution

Discovery and planning are bounded and deterministic. Automatic grouping requires the documented
strong evidence; otherwise explicit caller confirmation remains `CALLER_EXPLICIT`.

Durable campaign execution uses the existing Task, Queue, Governor and Project DB recovery model.

**Status:** PASS / FROZEN

### Photo Quality Triage

Quality analysis is an operational selection layer, not a scientific identity. It produces explicit
GOOD / SUSPECT / REJECT recommendations and keeps human overrides distinct.

**Status:** PASS / FROZEN

### Selected scientific execution

The selected-execution snapshot binds the retained acquisition groups to explicit scientific
representations without inferring identity from paths, filenames or metadata.

**Status:** PASS / FROZEN

### Optical profiles and calibration selection

Project DB v23 provides generic camera-body profiles, lens profiles, optical configurations,
campaign/Capture assignments, calibration profiles and explicit compatible calibration selection.

Electronic metadata aliases are exact. Manual lenses without EXIF are normal. Missing or ambiguous
identity remains unresolved rather than guessed.

**Status:** IMPLEMENTED / VALIDATED

### RAW development

The historical `raw.develop/1` path remains valid. Project DB v24 adds
`raw.develop.batch/1`, allowing bounded independent RAW preparation with deterministic owner-only
publication in selected-item order.

Per-Capture publication remains atomic while cross-Capture preparation may be concurrent.

**Status:** IMPLEMENTED / VALIDATED

### Feature Store

Feature Store publishes immutable content-addressed Feature Files and bounded typed readers for ORB,
SIFT and RootSIFT data.

The historical `features.extract/1` task remains valid. Project DB v25 adds
`features.extract.batch/1`, which prepares independent selected images with bounded participants and
publishes Feature Sets deterministically through the owner callback.

**Status:** IMPLEMENTED / VALIDATED

### Visual Index

The ORB LSH Visual Index is persistent and segmented. It indexes homogeneous Feature Sets in bounded
updates and retrieves candidate relationships without performing geometric validation.

**Status:** IMPLEMENTED

### Candidate Pair

Candidate Pair generation answers which image pairs are worth exploring. Scientific pair identity and
canonical ordering remain independent from the amount of execution parallelism used to prepare them.

Candidate currently uses a bounded coupled CPU/batch ladder because additional CPU cannot exercise
additional independent pair work while the admitted item window remains one.

**Status:** IMPLEMENTED

### Matcher

Matcher produces deterministic Match Results from Feature Sets.

ORB supports the validated Vulkan hot path with exact CPU fallback. SIFT and RootSIFT remain CPU
OpenCV L2 paths. GPU use is selected only when a backend is both validated and useful.

**Status:** IMPLEMENTED

### Geometric Verification

Geometric Verifier v3 performs Fundamental USAC/MAGSAC verification with bounded restartable Task
execution. Scientific solver parallelism remains controlled by its contract while cross-parent
preparation can use bounded Governor-admitted participants.

**Status:** IMPLEMENTED / VALIDATED

### Track Model and Track Builder

Track Model v1 and Track Builder publish immutable Track Sets with deterministic identity and bounded
recovery semantics.

Retained real-data evidence includes:

```text
REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

**Status:** PASS / FROZEN

### Sparse SfM

Calibrated Sparse SfM capability is implemented through Gates C-G:

- Gate C: calibrated geometric primitives;
- Gate D: incremental reconstruction core;
- Gate E: final per-component Bundle Adjustment;
- Gate F: durable Task orchestration and atomic Project DB publication;
- Gate G: Governor admission and resource integration.

Phase H v1 adds incremental enrichment from immutable predecessor snapshots without redefining Gate F
scientific identity.

**Status:** C-G PASS / FROZEN; PHASE H V1 PASS / FROZEN

Implemented capability must remain distinct from execution on a particular real campaign. The
historical S21 and A6000 Engine Bay campaigns remain `CALIBRATION_UNAVAILABLE` for known-calibration
Sparse SfM. The retained A6000 proof intentionally stopped before Sparse SfM and Dense/MVS.

### MVS boundary

MVS-M1 provides the validated bounded external OpenMVS boundary and deterministic COLMAP/PLY exchange
contracts.

Durable dense publication, full Dense/MVS orchestration, mesh refinement, texturing and export remain
future work.

**Status:** PASS / FROZEN boundary

### Task Runtime

Tasks own lifecycle state, progress, cooperative pause/cancel behavior, sequence boundaries,
checkpoints and typed durable reconstruction.

**Status:** IMPLEMENTED

### Task Queue

The Queue provides bounded FIFO dispatch with one active callback, stable scanning and resource-WAIT
bypass behavior. It does not own resource policy.

**Status:** IMPLEMENTED

### Task Kind Registry

The production registry currently contains 16 Task kinds. Historical documents may legitimately
record smaller inventories at their checkpoint.

**Status:** IMPLEMENTED

### Hardware Profile and Resource Snapshot

Hardware Profile detects static host capability. Resource Snapshot captures current observable
capacity and pressure signals required for admission.

Unavailable optional telemetry remains unknown; it must not be fabricated.

**Status:** IMPLEMENTED

### Resource Governor

The Governor is the sole production authority for CPU, RAM, GPU, I/O admission and process-local
scratch leases where explicitly supported.

Pressure may throttle future admission. When pressure clears, useful resources must be eligible to
ramp back up; throttling is not a permanent lower ceiling.

UMA GPU allocations are charged exactly once against host RAM. Swap, zram and external scratch never
become admitted RAM.

**Status:** IMPLEMENTED / VALIDATED

### Optional external SSD controller

The UDisks2/GDBus controller owns the reviewed physical lifecycle for the exact
`LARDON_SWAP` / `LARDON_SCRATCH` device contract. It is not a second scheduler or Governor.

The Governor remains the sole production entry for scratch leases. The current 16 production Task
kinds have zero authoritative scratch consumption; visible storage capacity is not fabricated usage.

**Status:** CURRENT / VALIDATED OPERATIONAL

### TUI observatory and control center

ncurses input and rendering remain on the main thread. Runtime observation is bounded and coalesced.
The TUI exposes project state, Tasks, durable progress, resource state, optical configuration and SSD
control.

Validated layout boundaries are:

```text
full layout        >= 100x30
reference compact  72x20
minimum supported  60x15
```

Below the minimum, only the bounded terminal-too-small fallback is rendered.

Opening, closing or switching a project is a Queue/DB lifetime boundary: destroy and join the sole
Queue first, including finished callbacks, then close Project DB, recreate one empty Queue and rebind
observers.

**Status:** CURRENT / VALIDATED OPERATIONAL

## Publication and restart model

Long-running processing follows bounded sequence boundaries:

```text
admit bounded work
-> prepare bounded items
-> join participants
-> publish deterministic atomic results
-> persist typed cursor/state
-> checkpoint generic Task progress
-> release transient buffers
-> sequence break / re-admission
```

The exact order varies by Task contract, but durable progress must never claim work that has not
reached its authoritative publication boundary.

A crash may leave generic Task progress behind an already durable immutable result where the
documented retry contract permits exact reuse. Recovery must converge from durable identity; it must
not guess identity from paths, timestamps, basenames or similar metadata.

## Viewer and live boundaries

Viewer, coverage analysis, live localization and capture guidance remain future product areas.

The architecture requirement is already clear: visual consumers use validated immutable snapshots.
They do not access mutable worker buffers, own scientific identity or become a second execution
runtime.

Device-specific acquisition transports such as A6000 HDMI/USB or S21/mobile integration belong at the
acquisition adapter boundary. Heavy reconstruction remains PC-side.

## Core invariants

- No Task callback executes without a valid active reservation.
- Queue/runtime do not own resource policy.
- The Resource Governor is the sole production resource authority.
- ncurses remains owned by the main thread.
- Scientific and operational identities remain distinct.
- Task estimates and installed sequence contracts remain immutable for their defined lifetime.
- Memory, buffers, queues, files, descriptors, processes, threads and internal participants are bounded.
- Per-item atomicity does not imply cross-item serialization.
- Owner-only publication does not imply serial preparation.
- Validated and useful GPU backends are preferred when eligible and Governor-safe.
- UMA GPU memory is charged exactly once against host RAM.
- Swap, zram and scratch do not enlarge RAM admission.
- Failure and cancellation clean operation-owned transient resources.
- Atomic scientific publication never exposes partial results as complete.
- FROZEN scientific decisions are not reopened by resource-policy evolution.

## Current limitations and future scope

The following remain outside the current implemented execution model:

- general dependency DAG scheduling;
- multiple active Task callbacks / inter-Task worker pools;
- general CPU/GPU/I/O worker-pool architecture;
- multi-GPU scheduling;
- global orphan-artifact reconciliation;
- Visual Index compaction / larger-index evolution;
- authoritative Task scratch consumers;
- durable dense / mesh publication;
- full Dense/MVS orchestration;
- mesh refinement, texturing and final export workflow;
- viewer;
- offline coverage analysis;
- suggested supplementary viewpoints;
- live camera localization;
- live coverage overlay;
- A6000/S21 live acquisition integration;
- capture guidance;
- video ingestion and deterministic keyframe extraction.

Deferring inter-Task parallelism does not authorize accidental serialization inside the one active
Task. Bounded internal concurrency remains the current mechanism for independent work under
`SERIALISM_REQUIRES_PROOF`.

## Current summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

RAW_BATCH_PATH=raw.develop.batch/1
FEATURE_BATCH_PATH=features.extract.batch/1

REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN

SPARSE_SFM_CAPABILITY=C_G_PASS_FROZEN
REAL_A6000_SPARSE_SFM_EXECUTED=NO
REAL_A6000_DENSE_MVS_EXECUTED=NO

TUI=CURRENT_VALIDATED_OPERATIONAL
VIEWER=FUTURE
LIVE_CAPTURE_GUIDANCE=FUTURE
```

Detailed contracts remain owned by the specialized architecture documents. This overview summarizes
current repository state and must not replace those normative documents.
