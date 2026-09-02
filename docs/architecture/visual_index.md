# Visual Index v1

## Status

```text
VISUAL_INDEX_V1=IMPLEMENTED
VISUAL_INDEX_KIND=orb-lsh
VISUAL_INDEX_VERSION=1

CANDIDATE_PAIR=IMPLEMENTED
MATCHER=IMPLEMENTED

VISUAL_INDEX_GPU=REJECTED_WITH_MEASURED_REASON
CURRENT_PROJECT_DB_SCHEMA=v25
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Visual Index turns a homogeneous collection of immutable Feature Sets into bounded image-retrieval
candidates.

It is a retrieval stage, not a Matcher and not a geometric verifier.

Current downstream consumers are implemented:

```text
Feature Store
-> Visual Index
-> Candidate Pair Generator
-> Matcher
-> Geometric Verification
-> Tracks
```

Older text describing Candidate Pair or Matcher as future consumers is historical design context and is
not current status.

## Algorithm

Visual Index v1 uses deterministic binary LSH over ORB descriptors.

It uses six tables.

Each table selects 24 distinct positions from the 256 ORB bits.

The frozen v1 position rule is:

```text
position = (41 * table + 11 * bit) mod 256
```

A posting key is:

```text
(table_id, key24)
```

Changing this bit-selection policy requires a new Visual Index scientific version.

## Identity and configuration

Current kind/version:

```text
orb-lsh / 1
```

One index contains Feature Sets with homogeneous:

- descriptor type;
- descriptor dimension;
- extractor kind;
- extractor version;
- extractor parameter fingerprint.

Frozen v1 configuration contains:

```text
table_count = 6
key_bits = 24
max_features_per_set = 1..1024, default 512
max_bucket_postings = 1..4096, default 256
max_segments = 256
max_feature_sets_per_segment = 16
```

The canonical parameter fingerprint uses domain:

```text
L3DVICF1
```

with explicit little-endian fields.

No C struct padding, locale or host endianness enters the fingerprint.

## Sampling

At most `max_features_per_set` Feature entries are indexed.

V1 selects the increasing `feature_index` prefix.

Postings retain the original Feature index.

An empty Feature Set is a valid member and contributes no posting.

## Segment persistence

Project DB v6 introduced Visual Index persistence. v7 retained the model.

Later schema versions through v25 do not reinterpret Visual Index v1.

A logical index owns immutable READY segments.

One update publishes one segment containing between one and sixteen new Feature Sets.

Membership uniqueness is enforced on:

```text
(visual_index_id, feature_set_id)
```

A query snapshots the bounded READY segment list before asset reads.

A segment committed after that snapshot is visible to the next query, not retroactively injected into
the running query.

## Capacity

V1 currently allows:

```text
max_segments = 256
max_feature_sets_per_segment = 16
```

Therefore one v1 index can contain exactly up to:

```text
4096 Feature Sets
```

before another update returns the Visual Index limit.

This is an index-v1 capacity bound, not a project-wide image-count limit.

Compaction/base-delta redesign remains deferred.

## Segment File v1

Segment File v1 is explicitly little-endian and does not serialize C structs.

Magic:

```text
L3DVIDX\0
```

A posting contains:

```text
table_id:u32
key24:u32
feature_set_id:u64
feature_index:u32
reserved_zero:u32
```

Canonical persistent ordering is:

```text
table_id
key24
feature_set_id
feature_index
```

The complete Segment File is content-addressed by SHA-256 under the Visual Index asset tree.

## Publication

Publication follows the normal immutable-asset pattern:

```text
local temp
-> write
-> fsync
-> hash
-> no-overwrite publication/adoption validation
-> fsync directory
-> short Project DB transaction
```

A physical file may remain orphaned if DB publication fails after the file is published.

No partially committed READY segment is invented.

Durability distinguishes:

```text
DURABLE
PUBLISHED_NOT_DURABLE
```

## Reader validation

The reader validates the asset SHA before trusting the format.

It rejects:

- invalid counts;
- invalid offsets;
- overflow;
- malformed reserved fields;
- inconsistent DB metadata;
- unsupported future version.

A coherent future format version is `UNSUPPORTED_VERSION`, not generic corruption.

## Query

Query identity is centered on:

```text
(visual_index_id, query_feature_set_id)
```

Options include:

- ScanSet filter;
- same/other ScanSet policy;
- source-asset exclusion;
- minimum evidence count;
- `top_k` in `1..256`.

The source Feature Set/image is never returned as its own candidate.

## Evidence and score

One evidence unit is one distinct query `feature_index` that collides with the candidate.

Multiple tables or candidate postings do not multiply the same query-feature evidence.

Score:

```text
evidence_count / sampled_query_feature_count
```

Range:

```text
0..1
```

Canonical result order:

```text
score descending
evidence_count descending
image_id ascending
feature_set_id ascending
```

Visual Index score is retrieval evidence only.

It is not descriptor-match evidence and not geometric evidence.

## Burstiness bound

Bucket frequency is bounded across the retained query snapshot.

A bucket above `max_bucket_postings` is ignored.

This prevents common patterns from dominating score or creating unbounded posting accumulation.

The query accumulator is bounded to 4096 candidates and 256 returned results.

No global query cache is required.

## Durable Task

Task Kind:

```text
visual_index.update/1
```

Durable cursor:

```text
after_feature_set_id
```

One sequence handles a bounded admitted set of new Feature Sets, publishes a complete segment, commits
memberships, advances the cursor/checkpoint and returns through `sequence_break()` if more work remains.

Restart resumes from the durable cursor and membership uniqueness makes replay idempotent.

## Internal parallelism

The Queue owns one active heavy callback.

Visual Index may use bounded internal CPU participants inside that callback.

Current validated shape:

```text
CPU up to 16
batch/window 1..16
GPU 0
fixed RAM approximately 8 MiB
per-item RAM approximately 2 MiB
```

Each participant reads immutable Feature data into private work.

Participants do not publish the segment.

After join, the owner performs canonical total ordering, serialization, asset publication and Project DB
commit.

Thread-creation failure may fall back to owner computation of that slice without changing output.

## Determinism

The following must match the serial scientific result:

- posting set;
- posting order;
- Segment File bytes;
- SHA-256;
- membership set;
- generation ordering;
- query results.

Operational CPU width does not enter scientific identity.

## GPU policy

Current GPU classification:

```text
VISUAL_INDEX_GPU=REJECTED_WITH_MEASURED_REASON
```

The stage is dominated by posting construction, total ordering, hashing and deterministic publication,
and there is no validated production GPU seam that preserves the full contract with useful measured
benefit.

This does not authorize avoidable CPU serialism.

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

## Current downstream relationship

Candidate Pair Generator is implemented and consumes Visual Index queries.

Matcher is implemented and consumes persisted Candidate Pairs.

Therefore current relationship is:

```text
Visual Index
-> Candidate Pair Generator
-> Candidate Pair persistence
-> Matcher
```

Visual Index does not pass raw `feature_set_id + feature_index` pairs directly into a hypothetical
future Matcher.

The persisted Candidate Pair boundary remains explicit.

## Real A6000 evidence

The retained A6000 proof contains:

```text
Feature Sets     689
Candidate Pairs  38,420
Match Results    38,420
```

Final continuation replayed no new Visual Index work.

Checkpoint:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

This confirms the current Visual Index path was already durably reusable before downstream GV/Tracks
continuation.

## Limits

Current v1 limits/non-goals include:

- no segment compaction;
- one index limited to 4096 Feature Sets;
- no GPU backend;
- no geometric meaning assigned to retrieval score;
- no dense project-wide pair matrix.

A future index version may change capacity or data structure only through an explicit versioned
scientific/persistence decision.

## Summary

```text
VISUAL_INDEX_V1=IMPLEMENTED
VISUAL_INDEX_KIND=orb-lsh
VISUAL_INDEX_VERSION=1
VISUAL_INDEX_CAPACITY=4096_FEATURE_SETS
VISUAL_INDEX_SEGMENT_MEMBERS=16
VISUAL_INDEX_TOP_K_MAX=256

VISUAL_INDEX_TASK=visual_index.update/1
VISUAL_INDEX_GPU=REJECTED_WITH_MEASURED_REASON

CANDIDATE_PAIR=IMPLEMENTED
MATCHER=IMPLEMENTED

CURRENT_PROJECT_DB_SCHEMA=v25
REAL_A6000_PRE_SFM=PASS/FROZEN
```
