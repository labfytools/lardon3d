# Lardon3D Resource Boundary — No New Resource Subsystem

## Status

**ACCEPTED** — architecture decision for the post-Gate C documentation freeze.

Current Sparse SfM gates A through F are **PASS / FROZEN**. Gate G remains
open/planned.
Project Database: current schema **v17**; historical v16 remains frozen.

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
fingerprint, candidate identity and scientific determinism. Future Gate G owns
resource-management policy and operational refinement of that estimate.

Gate F v1 freezes its declarative Sparse SfM RAM request as the checked sum
`128 MiB + I*64 KiB + T*2048 + O*512`, rounded upward to one MiB, where `I`,
`T` and `O` are immutable participating-image, Track and observation counts.
The complete amount is fixed RAM for one atomic batch; per-item RAM and all GPU
fields are zero, batch bounds are one, and the task requests one CPU thread and
one IO slot in the CPU class. These coefficients are conservative operational
policy inputs, not measured dynamically or included in scientific identity.

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
