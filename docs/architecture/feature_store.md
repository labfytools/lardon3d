# Feature Store v1/v2

## Status

```text
CURRENT_PROJECT_DB_SCHEMA=v25

FEATURE_STORE_STATUS=IMPLEMENTED
FEATURE_FILE_V1=FROZEN
FEATURE_FILE_V2=IMPLEMENTED

HISTORICAL_FEATURE_TASK=features.extract/1
CURRENT_FEATURE_BATCH_TASK=features.extract.batch/1
PROJECT_DB_V25_FEATURE_BATCH=IMPLEMENTED/VALIDATED

PER_IMAGE_FEATURE_RESULT=ATOMIC
PER_IMAGE_ATOMICITY_REQUIRES_CROSS_IMAGE_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_PRE_SFM=PASS/FROZEN
```

The Feature Store is the persistent local visual-memory layer for Lardon3D.

The scientific identity of a Feature Set remains per image and immutable after publication. Project DB
v25 adds an operational selected-execution batch Task, but does not change Feature Set identity,
Feature File formats, ORB descriptor semantics or the historical single-image Task contract.

## Role and identity model

A logical `FeatureSet` belongs to one `image_id` and identifies:

- one extractor kind;
- one extractor version;
- one canonical parameter fingerprint;
- one exact source image content identity;
- one immutable published Feature Asset.

Two logical images with identical source content and identical extractor configuration keep distinct
`feature_set_id` values. They may share the same immutable physical Feature Asset when the resulting
Feature File bytes are identical.

Downstream correspondence identity references:

```text
feature_set_id + feature_index
```

Both components are immutable after Feature Set publication.

`feature_set_id` and `feature_asset_id` are SQLite `AUTOINCREMENT` identities. A committed published
identity is never reassigned to another object. SQLite stores logical relations, hashes, sizes,
extractor metadata and light metrics; keypoint arrays and descriptor arrays remain outside SQLite.

## Extractor registry and scientific configuration

The historical production ORB extractor uses OpenCV 5 behind a C boundary. No C++ exception or
`cv::Mat` crosses the public C API.

The historical registered Task Kind is:

```text
features.extract/1
```

That Task processes one image as one scientific unit.

The current selected-execution operational Task Kind is:

```text
features.extract.batch/1
```

It processes multiple independently selected images under one durable owner while preserving one
immutable Feature result per image.

### ORB v1 configuration

The ORB v1 configuration contains three `uint32_t` fields:

- `max_features`, range `1..8192`;
- `pyramid_levels`, range `1..16`;
- `fast_threshold`, range `1..255`.

Its SHA-256 parameter fingerprint is computed from the canonical 24-byte domain containing:

```text
L3DORBP1
version
max_features
pyramid_levels
fast_threshold
```

with fixed little-endian integer encoding.

The fingerprint does not depend on C struct padding, locale, Task ID, resource reservation,
admitted CPU count or admitted batch size.

ORB v1 produces 32-byte binary descriptors.

## OpenCV execution control

OpenCV thread count is process-wide operational state, not scientific identity.

Before Queue execution, Lardon3D establishes the validated OpenCV baseline from the actual host
compute pool. The active heavy callback applies the count required by its admitted contract and
restores the baseline on every exit path.

The Queue retains one active callback. This prevents unrelated heavy Tasks from racing the same
process-wide OpenCV configuration.

Historical `features.extract/1` may use the admitted OpenCV CPU count inside one image extraction.

For `features.extract.batch/1`, cross-image participants are the primary concurrency mechanism.
Independent participants prepare different images; Lardon3D does not blindly multiply a full OpenCV
thread team inside every participant.

The admitted CPU count is resource policy and never enters:

- ORB v1 fingerprint;
- Feature Set identity;
- Feature File bytes;
- source-image identity.

The controlled OpenCV 5.0.0 validation at 1, 2, 4, 8 and 12 threads required identical:

- feature count;
- keypoint order;
- binary32 values of all six persisted keypoint fields;
- descriptor row order and bytes;
- complete Feature File SHA-256.

Twelve threads are reference-host validation evidence, not a portable product ceiling.

`cv::KeyPoint::class_id` is not persisted by the frozen Feature File record and therefore cannot
define scientific identity.

## Decode bound

The 100,000,000-pixel limit is checked after `cv::imread`.

The API used by this implementation does not provide a reliable multi-format dimension probe without
decoding. Therefore decode peak memory can precede rejection.

Lardon3D does not claim pre-decode memory bounding and does not maintain a parallel JPEG/PNG dimension
parser for this purpose.

## Feature File v1

Feature File v1 is little-endian and requires IEEE-754 binary32.

Limits:

```text
maximum file size      16 MiB
maximum feature count  8192
ORB descriptor         U8 x 32
```

### Header

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `L3DFEAT\0` |
| 8 | 4 | format version = 1 |
| 12 | 4 | header size = 160 |
| 16 | 4 | feature count |
| 20 | 4 | descriptor dimension = 32 |
| 24 | 4 | descriptor type = U8 |
| 28 | 4 | keypoint record size = 24 |
| 32, 36 | 4 + 4 | decoded image width and height |
| 40, 48, 56 | 8 + 8 + 8 | keypoint offset, descriptor offset, total size |
| 64 | 32 | source image asset SHA-256 |
| 96 | 32 | parameter fingerprint |
| 128 | 16 | `orb\0` followed by reserved zero bytes |
| 144 | 4 | extractor version = 1 |
| 148 | 12 | reserved, must be zero |

Each keypoint contains six 32-bit words:

1. `x` binary32;
2. `y` binary32;
3. `size` binary32;
4. orientation binary32;
5. response binary32;
6. signed octave.

`x` and `y` are image pixels in the OpenCV-decoded image with top-left origin.

`size` is the neighborhood diameter in pixels.

Orientation is in degrees in `[0,360)`.

Keypoint record `i` corresponds exactly to descriptor row `i`.

## Feature File v1 validation

The reader validates:

- magic;
- format version;
- reserved-zero bytes;
- descriptor type;
- descriptor dimension;
- count bounds;
- offset bounds;
- exact total size;
- checked multiplication;
- external SHA-256;
- Project DB metadata consistency.

A future format version is distinguished from corruption when both file and DB record are otherwise
coherent.

The reader uses `pread`, reads at most 256 features per call and does not require loading the entire
Feature File.

The Project DB path must equal the canonical path derived from the stored SHA-256 before the file is
opened.

Outcomes include:

- missing file -> `NOT_FOUND`;
- truncation -> `CORRUPT`;
- hash mismatch -> `CORRUPT`;
- header/DB disagreement -> `CORRUPT`;
- coherent unsupported future version -> `UNSUPPORTED_VERSION`.

## Publication and persistence

Physical layout:

```text
assets/features/<first-2-hex>/<full-lowercase-sha256>
```

The SHA-256 covers the complete Feature File.

The publication protocol is:

```text
local temporary file
-> write
-> fsync file
-> hash complete Feature File
-> atomic no-overwrite link/publication
-> fully validate any concurrently existing identical asset
-> fsync parent directory
-> SQLite transaction
-> logical READY state
```

If the final parent-directory `fsync` fails after publication, durability is recorded as
`PUBLISHED_NOT_DURABLE`.

If SQLite fails after the physical file is published, the valid file remains an orphan and no partial
logical Feature Set row is invented.

A later exact retry may revalidate the physical asset, complete a successful directory `fsync` and
promote durability to `DURABLE` in the Project DB transaction.

## Historical Project DB Feature foundation

The Feature Store persistence foundation separates:

- `feature_assets`;
- `feature_sets`;
- `feature_extract_tasks`.

Logical uniqueness is based on image, extractor kind, extractor version and parameter fingerprint.

The historical `features.extract/1` Task is persisted before enqueue, admitted by Queue/Governor,
executes one complete image and checkpoints at its durable boundaries.

Its historical conservative execution shape includes:

```text
one image
one atomic ORB extraction/publication
no intra-image restart
```

The existing conservative single-image resource descriptor remains valid for those Tasks and their
restart compatibility. It must not be generalized into a rule requiring all future independent images
to execute serially.

Pause and cancellation are cooperative before and after the OpenCV call. The OpenCV extraction itself
is not interruptible.

The managed source image is rehashed before extraction.

A uniform image may legitimately publish a READY Feature Set with zero features.

## Selected-execution Feature batch — Project DB v25

Project DB v25 adds only:

```text
feature_extract_batch_tasks
```

and the durable Task Kind:

```text
features.extract.batch/1
```

The v24 -> v25 migration is transactional, additive and DDL-only.

It does not:

- create Feature Sets;
- convert historical `features.extract/1` Tasks;
- infer an `image_id`;
- infer a Feature Set;
- change ORB v1 parameters;
- change Feature File v1/v2;
- change Feature Set identity.

### Durable owner identity

One `features.extract.batch/1` owner is bound to:

- one immutable selected execution;
- one monotonic `next_item_index`;
- the exact ORB extractor kind;
- the exact ORB extractor version;
- the exact ORB parameters;
- the exact ORB parameter fingerprint.

The selected execution order is authoritative.

### Execution shape

The current operational path is:

```text
one admitted owner Task
-> bounded selected-item window
-> bounded independent image participants
-> each participant prepares one per-image Feature result
-> participants perform no SQLite publication
-> join
-> owner validates or reuses the exact READY Feature Set
-> owner publishes in selected-item order
-> owner advances the durable Feature cursor
-> generic Task progress/checkpoint follows
```

Each participant therefore computes an ordinary per-image scientific Feature result.

The owner-only publication stage preserves deterministic selected-item order without requiring
independent image preparation to run serially.

### Atomicity and concurrency

The scientific atomic result remains one Feature Set for one image.

```text
PER_IMAGE_FEATURE_RESULT=ATOMIC
PER_IMAGE_ATOMICITY_REQUIRES_CROSS_IMAGE_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO
```

Cross-image preparation may be concurrent when the Governor admits a useful bounded window.

This is not a generic DAG scheduler and does not add a second global worker pool. The Task Queue still
owns one active callback; the batch callback contains the bounded participant work.

### Crash and restart

An immutable Feature Set can become durable before the Feature-batch cursor or generic Task checkpoint
advances.

Therefore, after a crash:

```text
durable Feature Set may be ahead of Task checkpoint
```

but:

```text
Task checkpoint must never invent a Feature Set that is not durable
```

Restart revalidates and reuses the exact READY Feature Set and then converges the cursor. It does not
derive identity from path, processing order or Task-local position.

### Resource adaptation

For the selected-execution Feature batch, increasing admitted CPU while the item window remains one
cannot exercise additional independent images.

The current validated resource model may therefore couple CPU and batch growth where required by the
Feature-batch rung contract.

That behavior is Task-specific operational policy, not a universal Governor rule.

The canonical repository policy remains:

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

Preserve the interactive host reserve and all RAM/CPU/IO/GPU safety constraints first. Within that
safe envelope, use the maximum useful validated execution width.

Per-item atomicity is not evidence that unrelated items must be serialized.

## GPU boundary

Feature Extraction currently remains CPU.

The current production GPU audit classifies Feature Extraction as:

```text
FEATURE_GPU=REJECTED_WITH_MEASURED_OR_IMPLEMENTATION_EVIDENCE
```

On the validated OpenCV 5.0.0 host:

- no usable ORB/SIFT Vulkan/OpenCL extraction seam is validated;
- CUDA is unavailable in the installed OpenCV build;
- no GPU path has proven byte-identical Feature Files for this boundary.

Therefore Feature Extraction receives no production GPU path merely because the host has a GPU.

This does not authorize avoidable CPU serialism. Cross-image CPU parallelism remains valid where its
Task contract has been proven.

## Environment and reproducibility

Lardon3D does not claim ORB Feature File bytes are identical across arbitrary:

- OpenCV versions;
- platforms;
- extractor backends.

Idempotence is defined inside the supported environment and extractor-version contract.

Any change that modifies durable Feature semantics or bytes requires explicit review and, when
necessary, an extractor-version change. It must not silently reuse the old scientific identity.

## Feature File v2 and multi-descriptor support

**IMPLEMENTED v1A.**

ORB continues to write the historical Feature File v1 U8x32 representation.

SIFT and RootSIFT write Feature File v2 F32x128.

### v2 header

Feature File v2 has a 176-byte header.

Its fields include:

- magic;
- explicit version;
- header size;
- feature count;
- descriptor dimension;
- descriptor type;
- scalar size;
- keypoint-record size;
- decoded image dimensions;
- capabilities;
- keypoint/descriptor block offsets;
- total size;
- source SHA-256 at offset 72;
- parameter fingerprint at offset 104;
- 16-byte extractor kind at offset 136;
- extractor version at offset 152;
- twenty reserved zero bytes.

Stable descriptor type values are:

```text
U8  = 1
F32 = 2
```

with scalar sizes 1 and 4.

Every binary32 value is explicitly encoded little-endian; persistent output is not a C/C++ struct
dump.

Writer and reader reject NaN and Inf.

The typed descriptor readers:

- use `pread`;
- refuse incompatible descriptor type;
- return at most 256 features per call.

File-size limits remain:

```text
Feature File v1  16 MiB
Feature File v2  64 MiB
```

The extraction facade also carries `descriptor_bytes`.

Publication requires exactly:

```text
feature_count * descriptor_dimension * scalar_size
```

and rejects both truncated and oversized descriptor buffers before file creation/publication.

The detailed multipass, grid and RootSIFT scientific contracts remain in
[`precision_feature_pipeline.md`](precision_feature_pipeline.md).

## Downstream consumers

The Feature Store currently feeds implemented downstream stages:

```text
Feature Store
    |
    v
Visual Index
    |
    v
Candidate Pair
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

Visual Index consumes the bounded Feature reader without changing Feature File format.

Candidate Pair consumes Feature Set identities indirectly through Visual Index membership.

Matcher consumes persisted Feature Sets and their descriptors.

Geometric Verification and Tracks consume later correspondence products; they do not rewrite Feature
Store identity.

Sparse SfM Gates C through G are implemented and PASS/FROZEN, but real known-calibration Sparse SfM
was not executed in the retained historical S21/A6000 campaigns.

## Real A6000 evidence

The retained real A6000 selected execution contains:

```text
Feature Sets       689
Candidate Pairs    38,420
Match Results      38,420
```

The v25 selected Feature-batch path completed all selected images.

The retained checkpoint is:

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

The later GV/Tracks continuation required:

```text
Feature replay      0
Candidate replay    0
Matcher replay      0
```

and continued from retained immutable upstream products.

This proves the current Feature-batch operational path and restart/reuse boundary on that campaign.
It does not create calibration for that campaign and does not constitute real Sparse SfM execution.

## Current unfinished work

The following remain separate from the Feature Store scientific contract:

- global orphan-file reconciliation/scrub;
- EXIF orientation handling where still required;
- generic dependency/DAG scheduling;
- future extractor/backend work only after explicit equivalence/version review.

Multi-image Feature execution itself is no longer an unfinished item:
`features.extract.batch/1` is implemented and validated.

## Summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25

FEATURE_STORE_STATUS=IMPLEMENTED

FEATURE_FILE_V1=FROZEN
FEATURE_FILE_V2=IMPLEMENTED

HISTORICAL_FEATURE_TASK=features.extract/1
CURRENT_FEATURE_BATCH_TASK=features.extract.batch/1
PROJECT_DB_V25_FEATURE_BATCH=IMPLEMENTED/VALIDATED

FEATURE_RESULT_IDENTITY=PER_IMAGE
PER_IMAGE_FEATURE_RESULT=ATOMIC
PER_IMAGE_ATOMICITY_REQUIRES_CROSS_IMAGE_SERIALISM=NO
OWNER_ONLY_PUBLICATION_REQUIRES_SERIAL_PREPARATION=NO

FEATURE_GPU=REJECTED_WITH_MEASURED_OR_IMPLEMENTATION_EVIDENCE

RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL

REAL_A6000_FEATURE_SETS=689
REAL_A6000_PRE_SFM=PASS/FROZEN
```
