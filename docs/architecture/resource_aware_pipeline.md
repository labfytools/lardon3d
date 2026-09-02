# Lardon3D Resource-Aware Pipeline

## Status

This document describes the current production resource model for the selected pre-SfM pipeline and
its resource-sensitive execution paths.

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The scientific contracts of Feature Store, Candidate Pair, Matcher, Geometric Verification and Track
Builder remain owned by their specialized documents. This document owns the operational view:
admission, bounded work, internal concurrency, publication boundaries, pressure response and
reference-host measurements.

## Canonical execution rule

A heavy production unit starts only while an active Resource Governor reservation authorizes it.

The normal sequence is:

```text
immutable Task payload
-> Governor admission
-> bounded preparation / computation
-> deterministic owner publication
-> durable cursor / checkpoint
-> release sequence-local buffers and reservation
-> next admission
```

The Queue keeps one active Task callback. That does not require a Task callback itself to perform all
independent work serially. Where a Task owns multiple independent units, bounded internal participants
may prepare those units concurrently when the scientific and persistence contracts permit it.

Canonical policy:

```text
MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF
```

Per-item atomicity does not imply cross-item serialization. Owner-only or ordered durable publication
does not imply serial preparation.

A long-running CPU1 or batch1 path is acceptable only when a concrete dependency, measured scaling
knee, RAM limit, I/O limit, validated GPU path or another documented operational constraint proves
that more concurrency would not be safely useful.

## Host reserve and pressure model

Lardon3D preserves the interactive host reserve first, then gives the active workload the safe and
useful remaining capacity.

On the current validation host, the normal observed outcome is approximately:

```text
16 logical CPUs total
4 logical CPUs reserved for interactive host use
12 logical CPUs available to the compute pool
~3 GiB MemAvailable preserved as the hard RAM reserve
Radeon 780M UMA available to validated and useful GPU backends
```

These values are evidence for the current host, not portable product constants.

The Governor may use `MemAvailable`, CPU pressure, memory PSI, I/O PSI, active `pswpin` / `pswpout`
deltas, selected-GPU state, and Task-declared fixed, per-participant and transient costs.

Swap, zram and external scratch never enlarge admitted RAM. UMA GPU allocations are charged exactly
once against host RAM.

Pressure may reduce admission. When pressure clears, safe and useful capacity must be re-admitted
rather than leaving the process permanently throttled.

## Queue and sequence boundaries

The Task Queue remains bounded and has one active callback.

When pending work exists but all candidates receive `WAIT`, the Queue worker performs the validated
bounded wait and retries normal admission with a fresh snapshot. A running sequential Task may also
cross `sequence_break`, which releases the current reservation and requires fresh admission for the
next sequence.

The current architecture separates:

```text
cross-Task dispatch          -> one active Queue callback
inside-Task independent work -> bounded participants when justified
durable publication          -> owner-only where required
```

No second scheduler, uncontrolled worker pool, detached-thread system or parallel persistence
subsystem is introduced.

## Current Task inventory

The production registry contains 16 Task kinds.

The two additive kinds beyond the historical fourteen-kind maintenance inventory are:

```text
raw.develop.batch/1
features.extract.batch/1
```

Historical documents that correctly recorded fourteen kinds at their checkpoint remain historical
evidence. Fourteen is not the current production count.

## Feature Extraction

### Scientific atomicity

A Feature Set remains an immutable per-image scientific result.

The historical production path remains valid:

```text
features.extract/1
one image
-> feature preparation
-> Feature File publication
-> Project DB metadata
-> terminal Task checkpoint
```

That path remains important for legacy Tasks and exact restart compatibility.

### Current selected-execution batch path

Project DB v25 adds the current operational path:

```text
features.extract.batch/1
```

The durable owner is bound to one immutable selected execution, the exact ORB domain and a monotone
`next_item_index`. The selected item order remains authoritative.

The batch path is:

```text
one admitted owner Task
-> bounded independent selected-image participants
-> each participant prepares one per-image Feature result without SQLite access
-> join all admitted participants
-> owner validates / reuses the exact READY Feature Set
-> owner publishes in selected-item order
-> owner advances the durable Feature cursor
-> generic Task checkpoint follows
```

A crash may leave the generic checkpoint or Feature-batch cursor behind an already immutable Feature
Set. Restart revalidates and reuses that exact result; it does not infer a new identity.

Per-image Feature atomicity is therefore preserved while cross-image preparation may be concurrent.

### Feature CPU control

OpenCV thread count is process-wide state, so it must remain controlled.

The runtime establishes the validated baseline from the available compute pool before Queue
execution. The active heavy callback temporarily applies the admitted count required by its current
contract and restores the baseline on every exit path.

The Queue still has one active callback, so unrelated Tasks do not race process-wide OpenCV thread
configuration.

For `features.extract.batch/1`, cross-image participants are the concurrency mechanism. Additional
internal OpenCV threads inside every participant are not multiplied blindly.

The current coupled Feature-batch admission exists because increasing CPU while the admitted item
window remains one cannot exercise additional independent images. CPU and batch may therefore move
together for this Task where the validated rung contract requires it.

This is an explicit operational exception, not a universal Governor rule.

## RAW selected-execution batch

Project DB v24 provides `raw.develop.batch/1`.

Its pattern is analogous at the execution boundary:

```text
one admitted owner Task
-> bounded independent RAW participants
-> join
-> deterministic owner-only publication by selected_item_index
-> durable selected-execution cursor advancement
```

Per-Capture RAW publication remains atomic. Cross-Capture preparation need not be serial.

The retained real A6000 proof completed all 689 selected RAW representations through this path.

## Visual Index

Visual Index remains a bounded CPU path.

Its scientific identity and segment publication are unchanged. Internal parallelism may be used only
within its validated contract; no GPU path is promoted merely to make the accelerator busy.

The current GPU audit rejected Visual Index as a useful production GPU candidate. That rejection is a
measured backend decision, not permission to leave useful CPU capacity idle.

## Candidate Pair generation

Candidate Pair generation processes bounded Visual Index input and publishes deterministic canonical
pairs.

Current resource behavior distinguishes per-pair scientific identity from cross-item execution.

The validated Candidate path may use coupled CPU/batch rungs because additional CPU cannot exercise
additional independent source work while the admitted item window remains one.

The current operational model therefore permits:

```text
bounded source/item window
+ bounded CPU participants
-> deterministic Candidate preparation
-> owner publication
```

The exact current memory model and batch limits are owned by [`candidate_pair.md`](candidate_pair.md).
A historical CPU1/batch1 descriptor or older window estimate is not a permanent product ceiling.

## Matcher

`matcher.run` v1 is durable. Its scientific atomic unit is one Candidate Pair.

The normal Task pages Project DB by `candidate_pair_id`; it does not assume contiguous IDs and does
not retain the whole Candidate Pair set in memory.

The durable Matcher sequence is:

```text
bounded Candidate Pair page
-> match one or more admitted pairs
-> publish each exact Match Result
-> advance durable cursor
-> checkpoint
-> sequence_break
```

The current adaptive batch rungs are:

```text
1 -> 2 -> 4 -> 8
```

A published Match Result may precede its generic checkpoint after a crash. Restart finds and reuses
the exact durable Match Result rather than recomputing or inventing identity.

### ORB Vulkan

The validated ORB Matcher Vulkan backend is GPU-first when eligible.

The Radeon 780M is UMA, so backend memory is host RAM and is charged once.

The current production Vulkan contract retains:

```text
normal useful inflight depth = 1
validated private safety capacity = 2
helpers = 0
```

Depth 2 was measured and rejected as the normal useful setting because its improvement remained below
the accepted deadband. It remains a validated private capacity, not a production default.

The backend preserves canonical Match Result identity and exact CPU fallback. An ineligible pair or
backend failure is recomputed completely on CPU; no partial GPU result is published.

SIFT and RootSIFT remain CPU OpenCV L2 paths. Their Vulkan feasibility work did not reach production
eligibility and therefore receives no authoritative production GPU reservation.

## Geometric Verification

The current production verifier lineage is Geometric Verifier v3.

One Match Result remains the scientific atomic input.

The bounded execution shape is:

```text
one admitted owner Task
-> bounded independent GVR preparation
-> join
-> owner publishes the canonical prefix in order
-> durable cursor / checkpoint
```

The validated path may use up to eight useful participants and sixteen safe participants, with
admitted windows up to sixteen Match Results.

The internal USAC/MAGSAC scientific solver keeps its validated `isParallel=false` behavior. Cross-item
parallelism belongs outside that per-item solver and does not modify scientific thresholds or identity.

The retained real A6000 continuation completed:

```text
Match Results        38,420
Applicable GVRs      37,805
Verified GVRs        10,952
Rejected GVRs        26,853
```

with deterministic restart and no duplicate mappings.

## Track Builder

Track Builder consumes an immutable GVR scope.

Its compact current memory model supersedes the historical rejected S21 envelope that attempted to
reserve approximately 18.204 GiB.

The retained real S21 proof completed:

```text
Tracks              912,447
Track observations  2,495,768
```

The retained real A6000 proof completed:

```text
Tracks              130,714
Track observations  318,944
```

Both proofs preserve deterministic restart semantics and no authoritative scratch consumption.

Track Builder resource details remain owned by [`track_builder.md`](track_builder.md).

## GPU policy

GPU use is capability- and evidence-driven.

Canonical rule:

```text
validated AND useful backend -> preferred when eligible and Governor-safe
unvalidated backend          -> never promoted for utilization appearance
measured non-useful backend  -> may remain CPU
```

Current examples:

```text
ORB Matcher Vulkan        validated and preferred
SIFT / RootSIFT Matcher   CPU
Candidate                 CPU
Feature                   CPU
Visual Index              CPU
Geometric Verification    CPU
```

This inventory may evolve only through measured, validated backend work.

## External SSD and scratch

The optional external SSD controller is a physical-lifecycle boundary using the reviewed
UDisks2/GDBus contract.

Its state is registered with the Resource Governor. The Governor is the sole production orchestrator
for scratch leases.

At the current checkpoint:

```text
16 production Task kinds
0 authoritative scratch-consuming Task kinds
```

Scratch availability is therefore a capability, not fabricated usage.

A future Task may use scratch only after an explicit Task-specific contract defines eligibility,
lease lifetime, path ownership, cleanup, cancellation, failure handling, capacity accounting and
restart behavior.

Scratch and swap never become RAM.

## Reference-host measurements

The following measurements are evidence, not portable constants.

### CPU reserve

On the Ryzen 7 8845HS reference host:

```text
16 logical CPUs total
~4 logical CPUs reserved for interactive use
~12 logical CPUs available to Lardon3D compute
```

Historical isolated Matcher measurements observed only a small additional gain from 12 to 16 logical
threads. That observation supports the reference-host reserve; it does not create a global 12-thread
product ceiling.

### Geometric Verifier historical resource-aware run

A retained resource-aware GV run traversed approximately 2001 reused parents in about 5.870 seconds
through Task, Project DB, checkpoints and Governor.

The test process peaked around 25,964 KiB RSS, `MemAvailable` remained above approximately 10.69 GiB
and swap deltas remained zero.

This measurement validates the bounded path and restart behavior. It is not a universal latency
model.

### A6000 current proof

The later A6000 proof recorded 2,714 Governor admissions while continuing Match Result -> GV -> Tracks.

Its final GV admitted window was 16, with the reference-host compute pool at 12 logical CPUs and four
reserved for host use. Swap-in/out deltas remained zero.

The checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Sparse SfM and Dense/MVS were not executed by this proof.

## Recovery and deterministic publication

Parallel preparation never weakens publication ordering.

For every path where durable order matters:

```text
prepare independent work
-> join
-> validate exact immutable result
-> owner-only deterministic publication
-> durable typed cursor
-> generic Task checkpoint
```

A checkpoint may lag an already published immutable scientific result. Restart must reuse or validate
that result through its exact scientific identity; it must not fabricate a replacement identity.

Cancellation is cooperative at the Task's documented boundaries. All admitted participants are
bounded and joined before sequence completion or owner cleanup.

## Limits and deferred work

The following remain outside this document's current production contract:

- cross-Task worker pools;
- multiple simultaneous active Queue callbacks;
- general DAG scheduling;
- multi-GPU scheduling;
- cgroup/systemd constrained-runtime capacity accounting;
- generic resource residency/cache management;
- authoritative Task scratch consumers;
- unvalidated GPU ports;
- dense/MVS resource orchestration.

Deferring cross-Task parallelism is not permission to serialize independent units inside one active
Task.

The Queue/Governor plus bounded internal participants are the current production architecture.

## Current summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

raw.develop.batch/1          CURRENT
features.extract.batch/1     CURRENT

PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

VALIDATED_GPU_BACKENDS_PREFERRED_WHEN_USEFUL=YES
UMA_ACCOUNTED_ONCE_AGAINST_HOST_RAM=YES
SWAP_ZRAM_SCRATCH_AS_RAM=FORBIDDEN

REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
SPARSE_SFM_EXECUTED_IN_A6000_PROOF=NO
DENSE_MVS_EXECUTED_IN_A6000_PROOF=NO
```
