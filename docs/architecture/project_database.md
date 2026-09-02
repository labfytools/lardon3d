# Lardon3D Project Database

## Authority and current state

This document defines the persistent Project Database architecture and the durable contracts that
span schema versions. Historical sections remain authoritative for the version they describe, but
present-tense statements in this document use the current schema head.

```text
CURRENT_PROJECT_DB_SCHEMA=v25
```

The current head is additive:

```text
v22  Selected scientific execution foundation                 PASS / FROZEN
v23  Generic optical-context overlay                          IMPLEMENTED / VALIDATED / REVIEWED
v24  raw.develop.batch/1 persistence                          IMPLEMENTED / VALIDATED
v25  features.extract.batch/1 persistence                     IMPLEMENTED / VALIDATED
```

Project DB v25 preserves the scientific and persistence meaning of every earlier retained row. No
migration from v22 through v25 backfills scientific identity, infers camera/lens identity, invents
calibration, rewrites Capture identity, or changes historical Task payload interpretation.

The current real A6000 pre-SfM proof completed the v24 RAW-batch and v25 Feature-batch paths before
Geometric Verification and Track publication. Therefore v24 and v25 are not validation-in-progress
states.

## Database role

Project DB stores reconstruction metadata, durable execution payloads, immutable scientific
identities, provenance relationships and restart state. Large binary scientific artifacts remain
outside SQLite and are referenced through validated identities and paths.

The database is designed to remain:

- persistent;
- bounded in memory access;
- restartable;
- transactionally consistent;
- explicit about identity;
- independent from runtime-only resource reservations;
- compatible with sequential additive migration.

Project DB never persists pthread objects, callbacks, pointers, active Resource Reservations, worker
objects, ncurses state or other process-local runtime state.

## Identity invariants

The following identities are distinct unless a specific FROZEN contract explicitly relates them:

```text
Capture != file
Capture != Asset
Capture != image_id
Capture != SHA-256
Capture != path
Capture != basename
Capture != Task ID
Capture != campaign group ID
Task ID != scientific acquisition identity
campaign group ID != Capture identity
```

Specific meanings:

- SHA-256 identifies immutable asset bytes.
- `capture_id` identifies one physical acquisition representation in Project DB.
- `image_id` identifies one scientific image representation.
- `task_id` identifies durable execution work.
- campaign `group_id` identifies one stable operational group inside a campaign request.
- optical body, lens, optical configuration and calibration are separate identities.

No migration may recover or manufacture Capture identity from path, digest, basename, timestamp,
metadata, `image_id`, Task ID or campaign group ID unless an explicit future contract defines that
identity.

## Current additive head

### Project DB v23 - generic optical-context overlay

**IMPLEMENTED / VALIDATED / REVIEWED.**

The v22 -> v23 migration is transactional and additive. It creates nine empty tables:

- `camera_body_profiles`;
- `camera_body_aliases`;
- `lens_profiles`;
- `lens_profile_aliases`;
- `optical_configurations`;
- `acquisition_campaign_group_optics`;
- `capture_optical_configurations`;
- `optical_calibration_profiles`;
- `capture_calibration_selections`.

A camera body, lens, optical configuration and calibration are four separate identities. An optical
configuration binds exactly one body, one lens and an optional integer focal length in micrometers.

A manual lens without EXIF is a normal supported case. Its explicit lens profile requires no metadata
alias. The Meike fixture proves this generic path and does not introduce any brand- or model-specific
product branch.

Metadata aliases, when present, are exact `BINARY` make/model pairs. They are never fuzzy lookup,
case folding, spelling correction or implicit identity evidence.

A campaign may assign a different optical configuration to each group before materialization. When
S3-E returns a Capture, the same transaction retains the group-to-Capture mapping, copies the
campaign optical assignment and advances the campaign cursor. A late rollback reverts all three
mutations.

An explicit post-import optical assignment is allowed only for a Capture that is still unassigned.
Absence remains `NOT_FOUND`; no synthetic "unknown" profile is created.

A calibration profile references one existing immutable sparse calibration and exactly one optical
configuration. It does not copy, recompute, interpolate or fabricate coefficients. Compatible lists
filter only by exact optical-configuration identity.

Calibration selection on a Capture is always explicit and requires an exact configuration match.
Multiple compatible profiles remain ambiguous until one is selected. Exact retries converge. A
valid durable conflict returns `CONSTRAINT`; malformed persisted state returns `CORRUPT` before
comparison with an alternative request.

The v22 -> v23 migration does not inspect EXIF, path, basename, SHA-256, dimensions, device name or
historical calibration. `schema_version=23` is the durable publication point. Failure while creating
the overlay or updating a marker that does not target exactly v22 rolls the migration back.

### Project DB v24 - bounded selected RAW-batch persistence

**IMPLEMENTED / VALIDATED.**

The v23 -> v24 migration adds only:

```text
raw_development_batch_tasks(task_id, selected_execution_id)
```

`task_id` references generic Task state. `selected_execution_id` is unique. The row does not copy a
Capture, Asset, image, path, campaign group or selected-execution cursor.

The migration is transactional and DDL-only. It creates no historical association, and the v24
schema marker is its publication point.

`raw.develop.batch/1` reloads the durable `selected_executions` cursor for every admitted window.
Independent RAW items may be prepared concurrently by bounded participants. After all participants
join, only the owner Task publishes selected representations, in increasing selected-item order.

The durable ordering is:

```text
bounded independent RAW preparation
-> join all participants
-> owner-only selected representation publication
-> selected item + selected cursor commit
-> generic Task progress/checkpoint
```

Therefore a crash may leave generic Task progress behind durable selected execution state, but never
ahead of it. Restart resumes from durable selected state without guessing the identity of an already
published RAW-derived representation.

The historical `raw.develop/1` path remains valid. v24 does not modify RAW Policy v1, `L3DRAWD1`,
Capture identity, image identity or calibration.

The retained real A6000 proof completed this RAW-batch path.

### Project DB v25 - bounded selected Feature-batch persistence

**IMPLEMENTED / VALIDATED.**

The v24 -> v25 migration adds only `feature_extract_batch_tasks`.

A row binds one durable `features.extract.batch/1` Task to:

- one immutable selected execution;
- a monotonic `next_item_index` prefix;
- the exact ORB extractor kind/version/parameters/fingerprint domain.

The migration is transactional and DDL-only. It creates no scientific rows, converts no historical
`features.extract/1` Task, and infers no `image_id` or Feature Set.

Selected images may be prepared concurrently by bounded CPU participants without SQLite access.
After every participant joins, the owner publishes Feature Sets in selected order. An item advances
only after the exact READY Feature Set is durable; generic Task progress/checkpoint follows.

A crash may therefore leave generic progress or the Feature-batch cursor behind an immutable Feature
Set that is already durable. Restart revalidates that exact result and converges without inventing
identity.

ORB U8x32 format, keypoints, descriptor semantics, Feature Store identity and fingerprints remain
unchanged.

The retained real A6000 proof completed the v25 Feature-batch path and preserved:

```text
Feature Sets       689
Candidate Pairs    38,420
Match Results      38,420
```

No acquisition, RAW development, Candidate generation or Matcher replay was required during the
final A6000 GV/Tracks continuation.

## Optical TUI workflow

The optical TUI uses bounded public APIs and does not write SQLite directly. It can:

- inspect a Capture;
- resolve exact metadata aliases;
- page through bodies, lenses, configurations and compatible calibrations;
- create immutable body/lens/configuration profiles;
- explicitly assign campaign-group or Capture optics where the persistence contract permits;
- explicitly select an exactly compatible calibration.

A manual lens without electronics or metadata aliases is normal data. A lookup with no exact match
remains unresolved.

Absence, multiple candidates, incompatibility, `BUSY`, I/O failure and corruption remain explicit.
The TUI never fabricates an "unknown" profile, compatibility relationship or identity.

The TUI model borrows Project DB until unbind and must be detached before project close. See
[runtime.md](runtime.md).

## Project DB v22 - selected scientific execution snapshot

**PASS / FROZEN.**

The v21 -> v22 additive migration adds:

- `capture_source_assets`;
- `selected_executions`;
- `selected_execution_items`;
- `raw_development_tasks`.

`capture_source_assets` retains the explicit relation published by S3-E between a Capture, one
`SOURCE` Asset and the already validated JPEG/RAW source kind. Publication of source association and
source kind is transactional. Exact retry converges; a different source kind conflicts.

Captures migrated from v21 remain honestly unmapped. Migration never derives a mapping from path,
name, SHA-256, attachment order or logical image.

An ordered selected-execution snapshot explicitly retains:

```text
quality_task_id/group_id
-> campaign_task_id/group_id
-> capture_id
```

The quality and campaign `group_id` namespaces remain independent. Numeric equality between them is
never identity.

Both Tasks must target the same ScanSet, triage must be complete and every retained result must be
included at snapshot creation. Snapshot contents and order are immutable afterward.

Each selected item receives an `image_id` only through an already durable explicit `capture_images`
association. Representation publication and selected cursor advance are transactional. Exact retry
converges; a different `image_id` conflicts.

After the final item, an existing `calibration_scope_id` may be attached only when every retained
image belongs to that scope. Terminal `READY` is exactly:

```text
QUALITY_SELECTED
INTERSECT REPRESENTATION_READY
INTERSECT CALIBRATION_ASSIGNED
```

No step infers Capture from Asset, SHA-256, path, basename, Task ID, group ID or `image_id`.

The snapshot also freezes the representation source. An A6000 selected item explicitly retains the
RAW `asset_id` already associated as `SOURCE`. An item using a source image explicitly retains the
absence of RAW identity. This discriminant/Asset pair is immutable and part of exact retry.

The selected execution is bounded to 4096 items and reserves no execution resources by itself.

Calibration Bootstrap v1 validates a bounded calibration artifact against this snapshot, creates or
reuses immutable calibrations/scopes and attaches the result through v22 persistence. It does not
extend the schema, solve calibration or perform RAW development.

### Durable single-RAW Task

The RAW developer also exposes a bounded entry point keyed by explicit `source_asset_id`. It
requires the Capture/SOURCE-RAW relation, resolves managed bytes through that Asset ID, verifies the
bytes against persisted SHA-256, then reuses RAW Policy v1 unchanged.

`raw_development_tasks` stores exactly:

- `task_id`;
- `capture_id`;
- `source_asset_id`;
- monotonic phase `PENDING -> PUBLISHED`;
- the derived `image_id` when published.

Generic Task snapshot, checkpoint reference and typed RAW payload are written transactionally.
Immutable Asset/image publication may precede the generic checkpoint if the process dies; exact S3-B1
retry converges by content and publishes the phase/image identity without guessing.

Persistent reads reject malformed SQLite storage classes, phases, nullability or inconsistent
Capture/RAW/image relations as durable corruption.

The v22 persistence boundary passed its retained normal, C17, targeted ASan/UBSan and review
evidence. The full sanitizer context retains the documented third-party RADV/LSan qualification.
This freeze concerns persistence, not real-camera calibration availability.

## Project DB v21 - Photo Quality Triage

**PASS / FROZEN.**

The v20 -> v21 migration adds `photo_quality_triage_tasks` and
`photo_quality_triage_results` without changing v20 Capture/Asset/image identities.

Each result retains canonical plan `group_id` in `1..N`. `next_group_id` uses the same one-based
identity:

```text
initial       1
after group k k + 1
terminal      N + 1
```

A private `group_index = group_id - 1` may exist inside the executor but is not durable identity.

Result and cursor become durable atomically before generic Task checkpoint. Reads validate SQLite
64-bit storage class, sign, range and relationships before narrowing to public C fields and require
the exact `photo_quality.triage/v1` dispatch.

Measured recommendation and explicit human override remain distinct.

## Project DB v20 - durable acquisition-campaign execution

**PASS / FROZEN.**

The v19 -> v20 migration adds:

- `acquisition_campaign_tasks`;
- `acquisition_campaign_captures`.

It does not change v19 Capture/Asset identities.

`acquisition_campaign_tasks` has a one-to-one relation to generic Task through `task_id`, the
operational campaign identity. Creation and checkpoints transactionally retain generic Task snapshot,
checkpoint reference and typed campaign payload.

The campaign record stores:

- `scanset_id`;
- historical `next_group_id`;
- group count;
- immutable request bytes.

Despite its historical name, campaign-table `next_group_id` is a zero-based next-work position in
`0..N`; its value equals the number of one-based groups already retained.

An upsert may advance the cursor only when ScanSet, group count and immutable request bytes are exact
matches.

The request v1 codec is bounded and deterministic with explicit magic/version, fixed-width
little-endian integers, validated lengths/enums/indices and no trailing bytes. It persists explicit
confirmations as `CALLER_EXPLICIT`, never as inferred `STRONG`.

Plan groups keep one-based IDs. For every completed group,
`acquisition_campaign_captures` retains the unique relation:

```text
(task_id, group_id) -> capture_id
```

A Capture cannot represent two groups of the same campaign Task. Valid durable state contains exactly
the mapping prefix already retained, with no holes or mappings ahead of the cursor.

After S3-E returns, one transaction retains the current group mapping and advances the campaign
cursor before generic Task progress/checkpoint. Restart can then pass the retained `capture_id` back
to S3-E without rehoming or inference.

Malformed storage class, bounds, dispatch kind/version or persistent relationship returns `CORRUPT`
before mutation and never creates a false `resume_capture_id`.

Each Task sequence materializes one group. Pause/cancel are cooperative at group boundaries.
A non-terminal group sets `sequence_break`, releases admission and requires re-admission by the
existing Queue and Resource Governor.

The registry reconstructs the Task from Task ID and immutable request at project open. No parallel
runtime, Queue, Governor, Scheduler or persistence subsystem is introduced.

The accepted residual crash window is strictly after S3-E has created/returned a Capture but before
the campaign transaction has retained that Capture. In this window Project DB deliberately does not
infer `capture_id`; exactly-once is not claimed for that group.

## Project DB v19 - Capture / Asset Provenance v1

**PASS / FROZEN.**

The v18 -> v19 migration adds:

- `captures`;
- `capture_images`;
- `capture_assets`;
- `capture_selections`;
- `asset_derivations`.

A Capture belongs to one ScanSet. It is not an `image_id` and does not redefine scientific image
identity.

Source and derived Assets may be attached without creating a logical image. An optional current
selection references exactly one image already attached to the Capture and never mutates the image,
its Asset or prior scientific results.

Explicit attachment of an existing `SOURCE` Asset to an existing Capture is idempotent. The same
association is accepted without a new row or migration. It creates no logical image, changes no
selection and performs no automatic pairing.

`asset_derivations` intentionally represents one parent Asset and one child Asset with kind/version
and canonical 32-byte fingerprint. It is not a generic DAG and does not perform RAW development or
video extraction.

The v18 -> v19 legacy migration creates one independent legacy Capture per image in increasing
`image_id`, attaches that image Asset as source and selects the same image. It does not interpret
filename, EXIF or sibling relationships.

### S3-E - Multi-Source Ingestion v1

**PASS / FROZEN.**

S3-E receives one explicit ScanSet and at most 64 caller-supplied source paths. It never scans a
directory.

Every source is first published as an immutable managed Asset. S3-D evidence is then extracted from
those published bytes.

Automatic grouping creates a common Capture only for a unique mutual `SAME_ACQUISITION_STRONG`
relationship. Filename, path, SHA-256, timestamp, model, weak evidence, contradiction or ambiguity is
never Capture identity.

Duplicate Assets inside one request are rejected deterministically.

The caller may provide explicit groups. Their provenance is `CALLER_EXPLICIT`, distinct from
scientific `STRONG`. This basis is an operation result, not a new durable identity column.

Sibling relations are persisted through S3-C. A camera JPEG remains `SOURCE` and may become a
logical image of the Capture through transactional source-image publication. RAW remains `SOURCE`;
its logical image is produced only through deterministic S3-B1 derived publication.

RAW and JPEG from one acquisition do not create separate Captures. Selection changes only when the
caller explicitly requests the selected representation.

Once a Capture ID has been returned or durably retained, restart supplies that
`resume_capture_id`. Publication and attachments then converge without rehoming or duplicate logical
objects.

Before the caller durably retains that ID, v19 does not claim whole-request exactly-once and never
infers restart identity from SHA, path, basename, timestamp, metadata or `image_id`.

### S3-D - Acquisition Pairing Evidence v1

**PASS / FROZEN.**

S3-D extracts a fixed bounded metadata evidence set from immutable source Assets.

- RAW metadata extraction uses LibRaw without pixel decoding.
- JPEG metadata extraction uses libexif.
- Evidence is not scientific identity and does not modify existing identity semantics.

Only a known, non-empty, exactly shared camera `ImageUniqueID` is strong evidence eligible for
automatic association between two distinct Assets.

Timestamp, make/model, matching body serial number, dimensions/exposure and basename are weak or
corroborating only. Known conflicting `ImageUniqueID` or known conflicting body serial numbers prove
that the Assets differ. Missing values are not conflicts. Ambiguity is never resolved by input order.

S3-D performs no DB write and no schema change. S3-C remains the persistence primitive; S3-E consumes
S3-D evidence without redefining it.

### Bounded discovery and campaign planning

**PASS / FROZEN.**

The bounded discovery layer accepts 1..64 explicit absolute lexically normalized roots. It does not
recurse. Each root and its immediate regular entries are inspected without following symlinks.

Supported discovery suffixes are `.arw`, `.jpg` and `.jpeg`, case-insensitive. Discovery sources,
groups and proposals are each bounded to 4096 elements. Paths use deterministic `strcmp` ordering.

Planning runs S3-D metadata-only. It does not decode pixels, write Project DB or materialize Assets
or Captures.

Automatic association requires one unique mutual strong relationship. A shared filename stem is only
a proposal. Contradictions, ambiguities, insufficient comparisons and unresolved inputs remain
singletons.

The caller may explicitly confirm groups; confirmed groups are `CALLER_EXPLICIT`.

The plan and its transient progress are caller-owned. The durable campaign identity/restart mechanism
is the v20 Task path above.

The retained A6000 dry-run evidence observed:

```text
ARW sources                     953
JPEG sources                    953
total sources                   1906
metadata OK                     1906
strong groups                   0
stem candidate proposals        953
ambiguities                     0
contradictions                  0
```

The Sony JPEG files were valid MPF containers with one secondary JPEG and zero-only trailing fill.
The dry run did not write Project DB, materialize Captures or develop RAW. The 953 proposals required
explicit `CALLER_EXPLICIT` confirmation.

## Project DB v18 - Phase H v1 incremental reconstruction

**PASS / FROZEN.**

The v17 -> v18 migration:

- adds nullable `derivation_identity` to `sparse_reconstructions`;
- creates `incremental_reconstructions`;
- creates `incremental_reconstruction_tasks`.

Historical Gate F rows keep `derivation_identity IS NULL` and their exact
`parameter_fingerprint`. Their historical candidate tuple remains protected by its partial UNIQUE
index.

Derived Phase H rows retain the true H fingerprint in `parameter_fingerprint` and canonical H
scientific SHA-256 identity in `derivation_identity`, protected by a second partial UNIQUE index.

Transactional reconstruction of the parent and child tables preserves v17 IDs, rows, foreign keys
and cascades without rewriting scientific data.

`incremental_reconstructions` atomically binds one complete Sparse SfM snapshot to:

- its predecessor;
- extension Track Set;
- calibration scope;
- H kind/version;
- H fingerprint;
- unique H scientific identity.

`incremental_reconstruction_tasks` stores the immutable durable H Task payload.

No Resource Reservation state, resource snapshot, partial geometry, observation subset, component
remap or dependency graph is persisted. Restart recomputes from immutable inputs.

Phase H publication reuses the full-snapshot structural validator. Geometry and H metadata are
inserted in one transaction; late metadata failure rolls back the new geometry as well.

## Project DB v17 - Gate F durable Sparse SfM Task

**PASS / FROZEN.**

The historical v15 -> v16 Sparse SfM persistence model remains unchanged. v16 -> v17 adds exactly one
typed durable Task table: `sparse_sfm_tasks`.

One row per `task_id` references generic `tasks(task_id)` with cascade deletion and retains:

- immutable `track_set_id`;
- immutable `calibration_scope_id`;
- `sfm_kind`;
- `sfm_version`;
- the 27 effective `Lardon3DSparseIncrementalParameters` values.

Finite floating-point values are stored as SQLite `REAL` and must round-trip exact binary64 bits.
Canonical C-width integers preserve their defined domains.

Task kind/version remain in `tasks` and version interpretation of the typed payload. The F0
fingerprint is derived on reload and is not duplicated.

Generic Task summary and typed payload are written in one transaction. Missing, incompatible or
invalid typed state prevents runtime reconstruction; defaults are never silently substituted.

The four full-domain `uint64_t` fields for observation/Track limits and relative-pose/PnP seeds are
stored as exact eight-byte little-endian BLOBs.

Global persisted metrics are Gate F diagnostics over the final retained Gate E observations. They do
not participate in scientific identity.

Gate F publication persists a component exactly when registered-image count and landmark count are
both positive. A component whose BA was rejected but whose Gate D geometry remains valid is
persisted unchanged; an unreconstructed graph component is omitted.

## Project DB v16 - Sparse SfM persistence model

**PASS / FROZEN.**

Sparse SfM v1 is atomically published through:

- `sparse_calibrations`;
- `sparse_calibration_scopes`;
- `sparse_calibration_scope_images`;
- `sparse_reconstructions`;
- `sparse_reconstruction_components`;
- `sparse_registered_images`;
- `sparse_landmarks`;
- `sparse_landmark_observations`.

Results are immutable. Large collections are accessed through bounded cursors; Project DB does not
load a complete reconstruction by default.

Coordinates remain in the arbitrary frame of each component. Observation references do not duplicate
descriptors or pixel coordinates.

Child indexes support foreign-key deletion/validation paths, including calibration members and
reconstruction calibration scopes.

Gate C adds pure numerical primitives outside Project DB and creates no extra v16 table or identity.

## Project DB v15 - durable Track Builder payload

The v14 -> v15 migration adds only `track_builder_tasks`. It changes no Track Model table and no
scientific identity.

The row references an atomically published scope file:

```text
.lardon3d/checkpoints/<task_id>.scope
```

The row retains size, SHA-256, format, exact selector, Builder fingerprint, `gvr_count` and
`input_scope_hash`.

The scope file contains `L3DTSCP1`, explicit version, ID count and sorted unique little-endian
`uint64` IDs. Restart replays from the beginning; no cursor may discard transient graph edges.

Reconstruction validates format, size, checksum, bounds, ordering, uniqueness, selector, fingerprint
and `L3DTSIS1` before creating a new callback. Corruption makes the Task unreconstructable without
creating a Track Set.

## Project DB v14 - Track Model v1

The v13 -> v14 migration creates:

- `track_sets`;
- `tracks`;
- `track_observations`.

The executable SQL remains canonical in `src/project_db.c`; the complete scientific contract is in
[tracks.md](tracks.md).

Core persistent invariants:

```text
Track Set identity:
(builder_kind, builder_version, parameter_fingerprint,
 verifier_kind, verifier_version, verifier_fingerprint,
 input_scope_hash)

track observation primary identity:
(track_set_id, feature_set_id, feature_index)
```

SQL and API jointly enforce:

- one observation occurrence per Track Set;
- `observation_count >= 2`;
- cascade Track/Track Set deletion semantics;
- unique `position_in_track` per Track;
- referenced Feature Set existence;
- Track observation parent/set consistency;
- at most one observation from one image in a Track;
- `feature_index < feature_count`;
- exact Track/observation counts.

Track identities are immutable after publication.

## Project DB v13 - durable Geometric Verifier Task

The v12 -> v13 migration adds only `geometric_verifier_tasks`.

It stores:

- `after_match_result_id`;
- the seven v1 scientific parameters;
- the verifier control fingerprint.

UPSERT may advance only the cursor. Configuration remains immutable. Transactional migration failure
leaves a true v12 and exact retry converges.

## Project DB v12 - Geometric Verification Result Model

The v11 -> v12 migration adds only `geometric_verification_results`.

Parent must be a `MATCHED` Match Result; cross-row validation remains in the API.

Stable values include:

```text
FUNDAMENTAL=1
GEOMETRIC_REJECTED=1
GEOMETRIC_VERIFIED=2
```

Identity is:

```text
(match_result_id, verifier_kind, verifier_version, parameter_fingerprint)
```

`parameter_fingerprint` is exactly 32 bytes. Inlier-mask representation is canonical and bounded.
REJECTED forbids a model; VERIFIED requires all nine finite 3x3 model values. Both states require
canonical mask size/padding/popcount and `inlier_count <= parent.match_count`.

See [geometric_verification.md](geometric_verification.md) for the complete bit-order contract.

## Project DB v11 - durable Matcher Task

The v10 -> v11 migration adds `matcher_tasks`.

It stores immutable matcher configuration and durable `after_candidate_pair_id`. UPSERT may only
advance the cursor.

The cursor identifies the last checkpointed Candidate Pair ID; it does not imply contiguous IDs or
a persisted Candidate Pair list.

## Project DB v10 - Match Result Model

The v9 -> v10 migration adds `match_results`.

Persistent scientific identity is the six-part tuple:

```text
(candidate_pair_id,
 feature_set_id_a,
 feature_set_id_b,
 matcher_kind,
 matcher_version,
 parameter_fingerprint)
```

Core invariants:

- Candidate Pair exists.
- Feature Set A belongs to Candidate Pair image A.
- Feature Set B belongs to Candidate Pair image B.
- `NO_MATCH` requires count zero and no match artifact.
- `MATCHED` requires positive count and complete SHA/path/size artifact fields.
- execution failures remain Task Runtime state and create no Match Result row.
- `matcher_kind` text is bounded to 64 characters.
- `parameter_fingerprint` is exactly 32 bytes.

The executable SQL remains canonical in `src/project_db.c`.

## Project DB v9 - durable Candidate Pair Task

The v8 -> v9 migration adds `candidate_pair_generate_tasks`.

The durable payload stores:

- `visual_index_id`;
- non-negative `after_feature_set_id`;
- `top_k` in `1..256`;
- `minimum_evidence_count` in `0..1024`;
- `scanset_filter` in the defined enum domain;
- boolean `exclude_same_asset`.

UPSERT updates only the restart cursor under immutable configuration.

## Project DB v8 - Candidate Pair Model

The v7 -> v8 migration adds `candidate_pairs`.

Persistent invariants:

- `candidate_pair_id` is positive AUTOINCREMENT identity;
- `image_id_a < image_id_b`;
- self-pairs are impossible;
- `(image_id_a, image_id_b)` is UNIQUE;
- `created_at` is non-negative Unix seconds.

The API canonicalizes image order before INSERT, supports exact find/load and paginated listing.

## Project DB v7 - persistent project/runtime foundation

The v7 foundation contains the durable project, Task, checkpoint, catalog, Feature Store and Visual
Index state used by later migrations.

Important tables include:

- `metadata`;
- `project`;
- `tasks`;
- `checkpoints`;
- `artifacts`;
- `scansets`;
- `image_assets`;
- `images`;
- `image_import_tasks`;
- `feature_assets`;
- `feature_sets`;
- `feature_extract_tasks`;
- `sift_extract_tasks`;
- Feature Support tables;
- Visual Index tables and update Tasks.

`metadata` contains the schema version and durable next Task ID state.

`project` stores one stable logical Project identity.

`tasks` stores durable generic Task summary and exact Task kind/version when known.

`checkpoints` references `DURABLE` or `PUBLISHED_NOT_DURABLE` checkpoint publication state.

`artifacts` inventories external files as `STAGED` or `READY`.

`scansets`, `image_assets` and `images` store logical acquisition grouping, immutable content identity
and logical scientific image identity.

`UNIQUE(scanset_id, asset_id)` rejects duplicate content inside one ScanSet without merging distinct
ScanSets.

Feature assets remain external content-addressed files. SQLite stores Feature Set identity,
extractor/version/fingerprint, source hash, descriptor type/dimension/count and lightweight coverage
metrics.

Visual Index postings remain outside SQLite. SQLite stores index identity, segment identity,
membership and durable update cursor.

Published catalog, Feature Store and Visual Index identities use SQLite `AUTOINCREMENT` where the
contract requires non-reuse of committed identities. IDs from rolled-back transactions were never
published and may be reused.

SQLite configuration at this foundation is:

```text
foreign_keys=ON
journal_mode=DELETE
synchronous=FULL
busy_timeout=5000
```

DELETE journal mode matches the current single-owner design, avoids persistent WAL/SHM files and
preserves strong synchronization. The timeout bounds waiting on an external DB lock to five seconds.

## Historical migration ledger

The schema evolves only through sequential additive migrations.

```text
v1 -> v2    Task kind/version columns
v2 -> v3    durable image-import Task payload and next Task ID metadata
v3 -> v4    ScanSets, image Assets/images and legacy catalog marker
v4 -> v7    Feature Store, Feature Support and Visual Index foundations
v7 -> v8    Candidate Pair Model
v8 -> v9    Candidate Pair durable Task
v9 -> v10   Match Result Model
v10 -> v11  Matcher durable Task
v11 -> v12  Geometric Verification Result Model
v12 -> v13  Geometric Verifier durable Task
v13 -> v14  Track Model v1
v14 -> v15  Track Builder durable payload
v15 -> v16  Sparse SfM persistence
v16 -> v17  Sparse SfM durable Task
v17 -> v18  Phase H v1 persistence
v18 -> v19  Capture / Asset Provenance v1
v19 -> v20  durable acquisition-campaign execution
v20 -> v21  Photo Quality Triage
v21 -> v22  selected scientific execution + single RAW Task
v22 -> v23  generic optical-context overlay
v23 -> v24  selected RAW-batch Task persistence
v24 -> v25  selected Feature-batch Task persistence
```

Historical version-specific contracts remain valid for the rows and checkpoints they describe.
An older version number is not stale when the text explicitly describes historical state.

## Opening and migration

An empty Project DB is created and migrated through the complete known chain to v25.

A supported historical DB is migrated sequentially to v25. Each migration is transactional. A
migration failure rolls back both newly created objects and the schema-version marker for that step,
leaving the prior version complete and retryable.

The implementation rejects:

- future schema versions;
- missing/invalid schema-version metadata;
- impossible version/storage-class combinations;
- malformed required durable relationships.

The migration implementation must recognize only the known sequential range through v25. It must not
skip an intermediate contract.

Important rollback properties retained from historical tests include:

- v12 failure leaves true v11;
- v13 failure leaves true v12 without `geometric_verifier_tasks`;
- v14 failure leaves true v13 without Track Model tables;
- v15 failure leaves true v14 without `track_builder_tasks`;
- the same transactional principle covers v16 through v22;
- v23 failure leaves no partial optical overlay and a true v22;
- v24 failure leaves no partial RAW-batch table/marker and a true v23;
- v25 failure leaves no partial Feature-batch table/marker and a true v24.

Exact executable migration SQL is owned by `src/project_db.c`. Documentation may summarize it, but
must not contradict the source or the FROZEN migration tests.

## Task checkpoint boundary

The durable Task model and its checkpoint codec are implemented independently from Project DB.

Project DB stores a queryable logical summary and a checkpoint reference. The validated checkpoint
file remains the complete source for `lardon3d_task_restore()`; SQLite alone never reconstructs a
Task.

DB/checkpoint consistency compares the fields that `tasks` actually stores, including Task ID, name,
saved/recovery state, progress and sequence count. It does not pretend to compare fields that are not
stored in the DB summary.

A summary mismatch or invalid checkpoint prevents recovery.

Checkpoint paths are portable relative paths:

```text
.lardon3d/checkpoints/<task_id>.chk
```

Generic publication ordering is:

```text
write and durably publish .chk.next
-> commit SQLite checkpoint reference/summary
-> under .chk.lock, promote .chk.next to canonical .chk
```

`.chk.lock` is advisory and process-local synchronization, not recovery data.

Project inventory uses its DB page only for discovery. After acquiring `.chk.lock`, it reloads the DB
row because the row may have changed while waiting.

A valid canonical checkpoint whose summary matches DB wins over a stale/corrupt `.next`. If the
canonical file does not match, a valid matching `.next` may be promoted under the lock.

After copying the DB record outside the SQLite mutex, recovery consults the Task Kind Registry and
distinguishes:

- `LEGACY_UNTYPED`;
- `UNKNOWN_TASK_KIND`;
- `UNSUPPORTED_TASK_KIND_VERSION`.

No business reconstructor is called while holding the Project DB mutex.

Project open scans recoverable Tasks in pages of 8 by increasing Task ID. A full Queue window stops
the scan without mutating remaining Tasks. The bounded summary reports `inspected`, `resumed`,
`skipped`, `failed`, recovered `PUBLISHED_NOT_DURABLE` checkpoints and saturation.

Project/schema/identity migration errors are fatal to open. Per-Task legacy/unknown/future kind,
invalid checkpoint, unavailable source or reconstruction failure is non-fatal. SQLite `BUSY` after
the configured timeout stops the scan without a retry loop and leaves the Project open.

## Concurrency and ownership

One opaque Project DB connection is serialized by an internal mutex.

Each composite public operation owns its full transaction. No public transaction remains open across
API calls.

Artifact I/O does not occur under the SQLite mutex. The module verifies caller-published regular
files before setting durable READY state.

Closing Project DB concurrently with an active Project/DB API call is forbidden by owner contract.

Records and strings are copied into caller-provided buffers. No SQLite pointer escapes the call.
Statements are finalized before return.

Bounded list/recovery APIs use caller-owned pages, with the documented maximums for each interface.

## Project integration

`Lardon3DAppState` owns exactly one `Lardon3DProjectDb *` while `project_loaded` is true.

Canonical DB path:

```text
<project_root>/project.db
```

Project create/open opens the DB. `lardon3d_project_close()` closes it exactly once after the owning
runtime has respected the Queue/DB lifetime boundary.

Application shutdown destroys/joins the Task Queue before Project DB close.

`project.ini` v2 stores:

- project name;
- 128-bit hexadecimal `stable_id`;
- `version=2`.

The same stable identity exists in Project DB `project`. Divergence is an error.

A legacy INI without identity adopts an existing DB identity. If no DB exists, one identity is
generated once and the INI is atomically migrated before later opens use it. An existing DB with no
Project row may be initialized only when the INI already carries the identity required by the
migration contract.

## Real-data compatibility evidence

The generic optical overlay was validated on migrated copies of real S21 and A6000 projects with
scientific row counts preserved and the new optical tables empty until explicit use.

The current A6000 Project DB v25 proof retains the current selected scientific pipeline state without
identity rewriting:

```text
Selected images            689
Feature Sets               689
Candidate Pairs            38,420
Match Results              38,420
Applicable GVR             37,805
Verified GVR               10,952
Rejected GVR               26,853
Track Sets                 1
Tracks                     130,714
Track observations         318,944
Sparse SfM Tasks           0
Sparse reconstructions     0
Dense / MVS                0
```

The final continuation reused upstream durable work:

```text
ACQUISITION_REPLAY=0
RAW_REPLAY=0
FEATURE_REPLAY=0
VISUAL_INDEX_REPLAY=0
CANDIDATE_REPLAY=0
MATCHER_REPLAY=0
```

The restart proof traversed existing Match Results, produced no duplicate GVR mapping and reused
Track Set 1.

This evidence proves persistence/restart behavior through Tracks. It does not create calibration for
the historical A6000 campaign and does not authorize Sparse SfM or Dense/MVS execution.

## Deferred persistence integration work

The following items remain outside the acquired Project DB persistence boundary and must not be
misreported as complete:

- autosave wiring on every possible runtime transition;
- UI retry workflow for unavailable sources;
- legacy TUI migration cleanup;
- orphan-file reconciliation and safe scrub;
- Visual Index compaction or a future capacity evolution.

Feature Store and Visual Index v1 themselves are implemented. Visual Index v1 retains the historical
bounded capacity of 256 segments with 16 memberships per segment, for 4096 Feature Sets in one index.
Larger scopes require compaction or an explicitly authorized evolution; this operational capacity
must not be silently reinterpreted as a scientific dataset limit.

## Status summary

```text
CURRENT_PROJECT_DB_SCHEMA=v25

v22 selected scientific execution foundation       PASS/FROZEN
v23 generic optical-context overlay                IMPLEMENTED/VALIDATED/REVIEWED
v24 raw.develop.batch/1 persistence                IMPLEMENTED/VALIDATED
v25 features.extract.batch/1 persistence           IMPLEMENTED/VALIDATED

REAL_S21_TRACKS                                    PASS/FROZEN
REAL_A6000_PRE_SFM                                 PASS/FROZEN
```

Current Project DB opens and migrates supported historical databases through the sequential known
chain to v25.

Historical contracts for Candidate Pair, Matcher, Geometric Verification, Tracks, Sparse SfM,
Phase H, Capture/Asset Provenance, campaign execution, Photo Quality and selected scientific
execution remain valid at the versions where they were introduced.

No current Project DB migration:

- invents Capture identity;
- infers optical identity;
- fabricates calibration;
- turns metadata into scientific calibration;
- converts historical Task kinds into newer Task kinds;
- rewrites historical scientific rows;
- creates a generic dependency DAG;
- persists active Resource Governor reservations.

Future schema changes beyond v25 require explicit human authorization.
