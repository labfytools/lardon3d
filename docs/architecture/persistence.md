# Lardon3D Persistence and Project Database

## Current authority

The current Project DB schema is **v25**.

```text
CURRENT_PROJECT_DB_SCHEMA=v25

v22  selected scientific execution foundation        PASS/FROZEN
v23  generic optical-context overlay                  IMPLEMENTED/VALIDATED/REVIEWED
v24  raw.develop.batch/1 persistence                  IMPLEMENTED/VALIDATED
v25  features.extract.batch/1 persistence             IMPLEMENTED/VALIDATED
```

Project DB evolves additively. Older schema versions remain valid historical contracts when a
section explicitly documents the state published by that version. They must not be rewritten as if
they had always contained later overlays.

The detailed schema, migration ledger and identity contracts are owned by
[Project Database](project_database.md). This document owns the persistence model and publication,
checkpoint, recovery and artifact-boundary rules.

## Persistence model

Lardon3D keeps queryable logical state in SQLite Project DB while large numerical payloads remain in
bounded external files or artifacts designed for their format.

The split is intentional:

- identity, lifecycle state, relations, durable Task payloads and publication metadata -> SQLite;
- large descriptors, match payloads, checkpoints and other numerical artifacts -> bounded files;
- a file path is a storage locator after identity resolution, never scientific identity by itself.

Project DB does not serialize runtime-only objects such as mutexes, condition variables, callbacks,
userdata pointers, worker threads, Governor reservations or live execution contracts.

## Core invariants

- durable publication is explicit and ordered;
- partial artifacts are never accepted as complete scientific outputs;
- recovery begins from the last durable boundary the owning contract can prove;
- SQLite state and external files are not falsely described as one distributed transaction;
- immutable scientific identity is not inferred from path, basename, timestamp or operational IDs;
- large collections are read and written through bounded interfaces;
- retry behavior must converge only from identities already established by the owning contract;
- Project DB schema migration never silently creates new scientific meaning for historical rows.

## Logical and binary publication

The generic publication shape is:

```text
bounded computation
-> operation-owned temporary output
-> format/content validation
-> atomic file publication where required
-> Project DB transaction
-> READY / published durable state
```

The exact ordering is owned by the subsystem. Some contracts publish an immutable file before the
SQLite transaction and therefore explicitly admit an orphan-file window. Others publish metadata and
files through a more specialized ordered protocol. No document may strengthen a subsystem guarantee
beyond the actual implementation.

Failure cleanup may remove only resources owned by the current operation. Shared immutable assets are
not deleted merely because a later metadata transaction fails.

## Durable Task checkpoint v1

### Durable state

The versioned Task checkpoint contains bounded logical Task state only. It includes the stable Task
identity, name, immutable estimate, observed state, recovery state, progress, message, timestamps and
sequence count. It does not embed large scientific artifacts.

Task kind and typed business payload are separate concerns. The generic checkpoint format v1 does not
become a miniature Project DB and does not replace typed Project DB persistence.

### Process-stop normalization

| Observed state | Restored state |
|---|---|
| `TASK_PENDING` | `TASK_PENDING` |
| `TASK_RUNNING` | `TASK_PENDING` |
| `TASK_PAUSED` | `TASK_PENDING` |
| `TASK_COMPLETED` | `TASK_COMPLETED` |
| `TASK_FAILED` | `TASK_FAILED` |
| `TASK_CANCELLED` | `TASK_CANCELLED` |

A sequence break is not a persistent Task state. If execution stopped while a sequence was active,
recovery returns the Task to `TASK_PENDING`; its durable sequence count remains retained and a new
Governor admission is required before execution resumes.

### Standalone checkpoint file

The v1 codec is bounded and field-encoded with explicit magic/version/size/checksum semantics. It does
not serialize native structs or native padding.

Standalone publication uses a unique temporary file in the same directory, synchronizes the file,
renames atomically and then synchronizes the parent directory.

The result distinguishes three important boundaries:

- before successful rename: failure leaves the previously published checkpoint unchanged;
- after successful rename: the new visible checkpoint is published and is not presented as rolled back;
- parent-directory sync failure after rename: result is `PUBLISHED_NOT_DURABLE`; the visible file is
  valid, but name persistence across crash/power loss is not guaranteed by Lardon3D.

`OK` means both content and directory-entry synchronization completed, subject to the guarantees of the
filesystem and storage stack.

Persistent sizes are rejected before conversion if they exceed the local representable domain. The v1
timestamp representation likewise requires values representable by the local `time_t` before runtime
conversion.

## Project-owned checkpoint protocol

The project checkpoint protocol coordinates a Task file and SQLite metadata without claiming a true
filesystem+SQLite transaction.

The current project-level ordering is:

```text
capture bounded Task snapshot
-> publish .chk.next
-> record Task/checkpoint summary in SQLite
-> promote .chk.next to canonical .chk under .chk.lock
```

The canonical location is:

```text
.lardon3d/checkpoints/<task_id>.chk
```

The advisory `.chk.lock` exists only as process synchronization; it is not recovery data.

Recovery obtains the lock, reloads the relevant Project DB record because it may have changed while
waiting, and then selects a codec/version-valid checkpoint whose stored summary matches the Project DB
summary exactly for the fields Project DB owns.

A valid canonical `.chk` has priority. A valid matching `.chk.next` may be promoted when the canonical
file does not match. Stale or corrupt `.next` files never override a valid canonical checkpoint.

A missing, corrupt, future-version or summary-mismatched checkpoint makes that Task non-recoverable; it
does not make unrelated project state invalid.

## Project DB foundation

Project DB is SQLite with explicit schema versioning and sequential transactional migrations.

The retained configuration uses:

```text
foreign_keys=ON
journal_mode=DELETE
synchronous=FULL
busy_timeout=5000
```

`DELETE` journal mode matches the current single-owner Project DB model and avoids persistent WAL/SHM
files. The timeout bounds waiting on an external lock.

The project identity is duplicated intentionally across `project.ini` and the `project` table and must
match. A divergence is an error, not an opportunity to invent a new identity.

Published catalog and scientific IDs use SQLite integer identities according to their owning schema.
Where `AUTOINCREMENT` is part of the contract, a committed published identity is not later reused for a
different object. An ID allocated only inside a rolled-back transaction is not a published identity.

## Historical Project DB v7 foundation

Project DB v7 is the historical persistent project/runtime foundation. It covers the durable project
identity, Tasks, checkpoint references, generic artifacts, ScanSets, logical images, image assets,
Feature Store metadata and the segmented Visual Index foundation.

Important v7-era persistence rules remain current unless a later contract explicitly supersedes them:

- Task summary plus checkpoint reference is a single SQLite transaction;
- large checkpoint and scientific files remain external;
- an artifact file is published and validated before Project DB marks it `READY`;
- Project DB stores bounded metadata and references, not large descriptor/posting payloads;
- `AUTOINCREMENT` is used where published catalog/scientific IDs must not be recycled after committed
  deletion;
- the persistent import source path records durable execution intent, while imported scientific data
  ultimately depends on the managed immutable asset rather than the original external source path.

### Generic artifact orphan window

For generic file-first publication, a successful file publication followed by SQLite `BUSY` or another
SQLite failure leaves a valid orphan file on disk while Project DB retains its previous truth.

That file is not silently deleted. Global orphan reconciliation is a separate maintenance capability.
The absence of such reconciliation does not justify pretending the file and SQLite update were atomic.

## Import and managed source assets

`import.images` persists its source path and ScanSet identity and checkpoints after validated bounded
work. Managed image assets are content-addressed by SHA-256 after a complete bounded copy/hash pass.

A concurrently existing asset is adopted only after the implementation verifies the expected content
and size according to the owning contract. SQLite publication follows file publication.

`manifest.tsv` remains a historical/legacy projection. SQLite is the canonical logical commit for the
persistent catalog. The `legacy_image_catalog_pending` marker means legacy data may remain outside the
current catalog model; it does not claim that historical manifest rows were silently converted into
fully proven catalog identities.

## Selective Task recovery

Project open performs bounded selective recovery using Project DB pages and the Task Kind Registry.

A recoverable Task must have:

- a valid Project DB Task record;
- a supported Task kind and version;
- a valid coherent checkpoint boundary required by that kind;
- reconstructable typed business persistence where the kind requires it.

The recovery scan copies records outside the SQLite mutex before business reconstruction and enqueue.
A full Queue window stops the scan without mutating the unvisited records.

Project-level schema, migration and project-identity errors are fatal to opening the project. Per-Task
legacy, unknown-kind, unsupported-version, missing-checkpoint, invalid-checkpoint, unavailable-source or
reconstruction errors are isolated to the affected Task where the owning contract permits.

## External scientific artifacts

### Feature Store

Feature files are immutable external artifacts with versioned bounded readers. Project DB stores their
identity and publication metadata only after the external file is valid according to the Feature Store
contract.

Feature descriptors are not duplicated into SQLite.

### Visual Index

Visual Index postings remain in bounded immutable external segments. Project DB stores index identity,
segment metadata and memberships. Durable update Tasks retain the bounded cursor required to resume
publication without rebuilding already accepted segments.

### Match and later scientific payloads

Match payloads and other large scientific representations follow the same architectural principle:
SQLite owns durable identity, relations and bounded metadata; specialized external formats own large
numerical payloads where the subsystem contract requires them.

## Project DB v16-v22 retained scientific foundation

The scientific and persistence contracts introduced through v16-v22 remain historical PASS/FROZEN
foundations. Later schema versions are additive overlays and do not reinterpret those rows.

In particular:

- v16 publishes the immutable Sparse SfM persistence model;
- v17 adds the durable typed Sparse SfM Task payload for Gate F;
- v18 adds Phase H v1 incremental-reconstruction identity/persistence;
- v19 adds Capture / Asset Provenance v1;
- v20 adds durable acquisition-campaign Task persistence;
- v21 adds Photo Quality Triage persistence;
- v22 adds selected scientific execution, explicit Capture SOURCE-asset mapping and durable selected
  RAW development persistence.

Detailed tables, identities and migration invariants are defined in
[Project Database](project_database.md).

## Project DB v23 - generic optical-context overlay

Project DB v23 is an additive optical-context overlay above the v22 scientific foundation.

It separates four identities:

- camera body profile;
- lens profile;
- optical configuration;
- optical calibration profile.

The migration creates the optical relations empty. It does not inspect EXIF, path, basename, SHA-256,
dimensions, device name or historical calibration to backfill identity.

Electronic metadata aliases, when present, use exact stored identity semantics. A manual lens without
EXIF is a normal explicit profile and does not require a fabricated metadata alias.

Calibration selection is explicit and requires exact compatible optical configuration. No silent
interpolation, substitution or inferred calibration identity is introduced by persistence.

Historical S21/A6000 copies migrated through the optical overlay without changing their existing
scientific rows; empty optical tables remain an honest state until explicit data is supplied.

## Project DB v24 - RAW batch persistence

Project DB v24 adds only the typed durable relation required by `raw.develop.batch/1`:

```text
raw_development_batch_tasks(task_id, selected_execution_id)
```

The migration is additive and DDL-only. It does not create Capture, Asset, Image or selected-execution
identity and does not rewrite historical `raw.develop/1` Tasks.

The selected execution remains the durable scientific ordering authority. The batch Task may prepare
independent RAW items concurrently under Governor admission, but after all participants join, the owner
publishes selected representations in deterministic selected-item order.

The durable ordering remains:

```text
prepare bounded independent RAW items
-> join participants
-> owner publishes exact selected item
-> selected item/cursor commit
-> generic Task progress/checkpoint
```

A crash may therefore leave generic Task progress behind already durable selected scientific state; it
must never move generic progress ahead of unpublished selected state. Recovery resumes from durable
selected-execution identity/cursor and exact already-published representations rather than guessing
from paths or files.

The retained real A6000 run completed this RAW-batch path for all 689 selected RAW representations.

## Project DB v25 - Feature batch persistence

Project DB v25 is the current schema head. It adds only the typed durable relation required by
`features.extract.batch/1` through `feature_extract_batch_tasks`.

The v25 row binds one durable Feature-batch Task to:

- one immutable selected execution;
- the monotone selected-item prefix/cursor;
- the exact ORB extractor kind/version/parameters/fingerprint domain required by the Task contract.

The migration is additive and DDL-only. It creates no Feature Set, converts no historical
`features.extract/1` Task and infers no Image or Feature Set identity.

Selected images may be prepared concurrently without SQLite access. After participants join, the owner
publishes Feature Sets in selected order and advances the durable Feature cursor only when the exact
READY Feature Set is already durable.

A crash may leave generic Task checkpoint/progress or the Feature cursor behind an immutable Feature
Set that was already published. Recovery must revalidate and reuse that exact result; it must not infer
a replacement identity.

The retained real A6000 proof completed the v25 path with 689 Feature Sets and then continued through
Visual Index, 38,420 Candidate Pairs, 38,420 Match Results, Geometric Verification and Tracks without
replaying acquisition, RAW or Feature work.

This persistence result does **not** imply that real Sparse SfM or Dense/MVS was executed. Those counts
remain zero in `REAL_A6000_PRE_SFM=PASS/FROZEN`.

## Capture / Asset / Image identity boundary

Persistence must preserve these distinctions:

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

`asset_id` identifies a managed immutable asset record. SHA-256 identifies immutable bytes according to
the asset contract. `image_id` identifies a scientific image representation. `capture_id` identifies a
physical acquisition representation in Project DB. Task and campaign-group IDs remain operational
identities.

Retry and recovery may use an explicit persisted mapping between these domains only where a canonical
contract defines it. They may not reconstruct missing identity from coincidental equality or metadata.

## Acquisition-campaign crash/restart ordering

For durable campaign execution, the important persistence boundary is conceptually:

```text
S3-E returns capture_id
-> persist group_id -> capture_id mapping and campaign cursor
-> advance generic Task progress/checkpoint
-> next group may execute
```

The retained pre-return S3-E crash window remains intentional. If a Capture is created internally but
the process dies before the caller receives and durably retains its `capture_id`, campaign persistence
does not guess that identity from path, digest, basename, timestamp, metadata or `image_id`.

## Concurrency and ownership

A Project DB connection is serialized by its internal mutex. Each public compound operation owns its
entire transaction; a public transaction is not left open across calls.

External artifact I/O is not performed while holding the Project DB mutex where the owning contract
separates those operations. SQLite-owned strings and records are copied into caller-owned bounded
storage before returning.

Closing Project DB concurrently with an active Project DB call is forbidden by owner lifetime rules.
Application project/session teardown must first destroy and join the Queue so Task callbacks and leases
are finished, then close Project DB.

## Schema migration discipline

Project DB migrations are sequential, transactional and additive unless an explicit future human ticket
authorizes a different migration.

Current known sequence:

```text
v1 -> ... -> v22
v22 -> v23  generic optical-context overlay
v23 -> v24  RAW batch Task persistence
v24 -> v25  Feature batch Task persistence
```

A migration failure rolls back both its new schema objects and its schema-version publication marker.
A retry therefore starts from the previous complete known schema.

Future schema versions beyond v25 are rejected by the current implementation and require explicit human
authorization before code or documentation may treat them as current.

## Current status

```text
PERSISTENCE_DOCUMENT=CURRENT
CURRENT_PROJECT_DB_SCHEMA=v25

TASK_CHECKPOINT_V1                         IMPLEMENTED
PROJECT_CHECKPOINT_PROTOCOL                IMPLEMENTED/VALIDATED
PROJECT_DB_V7_FOUNDATION                   IMPLEMENTED
PROJECT_DB_V16_TO_V22_FOUNDATION           PASS/FROZEN
PROJECT_DB_V23_OPTICAL_OVERLAY              IMPLEMENTED/VALIDATED/REVIEWED
PROJECT_DB_V24_RAW_BATCH                    IMPLEMENTED/VALIDATED
PROJECT_DB_V25_FEATURE_BATCH                IMPLEMENTED/VALIDATED
REAL_A6000_PRE_SFM                          PASS/FROZEN
```

Current intentionally unfinished persistence-adjacent work includes global orphan-file reconciliation,
asset scrub/reconciliation and a general dependency/DAG recovery model. Those are separate future
capabilities; they do not change the current v25 schema authority.
