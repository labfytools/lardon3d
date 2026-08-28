# Lardon3D Resource Boundary — No New Resource Subsystem

## Status

**ACCEPTED** — architecture decision for the post-Gate C documentation freeze.

Current Sparse SfM gates A through G are **PASS / FROZEN**.
Project Database: current schema **v20**; historical v16 remains frozen.

This record is normative for the current architecture. It does not introduce
an implementation, a public API, a persistence format or a roadmap commitment.

## Context

Lardon3D contains a Resource Governor and persistent stores for images,
features, matches, geometric results, tracks and Sparse SfM data. The term
“resource” in the Governor denotes an execution budget, not a loadable runtime
object.

The repository does not define a generic Resource System, generic artifact
resolver, runtime cache, resource handle or runtime dependency graph.

## Current system

### 1. Persistent identity and metadata

Project Database v16 owns logical identities and relations. SQLite IDs identify
projects, ScanSets, images, assets, Feature Sets, results, tracks and Sparse
SfM records. SHA-256 values identify published file contents. Metadata stores
sizes, states, provenance and project-relative artifact paths.

Logical identity and physical content identity are distinct: two logical
images or Feature Sets may share one immutable physical asset.

### 2. Artifact resolution and validation

Resolution is specialized by each store. Readers derive project-relative paths,
validate content-addressed layouts, verify sizes and SHA-256 values, then
validate file formats and metadata. Feature, Match and Sparse SfM readers are
bounded and do not constitute a generic Resource System.

External import sources are persisted as provenance. After a successful import,
the managed asset is canonical and no longer depends on the source path. A
moved project does not make an absolute external source portable.

### 3. Runtime residency and ownership

Tasks and processing backends own their working buffers. Readers and temporary
objects have explicit local lifetimes. Vulkan ORB has its own bounded backend
state. There is no shared runtime resource cache, generic loaded-object handle,
generic unload operation or generic dependency lifetime.

### 4. Resource governance

The Resource Governor owns policy decisions for RAM, GPU, CPU and IO budgets.
It consumes immutable task estimates, samples system pressure, chooses bounded
lots and creates opaque transient reservations. A task must hold an active
reservation before executing its callback and releases it exactly once.

The Governor does not own persistent IDs, resolve paths, validate file formats,
load application objects, cache data or manage semantic dependencies.

## Problem statement

No current or clearly-next requirement demonstrates the need for a generic
runtime Resource System or a cross-store artifact resolver. Existing stores
already provide the identity, path and validation behavior required by their
contracts, while the Governor already provides resource admission.

Adding another layer now would invent ownership, syntax, persistence and
lifetime rules without a consumer and could contaminate frozen Gate B
persistence.

## Four-layer model

```text
identity / metadata
        ↓
specialized artifact resolution and validation
        ↓
task-owned runtime buffers and lifetimes
        ↓
Governor admission, reservation and bounded execution
```

The layers remain separate. No layer is promoted into a generic Resource
System by this decision.

## Resource Governor boundary

The Governor remains resource-type agnostic. It answers whether an operation
fits current budgets; it does not answer what an asset is or where it lives.
Tasks create estimates, request reservations, execute bounded work and release
reservations. The Governor owns only policy, budgets, pressure, lots and
reservations.

### Task demand declaration versus admission policy

**FROZEN.** A task producer owns the immutable declaration of the workload it
submits through the existing `Lardon3DResourceEstimate` contract. Constructing
that estimate from immutable payload, input shape and known implementation
characteristics describes what the task expects to require; it is not a
Governor policy decision.

The existing Task Runtime, queue and Governor remain responsible for deciding
whether and when the declared work is admitted and for creating the mandatory
reservation. Admission thresholds, current machine state, pressure, telemetry,
adaptive tuning, swap or scratch policy and future estimate calibration remain
outside the task producer. A producer never changes scientific inputs or
identity in response to resources.

Gate F may therefore construct the immutable Sparse SfM task estimate required
by `lardon3d_task_create_typed()` and submit the task through the normal queue.
It does not inspect resource snapshots, decide admission, create reservations,
change Governor or queue policy, or bypass the reservation invariant. The
estimate is operational metadata and is excluded from the Sparse SfM parameter
fingerprint, candidate identity and scientific determinism. Gate G owns
resource-management and admission policy, but Gate G core does not modify the
fields of the frozen Sparse SfM Gate F v1 estimate.

Gate F v1 freezes its declarative Sparse SfM RAM request as the checked sum
`128 MiB + I*64 KiB + T*2048 + O*512`, rounded upward to one MiB, where `I`,
`T` and `O` are immutable participating-image, Track and observation counts.
The complete amount is fixed RAM for one atomic batch; per-item RAM and all GPU
fields are zero, batch bounds are one, and the task requests one CPU thread and
one IO slot in the CPU class. These coefficients are conservative operational
policy inputs, not measured dynamically or included in scientific identity.

### Gate G resource-policy implementation

**PASS / FROZEN.** The G0a contract is implemented and validated. All seven G0
human decisions remain resolved.

#### Pending admission and snapshot freshness

When pending work exists but every candidate receives `WAIT`, the existing Task
Queue worker performs a timed wait of at most 500 milliseconds. An earlier
enqueue, resume, resource-change, cancellation or shutdown signal wakes it
immediately. On timeout it repeats the normal stable queue scan and obtains new
snapshots through the existing admission path. No monitor thread, scheduler or
subsystem is added. The separate polling interval for a running sequential task
waiting at `lardon3d_task_sequence_break()` remains 50 milliseconds.

Production snapshots use `CLOCK_MONOTONIC`. A directly supplied snapshot is
fresh through an age of exactly 1000 milliseconds. A snapshot older than 1000
milliseconds or timestamped in the future must produce `WAIT`, must not produce
`START` or `REDUCE_BATCH`, creates no reservation and does not mutate Governor
policy state. Normal production admission captures synchronously through
`lardon3d_resource_governor_reserve_available()` immediately before evaluation.
There is no last-known-good cache, grace cache or telemetry-cache subsystem.

A mandatory whole-snapshot capture failure is an operational/internal resource
error: the queued task becomes `FAILED`, no callback starts and no reservation
leaks. Unavailable optional CPU, memory or IO PSI and vmstat/swap telemetry is
unknown and asserts no artificial pressure. Actual GPU demand with no selected
GPU is `REJECT`; temporarily unknown required live VRAM for a selected dedicated
GPU is `WAIT`. VRAM availability is never fabricated.

#### Conservative RAM accounting and platform

Gate G core preserves the conservative live admission model:

```text
available_ram = max(
    0,
    min(MemAvailable, physical_ram)
        - host_ram_reserve
        - active_charged_reservations
)
```

The implementation retains its existing checked, saturating subtraction order.
The host reserve remains one quarter of physical RAM and the emergency floor
remains one eighth of physical RAM.
Possible overlap between `MemAvailable` and already-materialized memory from an
active reservation is accepted. The deliberate bias is false `WAIT`, not
overcommit. Gate G adds no RSS tracking, materialization state, allocation
measurement, reservation resizing or live per-task monitoring. The current
single-worker topology remains unchanged.

Gate G core supports a native unconstrained Linux host process. Capacity uses
the existing `_SC_PHYS_PAGES`, `_SC_PAGESIZE` and `/proc/meminfo` mechanisms.
It is not cgroup v2, systemd `MemoryMax`, `RLIMIT_AS` or `RLIMIT_DATA` aware and
does not claim correct host-capacity admission inside a tighter constrained
container or service. Effective constrained-runtime accounting is deferred. No
cgroup write, systemd dependency or new dependency is authorized.

#### Sparse SfM estimate authority

Gate G consumes and restores the exact Sparse SfM Gate F estimate above and
does not alter any producer field. Batch adaptation is permitted only for task
contracts whose existing minimum/maximum range allows it; Sparse SfM remains
fixed at batch one. Restart retains the estimate persisted when the task was
created and evaluates it against newly captured machine telemetry.

Any future coefficient change requires a separate explicit review and an
operational formula/version contract applying only to newly created tasks. It
must not alter F0, candidate identity, Gate D/E parameters or existing persisted
tasks. Estimate-formula refinement is not part of Gate G core.

#### Selected GPU

Gate G core supports one selected GPU and no multi-GPU scheduling. Hardware
Profile deterministically selects the lowest numeric `/sys/class/drm/cardN`
accepted by its selection rules and retains that identity internally. Snapshot
capacity and live usage must refer to that same device; Resource Snapshot must
not perform an independent first-usable-GPU selection. Memory is never summed
across devices and reservations are not made per device.

Dedicated memory uses the selected device's capacity and usage. UMA/shared GPU
demand is charged exactly once against system RAM and never against a second
fictitious VRAM pool. Backend and fallback selection remain task-producer/task
contract responsibilities, not Governor policy. Multi-GPU and complex hybrid
topologies are deferred.

#### Scratch, persistence and science

Scratch and external-storage support is excluded from Gate G core. It adds no
scratch estimate fields, demand/availability API, manager, allocator, removable
media monitor, mount logic, capacity policy, spill contract, cleanup contract,
project scratch path or SSD detection. Sparse SfM has no scratch consumer,
spill algorithm or out-of-core path. Swap, zram and external SSD capacity never
enlarge scientific RAM admission.

No automatic `swapon`, `swapoff`, mount, unmount, formatting, partitioning or
destructive cleanup is authorized. A future feature requires a real task
consumer, an explicit task-specific contract, user opt-in where appropriate and
a separate architecture review. No dormant generic API is introduced.

Gate G core requires no Project DB v18, resource-history, reservation,
telemetry, policy or scratch table. Reservations and snapshots remain ephemeral;
the generic Task checkpoint already owns estimate durability. Resource policy,
machine data and hardware identity never enter F0, `sfm_version`, candidate
identity or a resource fingerprint. No resource-policy version is required for
Gate G core.

Gate G never changes scientific thresholds, seeds, iteration limits, image or
track selection, Bundle Adjustment parameters, Gate D, Gate E, F0, candidate
identity, Track Model, Track Builder scientific semantics, Feature Store,
calibration identity or Project DB reconstruction semantics.

#### Known derivable implementation defects

These implementation obligations required no further human policy decision and
are implemented:

- **G-D01:** `UINT64_MAX` is the final valid reservation ID; the following
  reservation creation fails without an executable decision, reservation or
  accounting charge.
- **G-D02:** pressure, recovery and slow-start streak counters must saturate at
  the largest meaningful threshold and never wrap.
- **G-D03:** selected dedicated-GPU capacity and live usage must refer to the
  same Hardware Profile-selected DRM device.

`TOTAL REMAINING GATE G HUMAN DECISIONS: 0`.

#### Closure validation

The complete normal suite passes 41/41 and the Gate G resource/task core passes
the targeted ASan/UBSan/LSan validation with leak detection enabled. The three
OpenCL-touching tests `candidate-pair-task`, `feature-task` and
`precision-consolidation` pass functionally and under ASan/UBSan with leak
detection disabled. Their LeakSanitizer-only termination has the identical
external `/opt/cuda/lib64/libOpenCL.so` signature of 3808 bytes in 68
allocations and is not classified as a Lardon3D Gate G leak. The real-machine
`orb-vulkan-backend` test, `git diff --check`, and the complete human diff review
pass. No Gate G human decision or implementation blocker remains.

## Project DB boundary

The historical Project DB v16 migration and Sparse SfM reconstruction model
remain unchanged and frozen. Gate F is authorized to advance the current schema
head to v17 only with the dedicated `sparse_sfm_tasks` typed-payload table; that
task persistence does not add a resource table, handle table, cache table,
dependency table or resource-policy field.

No further schema expansion, bundle redesign, Track Model change, Track Builder
change or persistence API redesign is part of this resource-boundary scope.

## Options considered

### A. Governor evolution

Rejected for the current ticket. The Governor is already the correct owner of
budgets, estimates, pressure, lots and reservations. Extending it toward
identity or artifact ownership would violate the current boundary.

### B. Internal artifact resolver

Deferred as a possible future refactoring, not a current subsystem. It may be
reconsidered if concrete duplication appears in path resolution,
canonicalization, artifact validation or moved-project handling.

### C. Generic Resource System

Rejected for the current ticket. No demonstrated requirement needs shared
runtime residency, cache eviction, explicit unload, stable handles, stale-handle
protection, runtime dependency graphs, asynchronous loading, streaming or
CPU/GPU dual residency.

### D. No new subsystem

Accepted. Existing stores and the Governor cover current requirements while
preserving independent ownership and bounded execution.

## Decision

**NO_NEW_SUBSYSTEM**

The repository must not add a generic Resource System, generic artifact
resolver or Governor asset integration as part of the current architecture.

## Rationale

- Existing persistent identities are sufficient.
- Existing stores already validate their own artifacts.
- Existing task and backend ownership is explicit and bounded.
- Existing Governor policy is sufficient for resource admission.
- No new public syntax or ABI is required.
- Gate B v16 remains untouched.
- The decision is reversible if a concrete future requirement appears.

## Current scope

- Preserve the four-layer separation above.
- Continue using specialized readers and stores.
- Keep the Governor policy-oriented and independent of asset identity.
- Treat Gate A, Gate B and Gate C as closed at their verified boundaries.
- Correct factual documentation drift without turning future designs into
  current contracts.

## Explicit non-goals

- Resource manager or generic loader.
- Global runtime cache or eviction policy.
- Generic resource handles or stale-handle protection.
- Generic dependency graph or cycle management.
- Streaming or asynchronous resource loading.
- CPU/GPU dual-residency manager.
- Project DB schema beyond the additive Gate F v17 typed-task migration.
- Bundle redesign.
- Governor integration with persistent asset identity.
- BA, new orchestration, Task Runtime redesign or renderer redesign.

## Syntax impact

No new syntax is required. Do not introduce `resource://`, `asset://`, UUID,
manifest or handle syntax without a later concrete requirement and decision.
Existing SQLite, Feature File, Match File, checkpoint and specialized path
contracts remain authoritative in their own domains.

## Persistence impact

The historical Project DB v16 migration remains unchanged. Gate F adds only the
v17 typed-task payload already described above. Existing IDs, hashes, metadata
and relative paths are not renamed, generalized or duplicated.

## Memory / resource impact

None beyond existing task estimates and Governor reservations. No new cache or
resident object ownership is introduced. Existing bounded readers and backend
buffers remain responsible for their own memory.

## Concurrency impact

None. No new shared mutable state, worker, callback, lock or lifetime protocol
is introduced. Existing Governor mutex/condition ownership remains unchanged.

## Security impact

No new path-resolution surface is introduced. Existing specialized validation,
content hashing, bounded input handling and protected file opening remain in
force. No generic external-file access is added.

## Testing requirements for future changes

Future store changes must retain their own path, hash, size, format,
publication and corruption tests. Future Governor changes must retain admission,
reservation, pressure, bounds and concurrency tests.

Only if a future requirement creates a shared resolver or runtime residency
layer should new syntax, ownership, lifetime, cache, dependency and resource
scale tests be designed.

## Future extension triggers

A resolver may be reconsidered only after concrete cross-store duplication in:

- path resolution;
- canonicalization;
- artifact validation; or
- moved-project handling.

A generic Resource System may be reconsidered only after multiple real
requirements emerge among:

- shared runtime residency;
- cache and eviction;
- explicit unload;
- stable handles and stale-handle protection;
- runtime dependency graphs;
- asynchronous loading;
- streaming;
- CPU/GPU dual residency; or
- invalidation.

These are trigger conditions, not current requirements or roadmap commitments.

## Gate B freeze proof

- No schema or persistence code is changed by this decision.
- No new persistent identity is introduced.
- No Track Model or Track Builder contract is changed.
- No bundle or migration is introduced.
- Governor ownership remains limited to execution budgets.
- At the Gate B freeze, Project DB remained v16; the later additive Gate F v17
  migration does not alter that historical proof.

Therefore Gate B remains **PASS / FROZEN**.

## Open questions

None block the current decision. Future viewer, dense reconstruction, artifact
reconciliation or GPU residency requirements must be evaluated independently
when their implementations become concrete. Future questions do not create
current implementation requirements.
