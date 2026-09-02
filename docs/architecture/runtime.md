# Lardon3D Runtime

## Status

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

TASK_QUEUE_WORKERS=1
INTERNAL_PARALLELISM=BOUNDED
INTER_TASK_PARALLELISM=NOT_IMPLEMENTED

RUNTIME_OBSERVER=CURRENT/VALIDATED_OPERATIONAL
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```

The runtime coordinates the main thread, Task Queue, Resource Governor, Project DB lifecycle, bounded
Task-internal participants, runtime observation and optional external-SSD controller.

It does not introduce a second scheduler.

## Thread model

### Main thread

Owns:

- input;
- ncurses;
- TUI model binding;
- project open/close orchestration;
- bounded polling of runtime/SSD state.

ncurses remains main-thread-only.

### Task Queue worker

The single Queue worker owns one active heavy Task callback at a time.

A Task callback may create bounded internal participants only when its admitted Task contract permits
them.

Those participants join before owner publication.

### SSD operation thread

The SSD controller may own at most one bounded joinable operation thread while executing a synchronous
UDisks operation.

It does not become:

- a Task worker;
- a Queue;
- a scheduler;
- an ncurses owner.

## Task lifecycle

Conceptual lifecycle:

```text
create PENDING
-> persist typed intent where required
-> enqueue
-> Queue selects
-> Resource Governor admits/reserves
-> RUNNING
-> bounded sequence work
-> Task-specific durable publication
-> generic checkpoint
-> optional sequence_break/re-admission
-> COMPLETED | FAILED | CANCELLED
-> terminal callback
-> destruction
```

Pause/cancel are cooperative.

A sequence break is not a Task state.

## Durable restart

A generic snapshot stores logical Task state, not live execution machinery.

On restoration:

```text
RUNNING -> PENDING
PAUSED  -> PENDING
```

Terminal states remain terminal.

Restart never restores:

- worker thread;
- callback pointer;
- userdata pointer;
- CPU affinity;
- live reservation;
- GPU handle;
- scratch lease;
- adaptive feedback history.

Task Kind Registry reconstructs fresh runtime binding from exact durable kind/version and typed payload.

Every resumed Task is re-admitted by the Resource Governor.

## Project-open recovery

`project_open()` discovers durable Tasks in bounded pages, validates their checkpoint/typed identity,
reconstructs eligible bindings and submits them to the existing Queue.

It returns after enqueue; it does not wait for those Tasks to finish.

Unknown kinds, unsupported versions, legacy-untyped Tasks, invalid checkpoints or Task-specific
non-reconstructible input remain inspectable and do not cause guessed execution.

Generic dependency/DAG recovery remains unimplemented.

## Initialization order

Production startup establishes the safe driver/runtime policy before heavy worker/backend activity.

Conceptually:

```text
driver policy
-> hardware profile
-> Resource Governor
-> optional backend metadata
-> Task Queue / worker
-> optional SSD controller + Governor binding
-> TUI
```

Project open/recovery is then driven from the main thread.

The exact source initialization sequence remains authoritative.

## Project lifetime boundary

Changing/closing project is an exact ownership boundary.

Before Project DB close:

```text
views release DB borrows
-> Queue ingress closes
-> Queue cancels/joins/destroys
-> terminal callbacks finish
-> Project DB closes
```

A fresh empty Queue can then be created for the next project.

No terminal callback may dereference a closed Project DB.

Queue terminal history belongs to the current runtime session and does not leak between projects.

## Project Database

Project DB uses an opaque serialized SQLite connection with bounded transactional operations.

Current schema head:

```text
CURRENT_PROJECT_DB_SCHEMA=v25
```

Current additive selected-execution overlays include:

```text
v24 raw.develop.batch/1
v25 features.extract.batch/1
```

The v23 optical model remains valid but is no longer the schema head.

## Current production Task inventory

Production currently has sixteen Task Kinds.

Important selected-execution additions:

```text
raw.develop.batch/1
features.extract.batch/1
```

The Queue still has one active heavy callback; batch Task Kinds obtain throughput from bounded
participants inside that callback.

Per-item atomicity does not require cross-item serial execution.

## Resource Governor

Every production Task goes through the Resource Governor, including fixed-resource Tasks.

The canonical host policy is:

```text
preserve defined interactive reserve
then maximize safe useful throughput
```

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

Reference-host CPU counts are evidence, not portable constants.

Pressure may reduce a later admission; healthy recovery may ramp useful width again.

Swap/zram/scratch do not enlarge admitted RAM.

UMA GPU memory is charged once against host RAM.

## Internal parallelism

Validated Task Kinds may execute:

```text
one Queue owner callback
-> bounded participants
-> join
-> deterministic owner publication
```

Current examples include:

- Feature selected batch;
- Visual Index;
- Candidate Pair;
- Matcher CPU work;
- Geometric Verifier outer parallel preparation;
- RAW selected batch.

This is not inter-Task parallelism and does not create another global worker pool.

## Current Matcher backend policy

ORB Matcher normal production is Governor-owned AUTO.

Eligible ORB work prefers the validated Vulkan backend.

Fallback is complete CPU recomputation.

Normal Vulkan contract:

```text
inflight = 1
helpers = 0
useful batch <= 8
```

Depth 2 remains validated private safety/benchmark capacity but was rejected as normal useful policy.

SIFT/RootSIFT Matcher remains CPU.

## Runtime observation

The runtime observer borrows Queue/Governor state and publishes one bounded coherent snapshot for the
TUI.

It does not retain Task userdata.

Ordinary snapshots are rate-limited/coalesced.

On observation failure, the previous bounded view may be retained and explicitly marked stale.

Task observation includes live/pending/recent-terminal entries only within fixed capacity.

No unbounded Project DB scan is performed per frame.

## Durable progress and ETA

Typed Tasks publish exact `completed/total` only after their Task-specific durable prefix is committed.

The TUI must not infer exact scientific progress from:

- Task name;
- message text;
- generic percentage.

When exact counters exist, they are authoritative.

Throughput/ETA needs enough positive-time progress observations.

No-progress/pressure/restart cases become explicit states such as stalled, throttled or indeterminate
rather than fabricated precision.

A terminal Task with inconsistent durable progress is visible as an integrity problem rather than
silently forced to 100%.

## Pipeline observation

Current observable stages include:

```text
Acquisition
RAW
Quality
Features
Visual Index
Candidate
Matcher
GV
Tracks
Sparse SfM
Dense
```

Sparse SfM capability exists.

Dense has no production Task Kind and remains not applicable/unimplemented at the current checkpoint.

Historical S21/A6000 real campaigns have not executed Sparse SfM because known calibration is
unavailable for those campaigns.

## TUI resource observation

Resource UI may display known bounded values for:

- active/admitted/available CPU;
- GPU presence/backend/busy/memory;
- RAM and `MemAvailable`;
- host reserve;
- swap state and active deltas;
- batch/inflight/helpers;
- IO;
- scratch;
- Governor pressure.

The installed contract of the exact active Task is authoritative for that sequence.

A diagnostic indexed only by Task Kind cannot automatically be attributed to another Task/sequence.

Unknown values remain unknown.

## TUI layout

The validated layout classes remain bounded.

Current thresholds include:

```text
full layout >= 100x30
compact boundary = 72x20
supported minimum = 60x15
```

Below the supported minimum, the UI uses its too-small-terminal fallback.

Repository documentation is English. Any remaining non-English executable UI literal is legacy runtime
text and must be changed only in the explicitly scoped UI-language remediation pass; documentation does
not redefine executable behavior by pretending that source literal has already changed.

## Navigation

The current TUI provides screens for the implemented runtime surfaces, including:

- home;
- projects;
- import;
- tasks;
- resources;
- optics;
- SSD;
- help;
- viewer placeholder/future surface.

Key bindings and exact executable labels remain owned by the TUI source and its tests.

Documentation should describe behavior rather than preserve stale localized literals as authority.

## Optical workflow

The TUI uses the public optical APIs introduced by the v23 overlay.

It does not write optical SQLite rows directly.

It supports explicit inspection/selection and immutable profile creation.

Manual lenses without electronic metadata are valid data.

No workflow may fabricate:

- "unknown" lens identity;
- calibration compatibility;
- metadata match;
- focal/lens substitution.

Calibration selection remains explicit and exact.

## SSD F10 boundary

The SSD UI reflects controller capability/state rather than inventing actions.

The controller owns physical detection, pairing, mount/swap/scratch state and bounded UDisks operations.

The Resource Governor owns scratch lease admission.

A state that is incomplete, stale or physically inconsistent grants no control/lease authority.

Scratch is storage capacity, never RAM.

## Global shutdown

Ownership shutdown preserves:

```text
Task Queue / Task leases
-> project close
-> SSD operation join / Governor unregister
-> SSD controller
-> Resource Governor
```

A real outstanding scratch lease can block unregister and must remain an observable error.

Do not abandon a live lease pointer.

## Error/recovery model

Runtime operations use local rollback and explicit publication boundaries.

File asset publication plus SQLite is not treated as one distributed transaction.

A successfully published physical asset followed by DB failure may leave a valid orphan.

Recovery validates known durable representations; it does not guess or silently repair scientific
identity.

## Current real checkpoint

Current retained A6000 checkpoint:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

It exercised current runtime/Queue/Governor behavior through:

```text
selected RAW batch
selected Feature batch
Visual Index
Candidate Pair
Matcher
Geometric Verifier v3
Track Builder
```

The final continuation recorded deterministic restart/reuse and stopped with:

```text
Sparse SfM Tasks       0
Sparse Reconstructions 0
Dense/MVS              0
```

This is a current real runtime checkpoint, later than the historical global-maintenance checkpoint.

Both remain valid for the boundaries they prove.

## Current limits

Current runtime intentionally does not provide:

- multiple concurrent heavy Queue callbacks;
- generic inter-Task DAG scheduling;
- generic Task priorities beyond current Queue policy;
- generic autosave ahead of Task-specific durable publication;
- Dense/MVS production Task Kind;
- live capture/viewer reconstruction loop;
- generic scratch-consuming Task Kind.

Those are future product/implementation decisions, not silently missing state.

## Summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

TASK_QUEUE_WORKERS=1
ACTIVE_HEAVY_CALLBACKS=1
INTER_TASK_PARALLELISM=NOT_IMPLEMENTED
INTERNAL_PARALLELISM=BOUNDED

GENERIC_DAG=NOT_IMPLEMENTED
DENSE_TASK_KIND=NOT_IMPLEMENTED
CURRENT_SCRATCH_CONSUMING_TASK_KINDS=0

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```
