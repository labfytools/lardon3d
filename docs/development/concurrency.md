# Concurrency

## Status

```text
NCURSES_OWNER=MAIN_THREAD_ONLY
ACTIVE_HEAVY_QUEUE_CALLBACKS=1
TASK_CANCELLATION=COOPERATIVE
INTERNAL_PARALLELISM=BOUNDED
OWNER_ONLY_PUBLICATION=CANONICAL_WHERE_REQUIRED

TSAN_PORTABLE_PROJECT_MATRIX=QUALIFIED_PASS
TSAN_EXTERNAL_OPENCV_TBB=QUALIFIED
VULKAN_CONCURRENCY_VALIDATION=SEPARATE

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

Lardon3D separates UI ownership from heavy processing.

The main thread owns ncurses. The Task Queue owns one active heavy callback.
Individual validated Task Kinds may create bounded internal participants inside
that callback.

An SSD controller operation may also use at most one bounded joinable operation
thread under its own ownership contract.

## Execution model

```text
main thread
  input
  ncurses
  TUI orchestration

Task Queue worker
  one active heavy callback
  admitted Task sequence
  optional bounded internal participants
  deterministic owner publication

SSD operation thread
  zero or one bounded joinable controller operation
  no ncurses
  no Task callback
```

Internal participants are not a second global scheduler or Queue.

## Fundamental rules

### ncurses ownership

Only the main thread calls ncurses.

Workers publish observable state through protected data. They never call
`mvprintw`, `wrefresh`, or other ncurses APIs.

### Shared mutable state

Shared mutable state must have an explicit synchronization owner:

```text
mutex
condition variable
atomic primitive where the contract explicitly permits it
single-thread ownership
immutable-after-publication
```

Do not rely on timing or "normally only one caller".

### Condition variables

Always test the predicate in a loop around `pthread_cond_wait()`.

A signal is not durable state; the protected predicate is.

### Cooperative cancellation

Production Task cancellation is cooperative.

Do not use `pthread_cancel()` to stop a Task.

Task-specific non-preemptible operations finish their current atomic boundary
before pause/cancel is observed.

### Reservation before callback

No Task callback runs without the Resource Governor admission/reservation
required by its installed sequence contract.

Fixed-resource Tasks do not bypass the Governor.

### Terminal lifetime

Terminal callback completion precedes destruction of Task userdata.

Queue/Task ownership must ensure no observer dereferences freed userdata.

## Lock ordering

When multiple locks are required, the owning subsystem must define and preserve
one order.

Never add a reverse-order path to solve a local problem.

Avoid holding one subsystem mutex while calling into another subsystem that may
call back.

Where practical:

```text
copy bounded state under lock
release lock
perform I/O / expensive work
reacquire only for publication
```

## Queue ingress lifetime

The Queue owner closes ingress before destruction.

Shutdown waits for:

- active worker completion;
- registered in-flight API calls covered by the ownership contract;
- terminal callbacks.

This cannot make a raw C pointer safe if a caller begins a new call after the
object has already been freed. Callers must obey lifetime ownership.

## Bounded internal parallelism

A validated Task Kind may use internal participants while the Queue callback
remains the sole Task owner.

Required shape:

```text
one admitted Task owner
-> bounded participant count
-> bounded private work
-> join all participants
-> owner-only deterministic publication when required
-> Task-specific durable cursor
-> generic checkpoint
-> sequence_break
```

Participant count and memory must fit the admitted Resource Governor contract.

No participant may silently exceed the installed sequence contract.

## Atomicity does not imply serialism

Per-item scientific atomicity and cross-item execution width are separate.

```text
PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO
```

Current examples include selected RAW, selected Feature extraction, Candidate
Pair source work and outer Geometric Verification preparation.

Serialization is valid only where the subsystem's scientific, persistence,
library or measured-throughput contract proves it necessary.

## CPU/batch coupling

CPU and batch/window are not globally independent dimensions.

For a Task whose additional participants cannot do useful work while the
admitted item window remains one, a Task-specific capability may couple those
dimensions.

Current validated examples include:

```text
candidate_pair.generate/1
features.extract.batch/1
```

This is not a universal rule for all Task Kinds.

## Project lifetime boundary

Before closing a project:

```text
views release Project DB borrows
-> Queue is cancelled/joined/destroyed
-> Project DB closes
-> fresh empty Queue may be created for the next project
```

A terminal callback must never observe a Project DB already destroyed.

Project-specific runtime history must not leak into the next project.

## Global shutdown boundary

The current shutdown order preserves ownership across:

```text
Task Queue and Task leases
-> project close
-> join/unregister SSD binding
-> SSD controller
-> Resource Governor
```

A scratch unregister blocked by a real outstanding lease is an observable
failure, not permission to abandon a live pointer.

## SSD lease ownership

A scratch lease belongs to the exact caller-owned lease object used for the
operation.

Its mutable fields are controlled under the SSD controller mutex.

The caller must not:

- copy a live lease;
- move a live lease;
- present the same lease object to two controllers;
- release through a different ownership path.

Production acquire/release uses the Resource Governor wrappers.

The Governor releases its own mutex before entering the controller, and the
controller does not callback into the Governor while holding its mutex.

Scratch remains storage capacity, never RAM admission.

## Visual Index readers

Visual Index query readers do not share mutable query state.

A query obtains a bounded segment snapshot under the Project DB boundary, then
performs hash/read/accumulation after release.

A concurrent update makes a new segment visible only at its canonical commit
boundary. An already running query continues with its retained snapshot.

## OpenCV process-wide state

OpenCV thread configuration is process-wide.

The active heavy Queue callback owns temporary mutation of that setting where a
Task contract requires it and restores the previous/baseline value on all exit
paths.

Internal participants must not independently race `cv::setNumThreads()`.

For Feature batch, cross-image participants are used while internal OpenCV
threading is controlled explicitly.

## Vulkan boundary

ORB Vulkan concurrency is validated separately from portable TSan.

The production AUTO contract currently uses:

```text
normal inflight depth = 1
private validated safety depth = 2
helpers = 0
```

Depth 2 is a private safety/benchmark capability and was rejected as the normal
useful setting by measured throughput.

A Vulkan backend failure produces complete CPU fallback before publication.
Partial GPU scientific output is never published.

## TSan policy

For project concurrency changes, use a dedicated TSan build that matches the
supported proof boundary.

The retained global maintenance matrix used:

```text
GCC/G++
Vulkan disabled
selected concurrent targets
deterministic repetitions
```

and completed the retained 14/14 target matrix plus 220 repetitions.

The narrow suppression file is:

```text
tests/tsan-opencv.supp
```

It covers external non-instrumented OpenCV/TBB objects only.

It must not suppress Lardon3D frames.

Therefore the correct retained claim is not "TSan proves all concurrency".
It is:

```text
portable project concurrency matrix passed under the documented qualification
external OpenCV/TBB reports are narrowly qualified
Vulkan concurrency has a separate validation boundary
```

## What TSan does not prove

TSan is useful for instrumented conflicting memory access and some
synchronization misuse.

It does not prove absence of:

- deadlock;
- lost wakeup caused by incorrect predicate design;
- lifetime bugs outside the exercised paths;
- races hidden inside non-instrumented external libraries;
- Vulkan driver/runtime correctness;
- scientific determinism.

Lock-order review, ownership reasoning and deterministic tests remain required.

## Sanitizer command policy

Do not encode fixed `-j8` as canonical validation.

Example configuration:

```sh
CC=gcc CXX=g++ meson setup build-tsan -Db_sanitize=thread -Db_lundef=false -Dvulkan_orb=disabled
meson compile -C build-tsan
meson test -C build-tsan --print-errorlogs
```

Use host-aware compile/test parallelism unless the proof itself requires
serialization.

Do not repeatedly wipe an unchanged TSan tree.

## Concurrency review checklist

Before closing a concurrency-sensitive change, verify:

- ncurses remains main-thread-only;
- every shared mutable field has an explicit synchronization owner;
- condition predicates are checked in loops;
- lock order remains consistent;
- no Task uses forced asynchronous cancellation;
- Queue callbacks have an active reservation;
- internal participants stay within the admitted contract;
- all children join on every exit path;
- owner-only publication remains ordered where required;
- project-close ordering prevents DB use-after-close;
- SSD lease ownership remains exact;
- Task userdata outlives terminal notification;
- appropriate deterministic concurrency tests pass;
- portable TSan qualification is preserved;
- Vulkan validation is reported separately;
- ASan/UBSan is run when the change also affects lifetime/memory.

## Current retained evidence

The canonical global-maintenance record is:

```text
docs/architecture/global_maintenance_audit.md
GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN
```

The current A6000 checkpoint is later:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The later A6000 proof exercised current bounded parallel paths through selected
Feature batch, Candidate, Matcher, Geometric Verifier v3 and Tracks without
changing the historical TSan qualification.

Historical evidence remains historical; new changes require validation scoped
to their actual concurrency surface.

## Summary

```text
NCURSES_OWNER=MAIN_THREAD_ONLY
ACTIVE_HEAVY_QUEUE_CALLBACKS=1
TASK_CANCELLATION=COOPERATIVE
INTERNAL_PARALLELISM=BOUNDED

PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO

TSAN_PORTABLE_PROJECT_MATRIX=QUALIFIED_PASS
TSAN_EXTERNAL_OPENCV_TBB=QUALIFIED
VULKAN_CONCURRENCY_VALIDATION=SEPARATE

NO_FIXED_GLOBAL_J8=YES
NO_REPEATED_UNCHANGED_WIPE=YES

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```
