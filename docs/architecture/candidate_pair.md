# Candidate Pair subsystem

## Status

```text
CURRENT_PROJECT_DB_SCHEMA=v25
CANDIDATE_PAIR_MODEL=v1
CANDIDATE_PAIR_MODEL_STATUS=IMPLEMENTED
CANDIDATE_PAIR_TASK=candidate_pair.generate/1
CANDIDATE_PAIR_TASK_STATUS=IMPLEMENTED
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The Candidate Pair scientific model remains the Project DB v8 model. The durable Candidate Pair Task
was added in Project DB v9. Later schema versions through v25 are additive and do not redefine
Candidate Pair identity.

This document owns the Candidate Pair subsystem contract. Resource policy is governed by the Resource
Governor and the canonical resource documents; Candidate Pair declares bounded demand and preserves
deterministic scientific output.

## Purpose

The Candidate Pair subsystem answers one question:

> Which image pairs are worth presenting to the Matcher?

It does not answer whether two images actually have descriptor correspondences and it performs no
geometric verification.

The downstream Matcher is implemented and consumes persisted Candidate Pairs, but Matcher science and
persistence are outside this subsystem.

```text
Visual Index
    |
    v
Candidate Pair Generator
    |
    v
Candidate Pair persistence
    |
    v
Matcher
```

## Core invariants

| Invariant | Contract |
| --- | --- |
| Symmetry | `(A,B)` and `(B,A)` are the same scientific pair |
| Canonical order | Persist with `image_id_a < image_id_b` |
| No self-pairs | `image_id_a != image_id_b`, implied by the SQL ordering check |
| Persistent uniqueness | `UNIQUE(image_id_a, image_id_b)` |
| Bounded query result | `top_k <= LARDON3D_VISUAL_INDEX_TOP_K_MAX = 256` |
| Determinism | Same inputs and options produce the same pair decisions in the same canonical order |
| Idempotence | Repeating generation does not duplicate persisted pairs |
| Persistent model | Candidate Pair rows were introduced by Project DB v8 |
| Durable execution | `candidate_pair.generate/1` was introduced by Project DB v9 |

Candidate Pair identity is the canonical unordered image pair. Retrieval score, Visual Index provenance,
Task ID, timestamps and operational resource choices do not enter that identity.

## Persistent model

### `candidate_pairs` — Project DB v8

```sql
CREATE TABLE candidate_pairs(
    candidate_pair_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(candidate_pair_id>0),
    image_id_a INTEGER NOT NULL REFERENCES images(image_id),
    image_id_b INTEGER NOT NULL REFERENCES images(image_id),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK(image_id_a < image_id_b),
    UNIQUE(image_id_a, image_id_b)
);
CREATE INDEX candidate_pairs_image_a_idx ON candidate_pairs(image_id_a);
CREATE INDEX candidate_pairs_image_b_idx ON candidate_pairs(image_id_b);
```

The executable schema in `src/project_db.c` remains authoritative if prose and SQL excerpts ever
diverge.

### Public Project DB API

The Candidate Pair persistence surface includes:

- `lardon3d_project_db_create_candidate_pair()`;
- `lardon3d_project_db_load_candidate_pair()`;
- `lardon3d_project_db_find_candidate_pair()`;
- `lardon3d_project_db_list_candidate_pairs()`.

Creation canonicalizes the image order and persistence enforces uniqueness.

## Single-source generation

### API

```c
Lardon3DVisualIndexResult lardon3d_candidate_pair_generate(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *stats);
```

### Algorithm

For one source Feature Set:

1. load the source Feature Set;
2. obtain its `source_image_id`;
3. query the Visual Index with the supplied options;
4. for every returned candidate:
   - reject self-pairs;
   - canonicalize the image order;
   - find an existing Candidate Pair;
   - create the pair only when absent;
5. return bounded generation statistics.

The generator does not perform descriptor matching and does not perform geometric verification.

### Statistics

```c
typedef struct {
  uint32_t generated_count;
  uint32_t skipped_count;
  uint32_t queried_count;
} Lardon3DCandidatePairGenStats;
```

`generated_count` counts newly persisted pairs. `skipped_count` counts pairs already present.
`queried_count` counts candidates returned by the Visual Index query.

## Retrieval score and provenance

Retrieval score and Visual Index provenance are intentionally not stored in `candidate_pairs`.

Reasons:

- Candidate Pair identity is only the canonical image pair;
- retrieval score depends on Visual Index configuration;
- a later Visual Index execution may score the same pair differently;
- Matcher owns descriptor-level matching evidence;
- keeping retrieval evidence out of Candidate Pair identity preserves subsystem separation.

A generation fingerprint describes the generation request. It does not change the identity of an
already persisted Candidate Pair row.

## Determinism

### Deterministic inputs and decisions

For identical immutable inputs and options, the subsystem preserves:

- the same Visual Index query contract;
- the same top-K selection semantics;
- the same canonical image ordering;
- the same self-pair rejection;
- the same deduplication decisions;
- the same publication order for owner-published results.

### Non-scientific values

These values are not Candidate Pair scientific identity:

- `created_at`;
- `candidate_pair_id`;
- Task ID;
- resource reservation ID;
- admitted CPU count;
- admitted batch size.

`candidate_pair_id` is a durable technical identity allocated by SQLite. It is not a scientific
fingerprint.

## Generation fingerprint

### API

```c
void lardon3d_candidate_pair_generation_fingerprint(
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    unsigned char fingerprint[32]);
```

### Included fields

The generation fingerprint includes:

- `visual_index_id`;
- `source_feature_set_id`;
- `query_options->top_k`;
- `query_options->minimum_evidence_count`;
- `query_options->scanset_filter`;
- `query_options->exclude_same_asset`.

### Excluded fields

It excludes:

- `created_at`;
- `candidate_pair_id`;
- operational CPU/batch admission;
- processing order of unrelated source Feature Sets.

### Reuse meaning

The same fingerprint means the same generation request may be reused.

A different fingerprint means the generation request must be evaluated again. Existing canonical
Candidate Pair rows are not silently deleted merely because a different generation request is run;
idempotent persistence may reuse rows that remain selected.

## Project batch generation

### API

```c
Lardon3DVisualIndexResult lardon3d_candidate_pair_generate_batch(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t after_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *total_stats,
    uint64_t *last_feature_set_id);
```

### Ordering and bounds

The project batch path:

- pages Feature Sets in bounded pages;
- processes source Feature Sets in increasing `feature_set_id`;
- never assumes IDs are contiguous;
- keeps top-K bounded by the Visual Index contract;
- keeps query/result memory bounded;
- returns the last processed Feature Set for restart.

A pair selected from multiple sources is persisted once because canonical pair identity is unique.

The historical single-source and project-batch APIs remain valid. They are not, by themselves, the
complete current resource-execution description of the durable Task.

## Durable Task

### Task Kind

```text
candidate_pair.generate/1
```

Status: **IMPLEMENTED**.

### Durable unit

The durable cursor is `after_feature_set_id` in `candidate_pair_generate_tasks`.

The Task consumes a bounded ordered set of Visual Index source memberships. A sequence handles an
admitted bounded batch, publishes the canonical pair decisions, persists the cursor, checkpoints and
returns through `lardon3d_task_sequence_break()` before the next Governor admission.

### Checkpoint and restart

The Task checkpoints through:

```text
lardon3d_project_checkpoint_candidate_pair_generate_task()
```

Restart:

1. restores the generic Task snapshot;
2. loads the typed Candidate Pair Task payload;
3. restores `after_feature_set_id`;
4. reconstructs the production binding through the Task Kind registry;
5. resubmits through the normal Queue/Governor path;
6. reuses already persisted Candidate Pairs idempotently.

A crash may therefore repeat work after the last durable cursor, but it must not invent a second
scientific pair identity.

### Historical resource descriptors

Older durable snapshots are accepted only through exact compatibility shapes already recognized by the
registry.

Historical forms include the exact earlier descriptors documented by the implementation, including:

```text
128 KiB fixed
64 KiB per item
batch 1..64
CPU1
IO1
GPU0
```

and the later exact historical CPU12 / 256 KiB fixed / 64 KiB-per-item form.

Those shapes are restart compatibility evidence. They are not the current resource model and must not
be copied into new Task creation.

The original durable snapshot remains the source supplied to reconstruction. Compatibility
normalization is ephemeral and does not rewrite the persisted checkpoint or Candidate scientific
identity.

## Current resource contract

### Current Task estimate

The current validated Candidate Pair Task declares approximately:

```text
fixed RAM       256 KiB
per-item RAM      8 MiB
batch range       1..64
GPU demand        0
IO demand         bounded by the existing Task estimate
CPU demand        reducible and bounded by the host compute pool
```

The exact implementation constants remain authoritative in source. This documentation records the
current validated capability and intentionally does not retain the obsolete 24-source / 64-KiB-per-item
description as current policy.

### Coupled CPU and batch admission

Candidate Pair generation has independent source work, but additional CPU cannot exercise additional
participants if the admitted source batch remains one.

For this Task, CPU and batch scaling are therefore coupled during adaptation. Conceptually:

```text
CPU1 / batch1
CPU2 / batch2
then larger safe coupled rungs
```

subject to:

- the Task's declared maximums;
- the host compute pool;
- current Governor pressure;
- measured usefulness;
- current admission policy.

This coupling fixes an operational scaling defect. It does not modify Candidate Pair scientific
identity, query options, top-K behavior, publication order or persistence.

### Canonical resource principles

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

The interactive host reserve is preserved first. After that reserve and all safety constraints are
satisfied, safe and useful compute capacity should not be left idle merely to preserve an old
single-thread measurement.

Reference-host values are observations, not portable constants.

### Atomicity is not serialism

One source query and one Candidate Pair publication decision remain bounded scientific/transactional
units.

That does not imply that independent source preparation must run serially.

```text
PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO
```

## Internal concurrency

The Task Queue still owns one active callback. Candidate Pair uses bounded parallelism inside that
callback.

The validated shape is:

```text
one admitted Candidate owner Task
-> bounded source window
-> bounded CPU participants
-> private read-side preparation
-> join
-> owner publishes in canonical source order
```

Participants do not create Candidate Pair rows.

Each participant uses its allowed private read-side state. After participants join, the owner alone
performs the canonical `find` / create publication sequence.

The SQL `UNIQUE(image_id_a, image_id_b)` constraint remains a persistent integrity guard, not a
parallel scheduling primitive.

No second global scheduler, global worker pool or parallel SQLite writer subsystem is introduced.

## Persistence race boundary

The public pattern:

```text
find candidate pair
-> create candidate pair when absent
```

is not a general atomic compare-and-insert primitive across arbitrary concurrent writers.

The production durable Candidate Task avoids introducing competing pair writers: participant work is
read/preparation only and owner publication is serialized.

Other callers must not infer a stronger concurrency guarantee from the Task's owner-only publication
model.

## Resource complexity

### Query bound

```text
top_k <= LARDON3D_VISUAL_INDEX_TOP_K_MAX = 256
```

### Memory

Current Task admission uses the current estimate:

```text
256 KiB fixed + 8 MiB per admitted item
```

with batch bounded to `1..64`.

Actual admission can be reduced by the Governor and host compute pool. Swap, zram and external scratch
never enlarge admitted RAM.

Candidate Pair currently has no authoritative scratch consumer.

### Algorithmic shape

The subsystem does not allocate an O(N^2) project pair matrix.

Per source, bounded work is dominated by:

- bounded Visual Index query;
- bounded top-K filtering;
- canonical pair lookup/publication.

Project traversal is paged and restartable.

## GPU policy

Candidate Pair currently remains CPU.

The validated GPU audit classified it as:

```text
CANDIDATE_GPU=REJECTED_WITH_MEASURED_REASON
```

The workload is dominated by Visual Index access, filtering, branching and deterministic ordered SQLite
publication, and no validated GPU primitive currently preserves the complete Candidate contract with a
useful measured advantage.

This rejection does not authorize CPU serialism. Safe useful CPU parallelism remains required by the
canonical resource policy.

## Real A6000 evidence

The retained real A6000 pre-SfM execution exercised the current Candidate path before Matcher, GV and
Tracks.

It produced:

```text
Candidate Pairs = 38,420
Match Results   = 38,420
```

The later checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Candidate replay at the retained checkpoint created no new Candidate work. The proof continued through
Matcher, Geometric Verifier v3 and Tracks, then stopped before real Sparse SfM.

This evidence validates the operational Candidate path on that project. It does not change Candidate
Pair v1 identity or the Project DB v8/v9 historical contracts.

## Relationship to current Project DB

The Candidate Pair model was introduced in Project DB v8 and its durable Task payload in v9.

The current schema head is v25:

```text
v22  selected scientific execution foundation
v23  generic optical-context overlay
v24  raw.develop.batch/1 persistence
v25  features.extract.batch/1 persistence
```

Those later additive migrations do not reinterpret Candidate Pair rows.

## Known limits

Current limits and non-goals include:

- retrieval score is not persisted in `candidate_pairs` by design;
- Visual Index segment compaction remains separate work;
- no generic DAG dependency scheduler is introduced by Candidate Pair;
- no Candidate GPU backend is currently validated;
- no scratch/spill path is authoritative for Candidate Pair;
- arbitrary concurrent pair writers are not provided by the Task owner-publication model.

The downstream Matcher is implemented; it is not a missing Candidate Pair feature.

## Pipeline relationship

```text
Feature Store
    |
    v
Visual Index
    |
    v
Candidate Pair Generator  <- this document
    |
    v
Matcher
    |
    v
Geometric Verification
    |
    v
Tracks
```

Candidate Pair selects plausible image pairs. Matcher computes descriptor-level correspondence evidence.
Geometric Verification validates geometry. Track Builder creates multi-view observation tracks. These
scientific responsibilities remain separate.

## Summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25

CANDIDATE_PAIR_MODEL=v1
CANDIDATE_PAIR_MODEL_STATUS=IMPLEMENTED

CANDIDATE_PAIR_TASK=candidate_pair.generate/1
CANDIDATE_PAIR_TASK_STATUS=IMPLEMENTED

CANDIDATE_PAIR_PERSISTENCE_VERSION=v8
CANDIDATE_PAIR_TASK_PERSISTENCE_VERSION=v9

CANDIDATE_CURRENT_FIXED_RAM=256_KiB
CANDIDATE_CURRENT_PER_ITEM_RAM=8_MiB
CANDIDATE_CURRENT_BATCH_RANGE=1..64
CANDIDATE_CPU_BATCH_ADAPTATION=COUPLED
CANDIDATE_GPU=REJECTED_WITH_MEASURED_REASON
CANDIDATE_SCRATCH_CONSUMER=NO

PER_ITEM_ATOMICITY_REQUIRES_CROSS_ITEM_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```
