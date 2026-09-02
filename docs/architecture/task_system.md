# Task System

## Status

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

TASK_SYSTEM_STATUS=IMPLEMENTED
GENERIC_CHECKPOINT_VERSION=1

COMPUTE_GOVERNOR_V2=PASS/FROZEN
ORB_VULKAN_ASYNC_EXECUTION=PASS/FROZEN

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```

The Task System owns the lifecycle of executable work in Lardon3D.

It defines Task state, progress, cooperative pause/cancel, sequence boundaries, durable snapshots,
checkpoint publication and the runtime contract used by Queue and Resource Governor.

It does not own scientific identity, Project DB schema design, dependency/DAG planning, artifact path
resolution or resource-admission policy.

## Files

Primary implementation:

- `include/lardon3d/task.h`;
- `src/task.c`.

Task-specific reconstruction is provided through the Task Kind Registry. Queue admission and execution
ordering are documented separately.

## Core model

A Task represents durable executable intent plus transient runtime state.

The durable part includes:

- stable Task ID;
- immutable Task Kind/version;
- immutable durable resource estimate;
- saved/recovery state;
- bounded progress;
- timestamps;
- sequence count;
- Task-specific durable cursor/payload owned outside the generic checkpoint.

The transient part includes:

- callback;
- userdata;
- mutex/condition state;
- active reservation;
- current admitted execution contract;
- private capability envelope;
- resource-feedback state;
- worker-local execution details.

Transient runtime objects are never serialized into the generic checkpoint.

## Task states

The real states are:

```text
TASK_PENDING
TASK_RUNNING
TASK_PAUSED
TASK_CANCELLED
TASK_FAILED
TASK_COMPLETED
```

A sequence break is not a Task state.

It is an execution boundary at which the current reservation is released and the non-terminal Task
must be admitted again before more work executes.

## Public API

The public Task surface includes:

| Function | Purpose |
| --- | --- |
| `lardon3d_task_create()` | Create a Task and copy its immutable estimate |
| `lardon3d_task_destroy()` | Request shutdown as needed, join execution lifetime, destroy |
| `lardon3d_task_start()` | Execute while holding an active admitted reservation |
| `lardon3d_task_pause()` / `resume()` | Cooperative pause control |
| `lardon3d_task_request_cancel()` | Cooperative cancellation request |
| `lardon3d_task_checkpoint()` | In-memory cooperative checkpoint boundary |
| `lardon3d_task_sequence_break()` | Release current reservation and require re-admission |
| `lardon3d_task_snapshot()` | Copy bounded runtime-observation state |

Durable APIs include:

| Function | Purpose |
| --- | --- |
| `lardon3d_task_durable_snapshot()` | Copy durable generic fields under the Task mutex |
| `lardon3d_task_restore()` | Restore a Task without live execution state |
| `lardon3d_task_create_typed()` | Create a persistable Task with immutable kind/version |
| `lardon3d_task_restore_typed()` | Restore a typed Task and transfer userdata ownership |
| `lardon3d_task_checkpoint_save()` | Atomically publish a generic checkpoint v1 |
| `lardon3d_task_checkpoint_load()` | Read and validate a bounded checkpoint |
| `lardon3d_task_checkpoint_stage()` | Publish the staged checkpoint representation |
| `lardon3d_task_checkpoint_promote_staged()` | Promote staged state under the Task checkpoint lock |

Exact API declarations remain authoritative in the public headers.

## Observation API and ABI boundary

The historical layout of `Lardon3DTaskSnapshot` remains unchanged.

The additive `Lardon3DTaskObservation` type carries an observation-oriented copy that includes the
historical snapshot prefix plus current typed/runtime fields such as:

- Task Kind/version;
- durable progress counters;
- sequence count;
- installed execution contract.

`lardon3d_task_observation()` fills caller-owned memory while holding the Task mutex only long enough
to obtain a coherent copy.

No internal pointer escapes.

`lardon3d_task_set_durable_progress()` is owned by typed Task code and is used only after the
Task-specific durable prefix has been published.

Durable counters are not inferred from:

- generic percentage;
- Task message;
- Task display name.

A later generic `set_progress()` invalidates exact durable counters when required so observation does
not preserve stale exactness.

This observation seam serves the TUI and diagnostics. It does not change:

- generic checkpoint codec;
- Task identity;
- scientific identity;
- immutable durable estimate.

## Fundamental invariants

### Immutable durable estimate

The `Lardon3DResourceEstimate` stored with the durable Task remains immutable.

Compute Governor v2 may attach a private capability envelope to the live Task, but that envelope is not
the durable estimate and is not persisted as scientific identity.

The contract selected for one admitted sequence is also immutable until that sequence ends.

Only a later sequence may receive a different resource contract.

### Valid state transitions

The nominal lifecycle is:

```text
PENDING
-> RUNNING
-> COMPLETED | FAILED | CANCELLED
```

with cooperative pause/resume where supported.

No callback may force another thread into an undocumented state transition.

### Cooperative pause and cancellation

Pause and cancellation are checked at Task-specific safe boundaries.

A Task callback calls the generic checkpoint/pause/cancel interfaces where its scientific contract
permits interruption.

Non-interruptible third-party calls complete their current atomic work before the Task observes the
request.

No other thread forcefully kills a callback.

### Bounded progress

Generic progress remains bounded by the Task estimate/contract.

Task-specific exact durable cursors remain authoritative for restart and are not reconstructed from
generic percentage.

### Re-admitted restart

A restored non-terminal Task returns through:

```text
Task Kind Registry
-> Task Queue
-> Resource Governor
-> active reservation
-> callback
```

It does not resume with a stale pre-crash reservation.

### Short mutex hold

Task snapshot fields are copied under the Task mutex.

Serialization and file I/O happen after the mutex is released.

## Sequence model

A sequence is the resource-admission unit for work that can make bounded progress and then safely yield.

Conceptually:

```text
admit
-> install immutable sequence contract
-> execute bounded work
-> publish Task-specific durable result/cursor
-> generic checkpoint
-> release reservation
-> sequence_break
-> re-admit
```

A Task whose scientific contract is one indivisible operation may have exactly one sequence.

A Task with repeatable bounded items may have many sequences.

Sequence boundaries do not create a second scheduler. They return control to the existing Queue and
Governor.

## Durable checkpoint v1

The generic checkpoint codec remains version 1.

It stores only the bounded generic Task snapshot. Large scientific artifacts and Task-specific payloads
remain outside the checkpoint.

It never serializes:

- mutexes;
- condition variables;
- callbacks;
- userdata pointers;
- worker handles;
- Governor objects;
- reservations;
- CPU affinity masks;
- GPU device identity;
- scratch leases.

## Standalone checkpoint publication

For the standalone file publication primitive:

1. encode a bounded temporary file in the same directory;
2. synchronize file contents;
3. atomically rename/publish;
4. synchronize the parent directory.

If failure occurs before rename, the old published checkpoint remains unchanged.

If rename succeeded but the parent-directory sync fails, the result is:

```text
LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE
```

The new visible file is valid, but crash/power-loss durability of the directory entry is not claimed.

## Project checkpoint protocol

The project-integrated checkpoint slot uses:

```text
.lardon3d/checkpoints/<task_id>.chk
.lardon3d/checkpoints/<task_id>.chk.next
.lardon3d/checkpoints/<task_id>.chk.lock
```

The advisory `.chk.lock` serializes the writer and recovery logic for the fixed staged slot.

The lock is process synchronization only. Kernel release after process death is not durable recovery
state.

The project publication order is:

```text
capture durable Task snapshot
-> publish validated .chk.next
-> commit SQLite Task summary + checkpoint reference
-> promote .chk.next to canonical .chk under .chk.lock
-> synchronize the checkpoint directory
```

Filesystem and SQLite are not claimed to be one distributed transaction.

If SQLite fails after a staged file was published, that file may remain an orphan while Project DB
retains the previous logical truth.

If SQLite commits and staged promotion later fails or has uncertain directory durability, `.chk.next`
remains a potential recovery representation.

## Project recovery

Recovery acquires `.chk.lock` and reloads the SQLite record after lock acquisition because the
discovery page may have become stale while waiting.

It validates:

- checkpoint codec;
- checkpoint version;
- bounded fields;
- Project DB summary agreement.

The DB comparison covers only fields actually stored in the generic `tasks` summary, including:

- `task_id`;
- name;
- saved state;
- recovery state;
- progress;
- sequence count.

The DB does not duplicate the full durable estimate or all checkpoint timestamps, so recovery does not
pretend to compare fields that are not stored there.

A valid canonical `.chk` whose stored summary matches Project DB has priority.

If canonical state is unusable, a valid matching `.chk.next` may be promoted under the same lock and
used.

A historical project that legitimately contains only canonical `.chk` remains recoverable.

Missing, corrupt, future-version or summary-divergent checkpoint state makes that Task
non-recoverable without corrupting unrelated Tasks.

## Task Kind Registry boundary

Generic Task recovery does not guess business logic.

A durable Task record is classified through the production Task Kind Registry.

Typed recovery distinguishes at least:

```text
LEGACY_UNTYPED
UNKNOWN_TASK_KIND
UNSUPPORTED_TASK_KIND_VERSION
```

A business-specific reconstruct function is selected only by exact supported kind/version.

No persisted callback or code address exists.

The current production registry contains:

```text
CURRENT_PRODUCTION_TASK_KINDS=16
```

including the additive:

```text
raw.develop.batch/1
features.extract.batch/1
```

## Queue boundary

Every production Task passes through the single existing Task Queue, including Tasks whose resource
contract is completely fixed.

The Queue owns stable execution-order behavior.

It does not invent Task resource budgets.

A pending Task is executed only after the Resource Governor returns an executable admitted contract and
a reservation is successfully installed.

The current architecture keeps one active heavy Queue callback.

Bounded internal participants inside that callback are not additional Queue workers.

## Resource Governor boundary

```text
COMPUTE_GOVERNOR_V2=PASS/FROZEN
```

The Resource Governor owns:

- RAM admission;
- CPU admission;
- IO admission;
- GPU admission;
- host reserve;
- pressure response;
- capability selection;
- transient reservations;
- bounded adaptive feedback.

The Task owns its declared workload and scientific work.

Resource policy never changes scientific input identity merely to fit a machine.

## Private capability envelope

A live Task has a bounded private capability envelope.

By default, a Task with no alternatives has one capability equal to its honest fixed contract.

Kinds with validated alternatives may expose multiple legal capabilities.

The capability envelope is not:

- a new public Task Kind descriptor;
- a Project DB payload;
- a generic checkpoint field;
- a scientific fingerprint;
- a second scheduler.

Queue admission and `sequence_break()` ask the Governor to select from the legal envelope using one
current resource snapshot.

The chosen sequence contract is installed once and stays unchanged until release.

## Canonical resource policy

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

The interactive host reserve and all safety constraints are preserved first.

Inside the remaining safe envelope, Lardon3D should use the maximum validated useful resources.

Leaving safe useful CPU/GPU capacity idle solely because an older Task was once measured at a smaller
width is not a stability policy.

Reference-host values remain observations, not portable constants.

## Adaptation dimensions

The Governor may adapt only dimensions explicitly exposed by the Task's validated capability contract.

Typical dimensions include:

- CPU participants;
- batch/window size;
- ORB Vulkan inflight depth where that private seam exists.

A selected contract is never mutated during execution.

### Independent-dimension trials

Where dimensions are independently meaningful, adaptation should isolate them so measured throughput
changes can be attributed correctly.

The historical generic rule of trying one dimension at a time remains appropriate for such Tasks.

### Coupled CPU/batch trials

CPU and batch are **not universally required to be independent**.

For a Task whose extra CPU participants cannot perform useful work while batch/window remains one,
testing `CPU2/batch1` does not measure actual scaling.

A Task-specific validated capability may therefore expose coupled CPU/batch rungs.

Current examples include:

```text
candidate_pair.generate/1
features.extract.batch/1
```

where CPU and admitted item window may advance together so every tested rung exercises independent
work.

This is explicit Task-specific policy. It does not mean all Tasks couple CPU and batch.

```text
CPU_AND_BATCH_MAY_COUPLE_WHEN_TASK_CONTRACT_REQUIRES=YES
GLOBAL_CPU_BATCH_COUPLING_RULE=NO
```

This correction preserves scientific identity because admitted width is operational state.

## Per-item atomicity versus cross-item concurrency

A scientific item can remain atomic while independent items execute concurrently.

Examples include:

- selected RAW Captures;
- selected Feature images;
- Candidate source memberships;
- Match Results prepared by Geometric Verifier.

Therefore:

```text
PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO
```

Owner-only publication can preserve deterministic durable order after bounded participants join.

## Feedback loop

The private adaptation loop is conceptually:

```text
observe
-> choose
-> reserve
-> execute
-> publish/checkpoint
-> measure durable useful work
-> adapt a later sequence
```

Only successfully completed durable work counts as useful throughput where the Task contract requires
such feedback.

Uncertain or non-durable publication must not be counted as successful work for resource scaling.

Current feedback is bounded process-local operational state and is not persisted as scientific
history.

## Host telemetry boundary

Current private host observation may include:

- compute-pool CPU use;
- `MemAvailable`;
- CPU/memory/IO PSI;
- active swap deltas;
- selected GPU busy signal;
- process RSS/HWM diagnostics.

RSS/HWM is process observation, not a Task reservation.

Missing optional telemetry becomes unknown rather than fabricated pressure.

The Resource Governor owns interpretation of those signals.

Task code does not directly lower scientific quality based on telemetry.

## Pressure behavior

A resource contract already installed for a healthy bounded sequence is not asynchronously shrunk
because a new pressure sample arrives.

Pressure affects later admission.

Where policy still permits execution under pressure, the Governor may return the Task to its minimum
safe capability before reservation.

After pressure recovery, useful width may ramp again according to the validated per-kind adaptation
contract.

## Matcher ORB AUTO boundary

ORB Matcher has a validated GPU-first operational path.

For normal AUTO operation:

```text
backend selection       Governor-owned AUTO
preferred eligible path Vulkan
fallback                complete CPU recomputation
normal inflight depth   1
validated safety depth  2
normal useful batch     up to 8
```

Depth 2 was measured safe but did not meet the accepted useful-throughput deadband for normal AUTO
policy.

Matcher batch 12 likewise remains a safe/private validation capability where documented, while the
normal useful AUTO ceiling remains 8.

These choices do not change:

- Matcher Task Kind/version;
- Match Result identity;
- Match File bytes;
- Matcher scientific fingerprint;
- durable cursor.

A backend failure never publishes a partial Vulkan result.

## Feature execution

### Historical single-image paths

The following single-image scientific Tasks remain valid:

```text
features.extract/1
features.extract.sift/1
features.extract.rootsift/1
```

One image remains the atomic Feature result.

Non-interruptible OpenCV extraction restarts that image after a crash rather than claiming intra-image
resume.

### Current selected Feature batch

Project DB v25 adds:

```text
features.extract.batch/1
```

One durable owner handles an immutable selected execution.

Bounded participants prepare independent images, join, and the owner publishes/reuses READY Feature
Sets in selected-item order before advancing the typed cursor and generic Task checkpoint.

Cross-image preparation may run concurrently while per-image Feature publication remains atomic.

## RAW selected batch

Project DB v24 adds:

```text
raw.develop.batch/1
```

Independent selected RAW items may be prepared in a bounded admitted window.

The owner publishes the canonical selected prefix and advances the selected-execution cursor.

The historical `raw.develop/1` path remains valid.

## Candidate Pair Task

`candidate_pair.generate/1` processes bounded Visual Index source work and publishes canonical
Candidate Pairs.

Its current validated resource model includes a batch range up to 64 and coupled CPU/batch adaptation
where required to exercise participants.

Participants perform read/preparation work; owner publication remains deterministic.

A restart loads `after_feature_set_id` and reuses already persisted Candidate Pairs idempotently.

## Visual Index Task

`visual_index.update/1` processes bounded Feature Set input and publishes immutable deterministic
segments.

It checkpoints only after the segment publication boundary required by its persistence contract.

A segment with uncertain publication durability is not counted as successful adaptive work.

## Geometric Verifier Task

`geometric_verifier.run/1` uses outer cross-item concurrency while preserving the validated
per-item scientific solver configuration.

The current validated execution shape supports:

- up to 8 useful participants;
- up to 16 safe participants/window entries;
- owner-only canonical publication.

Project DB v13 owns its typed Task cursor.

The internal USAC/MAGSAC scientific solver remains in the validated non-parallel inner configuration.

## Track Builder

`track_builder.run/1` is a durable full-rebuild Task over one immutable GVR scope.

Restart rebuilds from the explicit scope and publishes/reuses the exact Track Set according to the
Track Builder contract.

No hidden dependency graph is persisted by the Task System.

## Acquisition campaign

`acquisition_campaign.run/1` is a durable Task over an immutable Project DB v20 request.

One S3-E group is materialized per sequence.

The Task-specific Capture mapping and cursor become durable before generic progress/checkpoint.

Restart uses retained explicit Capture identity rather than inferring from paths, hashes or image IDs.

## Photo Quality Triage

`photo_quality.triage/1` reuses the same Task/Queue/Governor machinery.

Its canonical one-based `next_group_id` is Task-specific durable state.

Result and cursor publication precede generic checkpoint advancement.

The Task System does not reinterpret already published quality identity from generic percentage.

## Import Task

The production import path no longer owns a private execution thread or private cancellation flag.

Its TUI wrapper enqueues, cancels and observes the generic Task.

Each callback executes bounded admitted work, publishes its Task-specific durable state, checkpoints and
uses a sequence break when more work remains.

## Sparse SfM

`sparse_sfm.run/1` is **PASS/FROZEN**.

Its historical fixed CPU1/batch1 execution contract remains valid for that frozen scientific Task.

This is not a global Task System rule.

Sparse SfM Gates C through G are implemented. Historical real S21/A6000 campaigns have not executed
real known-calibration Sparse SfM because known calibration remains unavailable for those campaigns.

## Incremental reconstruction

`incremental_reconstruction.run/1` is **PASS/FROZEN**.

It recomputes its atomic Phase H result from immutable predecessor/extension/calibration inputs after
restart.

No transient solver state is persisted in the generic checkpoint.

## Terminal callback ownership

An optional terminal callback is notified exactly once for:

```text
COMPLETED
FAILED
CANCELLED
```

It is not a notification for pause or sequence break.

Terminal state is committed under the Task mutex.

The terminal reservation is released before the callback is invoked outside the Task mutex.

`join()` waits for terminal callback completion.

Userdata therefore remains valid for the callback and is destroyed only afterward by Task destruction.

## Reconstructed Task rejected before Queue transfer

A reconstructed Task that is rejected locally before ownership transfers to Queue is destroyed locally.

Its userdata destructor runs.

No terminal callback is fabricated and no false durable cancellation is written.

An explicit user-requested cancellation remains a real lifecycle transition and keeps its terminal
notification semantics.

## TUI observation boundary

The TUI observes copied Task/Queue/Governor state.

It does not retain Task userdata and does not drive hidden lifecycle transitions.

The installed execution contract on the active Task is the authoritative Task-specific CPU/batch
contract for that sequence.

A Governor diagnostic keyed only by Task Kind cannot be attributed to one specific Task unless the
observation also establishes the Task/sequence association.

## Current Project DB

```text
CURRENT_PROJECT_DB_SCHEMA=v25
```

The current additive chain includes:

```text
v22  selected scientific execution foundation
v23  generic optical-context overlay
v24  raw.develop.batch/1 persistence
v25  features.extract.batch/1 persistence
```

These schema additions do not change generic checkpoint codec v1.

Project DB stores logical Task summaries and typed Task-specific payloads. It does not store live
reservations, callbacks, affinity masks or Governor feedback history.

## Current real checkpoint

The retained current upstream real checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The run exercised current Task/Queue/Governor/restart behavior through:

- selected RAW batch;
- selected Feature batch;
- Visual Index;
- Candidate Pair;
- Matcher;
- Geometric Verifier v3;
- Track Builder.

The final continuation retained zero Sparse SfM Tasks/reconstructions and performed no Dense/MVS work.

## Limits

Current Task System limits/non-goals include:

- one active heavy Queue callback;
- no generic Task dependency DAG;
- no internal priority system beyond current Queue policy;
- no generic artifact-reference graph in checkpoint v1;
- no generic autosave timer;
- no second scheduler;
- no generic preemption of third-party atomic calls;
- no persistence of Resource Governor feedback;
- no authoritative scratch-consuming Task Kind at the current checkpoint.

Task-specific code owns the durable scientific/business cursor and must publish that state before the
generic checkpoint is allowed to claim corresponding progress.

## Summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

TASK_SYSTEM_STATUS=IMPLEMENTED
GENERIC_CHECKPOINT_VERSION=1

TASK_QUEUE_ACTIVE_HEAVY_CALLBACKS=1
GENERIC_TASK_DAG=NOT_IMPLEMENTED
GENERIC_AUTOSAVE_TIMER=NOT_PRESENT

CPU_AND_BATCH_MAY_COUPLE_WHEN_TASK_CONTRACT_REQUIRES=YES
GLOBAL_CPU_BATCH_COUPLING_RULE=NO

PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO

CURRENT_SCRATCH_CONSUMING_TASK_KINDS=0

COMPUTE_GOVERNOR_V2=PASS/FROZEN
ORB_VULKAN_ASYNC_EXECUTION=PASS/FROZEN

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```
