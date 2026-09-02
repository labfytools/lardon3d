# Task Kind Registry

## Status

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

TASK_KIND_REGISTRY_STATUS=IMPLEMENTED
COMPUTE_GOVERNOR_V2=PASS/FROZEN
ORB_VULKAN_ASYNC_EXECUTION=PASS/FROZEN

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

The production registry contains exactly sixteen version-1 Task Kinds.

The registry is a bounded static dispatch table. It maps durable Task Kind identity to a reconstruction
function. It is not a scheduler, dependency graph, plugin loader, resource-policy database or
scientific identity resolver.

Project DB v25 is the current schema head. Earlier schema versions remain authoritative for the Task
payloads they introduced.

## Identity contract

A durable Task has several distinct identities and versions:

- stable Task ID;
- Task Kind string;
- Task Kind version;
- generic checkpoint codec version;
- Task state;
- scientific input identity owned by the Task-specific payload.

These fields are not interchangeable.

A Task Kind v1 string is ASCII, 1 to 64 characters, with the form:

```text
[a-z0-9][a-z0-9._-]*
```

The Task Kind version is a non-zero integer.

No Task Kind is inferred from:

- Task display name;
- callback address;
- userdata address;
- source file name;
- persisted function pointer;
- Task ID;
- scientific fingerprint.

No normalization is performed on the persisted kind string.

## Registry ownership

The registry contains at most 64 descriptors in a static immutable array.

Lookup is:

- linear;
- deterministic;
- allocation-free;
- safe for concurrent readers.

The registry never dynamically loads code.

A descriptor contains only:

- Task Kind string;
- Task Kind version;
- reconstruction function.

The public descriptor does not contain resource-policy state, CPU topology, GPU identity, scratch
state or persisted scheduler configuration.

## Reconstruction ownership

A reconstruction function returns:

- callback;
- callback userdata;
- optional userdata destructor;
- Task-specific private binding state where required.

Before ownership transfer, the registry destroys any newly allocated userdata on failure.

After a successful `Lardon3DTask` restoration, the Task owns the userdata and destroys it exactly once
after execution lifetime ends.

A business-specific constructor is never called while the Project DB mutex is held.

Project-open recovery copies the durable record out of the DB boundary before registry lookup and
Task-specific reconstruction.

## Exact legacy estimate normalization

The registry may recognize exact historical operational resource descriptors for restart compatibility.

The reconstruction function always receives the original durable snapshot.

If an exact historical descriptor is recognized, the registry may replace only the private effective
resource estimate passed to the restored runtime Task.

This normalization is:

- ephemeral;
- deterministic;
- exact-shape only;
- non-persistent.

It never rewrites:

- the generic checkpoint;
- Task ID;
- Task Kind/version;
- Task progress;
- Task-specific cursor;
- scientific parameters;
- scientific fingerprint;
- Project DB payload.

A neighboring or partially matching historical resource descriptor is rejected rather than guessed.

A crash before a later terminal checkpoint may therefore repeat the same exact normalization on the
next restart.

## Production inventory

The production registry is created by:

```text
src/task_kinds.c::lardon3d_task_kind_registry_production()
```

It contains exactly these sixteen version-1 kinds:

| # | Task Kind | Durable payload / introduction | Reconstruction |
| ---: | --- | --- | --- |
| 1 | `raw.develop` | Project DB v22 `raw_development_tasks` | `lardon3d_raw_development_task_reconstruct` |
| 2 | `raw.develop.batch` | Project DB v24 `raw_development_batch_tasks` | `lardon3d_raw_development_batch_task_reconstruct` |
| 3 | `photo_quality.triage` | Project DB v21 | `lardon3d_photo_quality_task_reconstruct` |
| 4 | `acquisition_campaign.run` | Project DB v20 | `lardon3d_acquisition_campaign_task_reconstruct` |
| 5 | `import.images` | historical typed import payload | `lardon3d_image_import_reconstruct` |
| 6 | `features.extract` | historical Feature Task payload | `lardon3d_feature_extract_reconstruct` |
| 7 | `features.extract.batch` | Project DB v25 `feature_extract_batch_tasks` | `lardon3d_feature_extract_batch_reconstruct` |
| 8 | `features.extract.sift` | typed SIFT Feature payload | `lardon3d_sift_extract_reconstruct` |
| 9 | `features.extract.rootsift` | typed RootSIFT Feature payload | `lardon3d_sift_extract_reconstruct` |
| 10 | `visual_index.update` | Visual Index Task payload | `lardon3d_visual_index_update_reconstruct` |
| 11 | `candidate_pair.generate` | Project DB v9 `candidate_pair_generate_tasks` | `lardon3d_candidate_pair_generate_reconstruct` |
| 12 | `matcher.run` | Project DB v11 `matcher_tasks` | `lardon3d_matcher_task_reconstruct` |
| 13 | `geometric_verifier.run` | Project DB v13 `geometric_verifier_tasks` | `lardon3d_geometric_verifier_task_reconstruct` |
| 14 | `track_builder.run` | Project DB v15 `track_builder_tasks` | `lardon3d_track_builder_task_reconstruct` |
| 15 | `sparse_sfm.run` | Project DB v17 `sparse_sfm_tasks` | `lardon3d_sparse_sfm_task_reconstruct` |
| 16 | `incremental_reconstruction.run` | Project DB v18 `incremental_reconstruction_tasks` | `lardon3d_incremental_reconstruction_task_reconstruct` |

The current detailed resource-capability inventory is owned by
[`resource_governor.md`](resource_governor.md), in its production sixteen-kind audit.

Historical documents that correctly recorded fewer kinds at their checkpoint remain historical
evidence. They must not be rewritten merely to make their old count equal the current count.

## Current additive Task Kinds

The two Task Kinds added after the historical fourteen-kind maintenance inventory are:

```text
raw.develop.batch/1
features.extract.batch/1
```

### `raw.develop.batch/1`

Project DB v24 binds one Task to one immutable selected execution.

The selected-execution cursor remains authoritative. Independent RAW items may be prepared within a
bounded admitted window, participants join, and the owner publishes the selected prefix in canonical
order.

The Task-specific restart path reuses exact already published RAW-derived representations and does not
infer identity from path, basename or processing position.

### `features.extract.batch/1`

Project DB v25 binds one Task to:

- one immutable selected execution;
- one monotonic `next_item_index`;
- exact ORB kind/version/parameters/fingerprint.

Independent selected images may be prepared concurrently without participant SQLite publication.

After join, the owner validates/reuses the exact READY Feature Set, publishes in selected-item order,
advances the typed cursor and then advances generic Task progress/checkpoint.

The historical `features.extract/1` kind remains valid and reconstructible.

```text
PER_IMAGE_FEATURE_RESULT=ATOMIC
PER_IMAGE_ATOMICITY_REQUIRES_CROSS_IMAGE_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO
```

## Compute Governor v2 private seam

```text
COMPUTE_GOVERNOR_V2=PASS/FROZEN
```

The public registry descriptor remains limited to kind, version and reconstruct function.

Resource capability alternatives live in private runtime state, including the opaque `Lardon3DTask`
and private Task/Resource Governor seams.

The key production admission boundaries remain:

- Queue admissibility selection;
- `lardon3d_task_sequence_break()`;
- Resource Governor capability selection from one fresh snapshot.

This private capability envelope is not:

- scientific identity;
- a new Project DB payload;
- a Task Kind version;
- a scheduler;
- a dependency graph;
- durable hardware identity.

The selected resource contract is immutable for one admitted sequence. A later sequence may receive a
different admitted contract.

A Task with no valid alternatives receives the honest fixed capability represented by its contract.

## Host CPU policy

The Resource Governor owns host CPU policy.

It determines:

- allowed affinity mask;
- package/core/SMT topology where available;
- interactive reserve;
- compute pool;
- actual worker affinity;
- per-kind admission bounds.

CPU IDs never enter:

- Task Kind descriptor;
- generic checkpoint;
- Project DB Task payload;
- scientific fingerprint.

Reference-host observations such as a 12-logical-CPU compute pool are evidence for that host, not a
portable registry ceiling.

The canonical repository policy is:

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

After the interactive reserve and all safety constraints are preserved, useful validated compute
capacity should not remain idle merely because an older durable resource descriptor was smaller.

## OpenCV kinds

The OpenCV-controlled kinds include:

```text
features.extract
features.extract.sift
features.extract.rootsift
```

Exact historical CPU1/CPU12 descriptors may be recognized for restart compatibility.

The effective current Task request can use the portable positive OpenCV upper bound, while the Resource
Governor limits actual admission to the host compute pool.

The callback applies the admitted OpenCV thread count and restores the process-wide baseline on every
exit path.

Controlled validation at 1/2/4/8/12 threads preserved the required scientific outputs for the
validated ORB/SIFT/RootSIFT contracts.

Those thread counts are operational evidence and do not change:

- extractor fingerprint;
- Feature Set identity;
- Feature File format;
- Task Kind version.

For `features.extract.batch/1`, cross-image participants are the primary concurrency mechanism.
OpenCV CPU teams are not multiplied blindly inside every participant.

## Candidate Pair normalization

`candidate_pair.generate/1` reconstructs:

- `visual_index_id`;
- `after_feature_set_id`;
- `top_k`;
- `minimum_evidence_count`;
- `scanset_filter`;
- `exclude_same_asset`.

Exact earlier CPU1 and CPU12 resource shapes may be normalized in memory.

The current validated Candidate capability is represented by the current source/runtime contract,
including:

```text
fixed RAM       256 KiB
per-item RAM      8 MiB
batch range       1..64
CPU/batch         coupled during useful scaling
GPU               none
```

The registry does not reinterpret the old durable checkpoint as if it had been created with those
current values.

## Matcher reconstruction

`matcher.run/1` reconstructs the immutable Matcher configuration and durable
`after_candidate_pair_id` cursor.

Project DB v11 introduced the typed Matcher payload after Match Result persistence in v10.

### Durable backend-class compatibility

The reconstruct path accepts only explicitly supported exact historical/current resource signatures.

These include the validated classes required for:

- current CPU operation;
- current ORB AUTO/MIXED policy;
- explicit Vulkan operation;
- exact historical CPU/Vulkan restart shapes.

A neighboring signature fails rather than being treated as a backend hint.

No hardware identity is persisted in `matcher_tasks`.

### ORB AUTO

Only the AUTO/MIXED durable class reconstructs the shared GPU-first policy.

On a portable build with no usable Vulkan backend, the same durable policy can reconstruct while
exposing only the CPU capability.

Restoring a fixed CPU or fixed Vulkan historical Task does not mutate shared AUTO availability state.

Backend probing/initialization does not occur on the `project_open()` thread. Actual backend begin
belongs to admitted Queue execution.

### Current useful ORB bounds

The current production AUTO policy retains:

```text
GPU-first when eligible
CPU fallback complete
normal Vulkan inflight depth = 1
validated private safety depth = 2
normal useful Vulkan batch <= 8
```

Depth 2 and Matcher batch 12 remain validated private/safety or benchmark capabilities where
applicable, but did not meet the useful-throughput deadband for normal AUTO policy.

Those performance decisions do not change:

- `matcher.run/1`;
- Matcher fingerprint;
- Match Result identity;
- Match File bytes;
- durable cursor.

## Geometric Verifier reconstruction

`geometric_verifier.run/1` reloads:

- immutable Fundamental verifier configuration;
- validated verifier fingerprint;
- `after_match_result_id`.

Project DB v13 adds only its typed Task payload.

The exact historical serial resource descriptor is recognized for compatibility and may be normalized
ephemerally to the current outer-parallel capability.

The current validated execution shape allows:

- up to 8 useful participants;
- a safe admitted window up to 16 Match Results;
- 8 MiB per admitted parent;
- owner-only ordered publication.

The scientific USAC/MAGSAC inner solver remains in its validated serial configuration. Outer
cross-item concurrency does not change verifier science.

## Track Builder reconstruction

`track_builder.run/1` reconstructs the explicit immutable scope from:

- Project DB v15 typed Task payload;
- validated little-endian scope asset;
- exact builder selector/fingerprint;
- exact GVR scope identity.

Corruption, unsupported version, fingerprint mismatch, checksum mismatch, non-canonical ordering,
duplicates or invalid scope identity make the Task non-reconstructible.

Track Builder restart does not invent a scope from current Project DB contents.

## Sparse SfM reconstruction

```text
sparse_sfm.run/1
```

is **PASS/FROZEN**.

It reloads the explicit Project DB v17 payload, restores the persisted generic Task estimate and
replays the frozen Sparse SfM D/E execution from explicit Track Set and calibration-scope references.

F0 is recomputed from the scientific payload.

The generic checkpoint remains version 1.

The historical fixed CPU1/batch1 Sparse SfM estimate is part of that frozen Task contract. It is not a
global argument for serializing unrelated modern Task Kinds.

## Incremental reconstruction

```text
incremental_reconstruction.run/1
```

is **PASS/FROZEN**.

It reloads the Project DB v18 payload containing:

- predecessor reconstruction;
- extension Track Set;
- calibration scope;
- Phase H fingerprint.

Restart recomputes from those immutable inputs.

No solver state, generic DAG or hidden dependency edge is persisted.

## Generic checkpoint and Project DB boundary

The generic checkpoint codec remains version 1.

Project DB stores Task Kind and Task Kind version in the generic Task summary.

Rows migrated from the oldest untyped schema remain:

```text
task_kind = NULL
task_kind_version = NULL
```

and are classified as:

```text
LEGACY_UNTYPED
```

They remain inspectable but cannot be reconstructed as a typed production Task.

Unknown kinds are classified separately from unsupported versions.

A future unknown Task Kind or unsupported Task Kind version must never trigger guessed code execution.

No function address is persisted.

## Project-open recovery

`project_open()` uses the immutable production registry to restore eligible Tasks.

The recovery flow is conceptually:

```text
load bounded Task page
-> copy durable record outside DB mutex
-> validate checkpoint and summary
-> classify Task Kind/version
-> reconstruct Task-specific binding
-> apply exact legacy resource normalization when allowed
-> restore Lardon3DTask
-> enqueue through normal Queue
```

A full Queue window stops further recovery without mutating unprocessed Tasks.

Task-local failures remain task-local unless the project schema/identity itself is invalid.

## Resource feedback is not scientific state

Private feedback may record bounded throughput observations per Task Kind/backend.

It may influence a later resource contract only where the capability contract permits adaptation.

It must not modify:

- scientific inputs;
- Task-specific durable cursor;
- Project DB identity;
- Task Kind/version;
- generic checkpoint codec;
- scientific fingerprint;
- canonical result ordering.

Only successfully completed durable work counts as useful work for adaptive feedback where that Task
contract requires it.

Uncertain publication does not become a successful scientific sample.

## Scratch boundary

The current production registry contains sixteen Task Kinds, but none has an authoritative
scratch-consuming scientific Task contract.

```text
CURRENT_PRODUCTION_TASK_KINDS=16
CURRENT_SCRATCH_CONSUMING_TASK_KINDS=0
```

The external SSD controller and Resource Governor scratch wrappers remain operational infrastructure.

Scratch, swap and zram do not enlarge RAM admission.

A future scratch consumer requires an explicit Task-specific eligibility, lifetime and cleanup
contract. Merely registering the SSD does not change a Task Kind.

## What the registry does not own

The registry does not own:

- Queue scheduling;
- resource admission;
- CPU topology;
- GPU selection;
- scratch allocation policy;
- Project DB migration;
- scientific fingerprint definitions;
- artifact path resolution;
- dependency/DAG planning;
- autosave timers;
- orphan reconciliation.

It only provides typed durable dispatch and reconstruction ownership.

## Current intentionally unfinished work

These remain outside the registry contract:

- global orphan-file reconciliation;
- generic dependency/DAG scheduling;
- any future new Task Kind not explicitly added to the production array;
- future resource-capability changes requiring new proof.

Existing reconstructible Task Kinds already checkpoint at their Task-specific durable boundaries.
The registry must not move those cursors forward with an independent autosave timer.

## Current checkpoint evidence

The current registry inventory includes both additive selected-execution kinds:

```text
raw.develop.batch/1
features.extract.batch/1
```

The retained A6000 checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

That proof exercised the current upstream/downstream registry through:

- selected RAW batch;
- selected Feature batch;
- Visual Index;
- Candidate Pair;
- Matcher;
- Geometric Verifier v3;
- Track Builder;

and stopped before real Sparse SfM.

The current production inventory remains exactly sixteen Task Kinds.

## Summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16
CURRENT_SCRATCH_CONSUMING_TASK_KINDS=0

TASK_KIND_REGISTRY_STATUS=IMPLEMENTED
TASK_KIND_REGISTRY_CAPACITY=64_DESCRIPTORS
GENERIC_CHECKPOINT_VERSION=1

RAW_BATCH_TASK=raw.develop.batch/1
FEATURE_BATCH_TASK=features.extract.batch/1

COMPUTE_GOVERNOR_V2=PASS/FROZEN
ORB_VULKAN_ASYNC_EXECUTION=PASS/FROZEN

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```
