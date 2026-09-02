# Lardon3D Resource Boundary — No New Resource Subsystem

## Status

**ACCEPTED / CURRENT.**

This document preserves the historical no-new-subsystem decision while reconciling it with the
current repository state.

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
NO_NEW_SUBSYSTEM=ACCEPTED
```

Sparse SfM Gates A through G remain **PASS / FROZEN** at their documented scientific boundaries.

The current Project DB head is v25. The additive lineage is:

```text
v22  selected scientific execution foundation
v23  generic optical-context overlay
v24  raw.develop.batch/1 persistence
v25  features.extract.batch/1 persistence
```

The original Gate C/G decision did not create a generic Resource System. Later authorized additions,
including the v23 optics overlay, v24 RAW-batch Task persistence, v25 Feature-batch Task persistence
and the bounded UDisks2 SSD controller, remain on their own side of this boundary and do not create
one.

Historical schema numbers, Task counts and measurements remain valid where they describe the
checkpoint at which they were recorded. They are not current-state authority unless explicitly
labelled current.

## Purpose

This document defines the separation between:

1. scientific and persistent identity;
2. specialized artifact resolution and validation;
3. Task-owned runtime residency;
4. Resource Governor admission and execution budgets.

It also records the architectural decision that Lardon3D does **not** currently need a generic
runtime Resource System, generic artifact resolver, runtime dependency graph or cross-store cache.

The runtime resource policy itself is owned by
[Resource Governor](resource_governor.md). This document defines the architectural boundary around
that policy and must not duplicate or weaken it.

## Context

Lardon3D contains a Resource Governor and persistent stores for images, Features, matches, geometric
verification results, Tracks and Sparse SfM data.

The word *resource* in Resource Governor means an execution budget or capacity. It does not mean a
generic loadable application object.

The repository therefore does not currently define:

- a generic Resource System;
- a generic artifact resolver;
- a shared runtime object cache;
- generic loaded-object handles;
- generic unload semantics;
- a runtime dependency graph;
- a cross-store residency manager.

## Current four-layer model

```text
persistent identity / metadata
        |
        v
specialized artifact resolution and validation
        |
        v
Task-owned runtime buffers and lifetimes
        |
        v
Resource Governor admission, reservation and bounded execution
```

The four layers remain separate. No layer becomes a generic Resource System by implication.

## 1. Persistent identity and metadata

Project DB v25 is the current persistent logical-state head.

Earlier layers remain valid foundations:

- v16: historical Sparse SfM persistence model;
- v17: durable Sparse SfM Task payload;
- v18: Phase H v1 incremental reconstruction metadata;
- v19: Capture / Asset Provenance;
- v20: durable acquisition-campaign execution;
- v21: Photo Quality Triage;
- v22: selected scientific execution;
- v23: generic optical context;
- v24: selected RAW-batch Task persistence;
- v25: selected Feature-batch Task persistence.

SQLite IDs identify logical project objects. SHA-256 identifies immutable published bytes.

These identities remain distinct. In particular:

```text
Capture != file
Capture != Asset
Capture != image_id
Capture != SHA-256
Capture != path
Capture != basename
Capture != Task ID
Capture != campaign group ID
```

Resource management must never generalize, merge or reinterpret these identities.

Logical identity and physical-content identity are separate. Multiple logical objects may refer to one
immutable physical asset when the relevant store contract permits it.

## 2. Specialized artifact resolution and validation

Artifact resolution remains owned by the relevant store or subsystem.

A specialized reader may:

- derive a project-relative path;
- validate a content-addressed layout;
- verify expected size;
- verify SHA-256;
- validate a versioned file format;
- validate bounded metadata;
- reject corruption.

Feature, Match, Track, checkpoint, calibration and reconstruction readers do not become a generic
Resource System merely because they perform similar validation steps.

External import paths are provenance. Once import publishes a managed asset, that managed asset is the
canonical project-owned content. Moving a project does not make an absolute external source path
portable.

A future internal artifact resolver may be reconsidered only if concrete cross-store duplication
justifies it. Such a refactor is not authorized by this document.

## 3. Runtime residency and ownership

Tasks and their processing backends own working buffers for the lifetime defined by their execution
contract.

Readers, decoded objects and temporary structures have explicit local lifetimes.

Validated backends may own bounded private state. For example, the Vulkan ORB backend owns its
validated bounded device/pipeline/request state.

The current architecture intentionally has no:

- shared generic residency cache;
- generic loaded-object handle;
- generic unload API;
- cache-eviction protocol;
- runtime object dependency lifetime;
- cross-backend residency manager.

Owner-only durable publication does not imply serial preparation. Runtime ownership and publication
ordering are separate concerns.

## 4. Resource governance

The Resource Governor is the sole production authority for runtime resource admission.

It owns policy for:

- CPU;
- RAM;
- GPU capacity and eligibility;
- I/O admission;
- pressure response;
- bounded batch/concurrency selection;
- process-local scratch lease orchestration where explicitly supported.

The Governor does **not** own:

- persistent scientific IDs;
- Capture identity;
- Asset identity;
- artifact paths;
- file-format validation;
- application-object loading;
- semantic dependency graphs;
- scientific thresholds;
- scientific fingerprints.

A production Task must hold the required active reservation before executing admitted work and must
release it according to the established runtime contract.

## Canonical utilization objective

The current human-authorized operational policy is:

```text
MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF
```

The system first preserves the interactive host reserve. Safe and useful capacity beyond that reserve
belongs to the active Lardon3D workload.

On the current reference host, the normal observed policy outcome is approximately:

```text
16 logical CPUs total
4 logical CPUs reserved for interactive host use
12 logical CPUs available to the compute pool
~3 GiB MemAvailable preserved as the hard RAM reserve
Radeon 780M UMA available to validated and useful GPU backends
```

These values are reference-host evidence, not portable product constants.

A future host must derive its usable capacity from actual topology, allowed affinity, memory,
pressure and backend capability. The current 12-compute-thread result must never become a global
product ceiling.

A long-running CPU1 or batch1 path with independent executable work is not automatically justified by
historical descriptors. It requires a concrete reason such as:

- true dependency or scientific serialism;
- a measured useful-scaling knee;
- memory pressure or bound;
- I/O saturation;
- validated GPU execution making additional CPU work useless;
- a publication dependency that cannot safely be separated from preparation;
- another measured and documented constraint.

Per-item atomicity does not imply cross-item serialization.

Ordered or owner-only publication does not imply serial preparation.

The accepted internal pattern is, where applicable:

```text
one admitted owner Task
    -> bounded independent participants / preparation
    -> join all participants
    -> deterministic owner-only publication
```

This pattern does not create a second scheduler or worker-pool subsystem.

## Task demand declaration versus admission policy

A Task producer declares immutable operational demand through the existing resource-estimate contract.
That declaration describes the workload shape and safe execution envelope; it is not an admission
decision.

The Task Runtime, Queue and Governor decide whether and when work is admitted.

A producer must not change scientific inputs, identities, thresholds or fingerprints in response to
machine pressure.

Resource estimates, measured scaling envelopes and backend eligibility remain operational metadata.
They do not become scientific identity unless a separate explicit scientific contract says otherwise.

### Historical Gate F v1 Sparse SfM estimate

Gate F v1 recorded this declarative Sparse SfM RAM estimate:

```text
128 MiB + I*64 KiB + T*2048 + O*512
```

rounded upward to one MiB, with immutable participating-image, Track and observation counts
`I`, `T` and `O`.

That historical Gate F v1 descriptor used:

- complete RAM as fixed RAM for one atomic batch;
- per-item RAM zero;
- GPU fields zero;
- batch one;
- CPU1;
- one I/O slot;
- CPU resource class.

The formula remains valid historical/FROZEN evidence for the Gate F v1 contract and existing persisted
Tasks that carry it.

It must **not** be generalized into a repository-wide rule that all future or revisited operational
paths remain CPU1/batch1. The current `SERIALISM_REQUIRES_PROOF` authority governs any explicitly
authorized operational re-evaluation while scientific identity remains unchanged.

A change to a persisted estimate formula requires its own explicit compatibility/version contract and
must never silently reinterpret an existing Task.

## Gate G resource-policy implementation

Gate G remains **PASS / FROZEN** at its validated boundary.

### Pending admission and snapshot freshness

When pending work exists but every candidate receives `WAIT`, the existing Queue worker performs the
validated bounded timed wait. Enqueue, resume, resource change, cancellation or shutdown may wake it
earlier.

The original Gate G validated values remain historical/current implementation facts where unchanged:

- pending-work timed wait: at most 500 ms;
- running sequential `sequence_break()` polling interval: 50 ms;
- production time source: `CLOCK_MONOTONIC`;
- directly supplied snapshot freshness: through exactly 1000 ms.

A future-timestamped or stale required snapshot must not produce an executable reservation.

There is no last-known-good cache, grace cache or telemetry-cache subsystem.

A mandatory whole-snapshot capture failure is an operational failure: no callback starts and no
reservation may leak.

Optional unavailable telemetry remains unknown rather than asserting fabricated pressure.

Required GPU demand with no eligible selected GPU is rejected. Required dedicated-GPU live-memory
capacity that is temporarily unknown waits rather than inventing VRAM.

### RAM accounting

The validated Gate G model remains conservative:

```text
available_ram = max(
    0,
    min(MemAvailable, physical_ram)
        - host_ram_reserve
        - active_charged_reservations
)
```

Subtraction is checked and saturating.

On the reference host, the hard reserve is approximately 3 GiB `MemAvailable`. The 3–4 GiB interval
is a caution/pressure region, not a permanent extra 1 GiB subtraction from every Task.

Smaller hosts must derive a bounded reserve without making the reference-host value a constant.

Potential overlap between `MemAvailable` and already materialized memory charged to an active
reservation is intentionally conservative. The bias is false `WAIT`, not overcommit.

The current native-host capacity model does not claim cgroup-v2, systemd `MemoryMax`, `RLIMIT_AS` or
`RLIMIT_DATA` constrained-service correctness. Supporting tighter constrained-runtime accounting is a
separate capability.

### CPU policy

CPU admission is topology- and affinity-aware where the validated Governor implementation provides
that evidence.

The current reference-host compute pool is approximately 12 logical CPUs after preserving the
interactive reserve. This is not a hardcoded product maximum.

A caller whose process affinity is already constrained must not receive a second artificial global
12-thread ceiling merely because the reference host produced that result.

### GPU policy

Gate G currently supports one selected GPU for production admission; multi-GPU scheduling remains
deferred.

Hardware Profile and Resource Snapshot must agree on the same selected device.

Dedicated GPU capacity and usage refer to that selected device only.

UMA/shared GPU allocations are charged exactly once against host RAM. No fictitious second VRAM pool
may enlarge capacity.

A backend that is both **VALIDATED AND USEFUL** should be preferred when Governor-safe and eligible.
An unvalidated GPU path must never be promoted merely to increase GPU utilization.

Backend correctness, exact fallback semantics and scientific equivalence remain backend/Task
contracts, not generic Governor identity policy.

## Scratch, swap and external SSD

Swap, zram and external scratch never enlarge admitted RAM.

Scratch availability is a capacity, not fabricated Task usage.

The bounded external SSD controller owns physical discovery and lifecycle for the reviewed
UDisks2/GDBus contract. It is not:

- a scheduler;
- a second Governor;
- a Task admission system;
- a generic allocator;
- Project DB persistence;
- a generic Resource System.

The reviewed physical identity uses the exact Drive plus filesystem labels/UUIDs required by the SSD
contract.

The controller must fail closed for incomplete, contradictory, replaced or unknown physical state.

It does not silently:

- format;
- partition;
- fsck/repair;
- power off;
- force swap removal;
- delete project data;
- execute ad-hoc shell control.

No unrelated code may invent parallel `swapon`, `swapoff`, mount/unmount or destructive control paths
outside the reviewed controller/Governor ownership.

### Governor scratch ownership

The controller's bounded snapshot may be registered with the Resource Governor.

The Governor wrappers are the sole production entry for process-local scratch lease acquire/release.

Drain must refuse new leases and wait for exact release of existing leases.

The controller does not invent which Tasks are eligible for scratch. A future scratch consumer
requires an explicit Task-specific contract defining:

- eligibility;
- storage bound;
- lease lifetime;
- path/workspace ownership;
- restart semantics;
- cancellation cleanup;
- failure cleanup;
- safe drain interaction.

At the current checkpoint, all 16 production Task kinds have zero authoritative scratch consumption.

A future dense/mesh Task may become a real consumer, but that capability is not created by this
document.

### Physical-state fail-closed behavior

Pairing or control authority requires coherent currently detected state including the reviewed Drive
and required UUID-bearing partitions.

Partial `DETECTED` state is observable but non-actionable.

A disconnected sticky hazard may preserve identity only as non-allocating `ERROR`.

Only the exact original tuple after complete reconnection may regain drain authority.

Controller source generation may legally saturate at `UINT64_MAX`. Public same-generation material
changes remain stale and may not regrant authority. The reviewed private Governor wrapper may
reconcile only its own exact serialized acquire/release completion at saturation; this exception does
not create a generic same-generation-update rule.

## Pressure and recovery

CPU, memory and I/O pressure may reduce admission.

Active swap-in/swap-out deltas may contribute to pressure decisions.

When pressure clears, useful resources must be re-admitted. A reduced rung must not become a
permanent throttle merely because it was once selected under pressure.

Host responsiveness is protected by the explicit reserve and pressure feedback, not by intentionally
leaving additional safe and useful resources idle.

## Persistence boundary

Resource reservations, live machine telemetry and transient policy state remain ephemeral.

Project DB through v25 contains no generic:

- resource-history table;
- reservation table;
- resource-object table;
- resource-handle table;
- cache table;
- dependency graph;
- generic scratch-allocation table.

The additive v23, v24 and v25 migrations do not change this decision:

- v23 stores optical context;
- v24 stores the typed owner association for `raw.develop.batch/1`;
- v25 stores the typed owner/progress contract for `features.extract.batch/1`.

They are persistence contracts for their own domains, not a generic resource layer.

The generic Task checkpoint owns persisted Task estimate durability where applicable.

Resource policy, current machine data and hardware identity do not enter scientific fingerprints merely
because they influence admission.

This document authorizes no new schema version.

Future Project DB changes beyond v25 require explicit human authority and their own persistence
contract.

## Scientific boundary

Resource policy must never silently change:

- scientific thresholds;
- deterministic seeds;
- iteration limits;
- image selection;
- Track selection;
- calibration identity;
- Sparse SfM candidate identity;
- Bundle Adjustment scientific parameters;
- Track Model semantics;
- Track Builder scientific semantics;
- Feature Store scientific identity;
- Geometric Verification scientific identity.

Operational optimization is allowed only within explicitly authorized operational contracts while
preserving the exact scientific result.

## Historical Gate G implementation obligations

The following reviewed implementation obligations remain acquired:

- **G-D01**: `UINT64_MAX` is the final valid reservation ID; the following reservation creation fails
  without an executable decision, reservation or accounting charge.
- **G-D02**: pressure, recovery and slow-start streak counters saturate at the largest meaningful
  threshold and never wrap.
- **G-D03**: selected dedicated-GPU capacity and live usage refer to the same Hardware
  Profile-selected DRM device.

The historical Gate G record closed with:

```text
TOTAL REMAINING GATE G HUMAN DECISIONS=0
```

## Validation evidence and qualification

Historical validation evidence must remain interpreted at its checkpoint.

Gate G originally closed with its complete normal suite and targeted resource/task sanitizer coverage.

The OpenCL-loading LeakSanitizer qualification is historical and remains valid: the externally sourced
`/opt/cuda/lib64/libOpenCL.so` signature was not reclassified as a Lardon3D leak.

The later global-maintenance checkpoint supersedes the need to repeat that whole historical validation
for unchanged code. It established:

```text
GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN
```

with fresh portable/Vulkan builds, normal suites, applicable sanitizer work, the documented
OpenCV/TBB/third-party qualifications, concurrency review, ABI/header checks and independent final
review.

The maintenance checkpoint is:

```text
global-maintenance-2026-09-01
```

Unchanged FROZEN systems inherit that evidence unless a concrete delta crosses their boundary.

The later real A6000 checkpoint:

```text
real-a6000-pre-sfm-2026-09-02
```

adds operational and real-data proof through Geometric Verification and Tracks. It does not create a
generic Resource System and does not execute Sparse SfM or Dense/MVS.

## Options considered

### A. Turn the Governor into artifact identity/storage ownership

Rejected.

The Governor owns admission and execution budgets. It must not become the owner of scientific identity,
artifact paths, file formats or application-object lifetime.

### B. Internal artifact resolver

Deferred.

A shared resolver may be reconsidered only after concrete cross-store duplication demonstrates a real
maintenance or correctness problem.

### C. Generic Resource System

Rejected for the current architecture.

No demonstrated requirement currently needs shared generic residency, cache eviction, explicit unload,
stable generic handles, stale-handle protection, runtime dependency graphs, asynchronous loading,
streaming or CPU/GPU dual residency.

### D. Preserve the four-layer model

Accepted.

Existing specialized stores plus the Task/Queue/Governor architecture satisfy current requirements
without inventing another subsystem.

## Decision

```text
NO_NEW_SUBSYSTEM
```

Do not add a generic Resource System, generic artifact resolver or persistent-asset ownership layer
inside the Resource Governor merely because multiple subsystems consume files or memory.

This is an architectural boundary, not a prohibition on all future additive capabilities.

A concrete future requirement may justify revisiting the decision through an explicitly scoped human
ticket.

## Explicit non-goals

This decision does not authorize:

- a generic loader;
- a global runtime cache;
- cache eviction policy;
- generic resource handles;
- stale-handle infrastructure;
- generic dependency graphs;
- cycle management;
- asynchronous generic loading;
- generic streaming;
- CPU/GPU dual-residency management;
- multi-GPU scheduling;
- a new Project DB schema version;
- bundle redesign;
- Governor ownership of persistent Asset identity;
- a second scheduler;
- a second Queue;
- a second Governor;
- a second persistence subsystem.

## Syntax impact

No new syntax is required.

Do not introduce `resource://`, `asset://`, generic UUID handle syntax, generic resource manifests or
similar syntax without a later concrete requirement and explicit design decision.

Existing SQLite, Feature File, Match File, checkpoint, calibration and specialized path contracts
remain authoritative in their own domains.

## Memory impact

The no-new-subsystem decision itself adds no runtime cache or generic residency budget.

The SSD integration retains only its reviewed bounded controller state plus the Governor's bounded
registered copy and process-local lease accounting.

Existing readers, Tasks and backend implementations remain responsible for their own explicitly
bounded memory.

## Concurrency impact

The decision creates no second worker topology.

The reviewed SSD controller owns its own bounded synchronization.

The TUI/controller integration may own the explicitly reviewed joinable operation thread, but this is
not a scheduler.

The Governor must not hold its registry mutex while entering the controller, and the controller must
not call back into the Governor.

Application shutdown order remains ownership-sensitive:

```text
destroy/join Queue and release Task leases
-> checked unregister of SSD binding
-> destroy SSD controller
-> destroy Resource Governor
```

## Security impact

No generic path-resolution surface is introduced.

Existing specialized validation, content hashing, bounded parsing and protected file-opening behavior
remain authoritative.

The resource boundary does not authorize arbitrary external-file access or shell execution.

## Testing requirements for future changes

A future specialized store change must retain tests for the relevant:

- path contract;
- SHA-256/content identity;
- size bound;
- format validation;
- publication ordering;
- corruption rejection;
- retry/restart semantics.

A future Governor change must retain tests for the relevant:

- admission;
- reservation;
- release;
- pressure;
- recovery;
- bounds;
- topology/affinity;
- UMA accounting;
- backend eligibility;
- concurrency.

Resource-sensitive tests and benchmarks follow the same canonical policy as production:

```text
preserve interactive host reserve
-> use maximum safe useful remaining resources
```

Artificial test serialization requires a concrete reason such as shared mutable fixtures, sanitizer
behavior, deterministic dependency, memory/I/O pressure or exclusive GPU state.

## Future extension triggers

An internal artifact resolver may be reconsidered only after concrete cross-store duplication appears
in areas such as:

- path resolution;
- canonicalization;
- artifact validation;
- moved-project handling.

A generic Resource System may be reconsidered only after multiple real requirements emerge among:

- shared runtime residency;
- cache and eviction;
- explicit unload;
- stable handles and stale-handle protection;
- runtime dependency graphs;
- asynchronous loading;
- streaming;
- CPU/GPU dual residency;
- invalidation.

These are trigger conditions, not current roadmap commitments.

## Historical Gate B freeze proof

The original Gate B freeze proof remains historical and valid:

- the Gate B decision itself changed no schema or persistence code;
- it introduced no new persistent identity;
- it changed no Track Model or Track Builder contract;
- it introduced no generic bundle or resource migration;
- Governor ownership remained limited to execution budgets;
- Project DB was v16 at that historical checkpoint.

Later additive migrations v17 through v25 do not rewrite that historical proof.

Therefore Gate B remains **PASS / FROZEN**.

## Current authority summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
GOVERNOR_RESOURCE_AUTHORITY=SOLE_PRODUCTION_AUTHORITY
GENERIC_RESOURCE_SYSTEM=ABSENT
NO_NEW_SUBSYSTEM=ACCEPTED
```

The four-layer separation remains the current architecture.

Resource-policy details are owned by `resource_governor.md`; reference-host measurements are owned by
`../performance/target_hardware.md`; engineering obligations are owned by `../../AGENTS.md`.

This document defines the boundary between those concerns and scientific/persistent identity.
