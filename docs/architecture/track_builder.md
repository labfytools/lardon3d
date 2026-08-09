# Track Builder v1

## Status

**GATE A — PASS.** This document is the scientific and algorithmic contract
for the future Track Builder. It specifies no implementation, migration or
Task. `FACT`, `DECISION` and `FROZEN` are intentionally distinguished below.

## Scope

The Builder consumes immutable, pairwise `GEOMETRIC_VERIFIED` evidence and
produces a complete logical Track Set. A Track is a set of coherent 2D
observations; it is not a 3D point. The Builder does not triangulate, estimate
poses, calculate 3D reprojection, or optimise cameras.

## Frozen upstream contracts

The following are **FROZEN** and are consumed without reinterpretation:

- Track Model v1 and Project DB v14.
- Observation identity `(feature_set_id, feature_index)`.
- One observation per image, minimum two observations per Track, and no
  persistent rejected Track state.
- Exact VERIFICATION_SELECTOR
  `(verifier_kind, verifier_version, verifier_fingerprint)`.
- `input_scope_hash = SHA-256("L3DTSIS1" || sorted uint64 little-endian GVR
  IDs)` and `gvr_count`; the scope is DB-local and non-empty.
- GVR status and inlier-mask representation, including LSB-first bit mapping
  to the canonical Match File entry order.
- Match Result, Candidate Pair and Feature Set ownership.
- Atomic complete Track Set publication and exact-identity reuse.

## Definitions

An **observation** is `(feature_set_id, feature_index)`. Its `image_id` is
derived from the immutable Feature Set. An **edge** is a verified relation
between two observations. A **component** is a connected component of the
graph of unique accepted edges. A **Track** is published only when its
component satisfies every Track Model and Builder invariant.

## Scientific input

The orchestration layer is the **input owner**: it supplies one explicit,
canonical, strictly increasing list of GVR IDs. The scientific core owns no
SQLite connection and consumes an immutable representation of that list and
the resolved evidence. It never expands the list to “all GVRs”.

For each listed GVR, the orchestration layer resolves the complete parent
chain:

`GVR → Match Result → Candidate Pair → Feature Set A/B → Image A/B`.

The Match File header must agree with the parent Feature Set IDs and counts.
For every set bit `i`, the Builder reads Match File entry `i` unchanged and
maps it to `(feature_set_id_a, feature_index_a)` and
`(feature_set_id_b, feature_index_b)`. It does not sort entries before mask
application.

## Verification Selector

The selector is exactly the FROZEN Track Model tuple
`(verifier_kind, verifier_version, verifier_fingerprint)`. A GVR is eligible
only when its status is `GEOMETRIC_VERIFIED` and all three fields match
exactly. `latest`, timestamps, greatest IDs and insertion order are forbidden.

## Input Scope

The explicit supplied IDs are sorted numerically, unique, counted, and hashed
with the FROZEN `L3DTSIS1` encoding. The supplied count must equal `gvr_count`.
The selector is not part of `input_scope_hash`; it remains part of Track Set
identity through the verifier fields. A scope is exact, DB-local and non-empty.

## Snapshot semantics

The caller resolves and validates the exact ID list in a short read phase,
then releases the database lock before graph computation. GVRs, Match Results,
Feature Sets and Match Files are treated as immutable published inputs. New
GVRs arriving after resolution are not silently included. Before publication,
the orchestration layer revalidates the exact count, IDs, selector, status,
parent chain, asset identity and streaming scope hash. A missing, extra or
changed item aborts the build; it never becomes a 999-of-1000 partial result.
No long SQLite transaction spans graph computation.

## GVR eligibility

`GEOMETRIC_VERIFIED` plus exact selector match is the complete eligibility
rule. `GEOMETRIC_REJECTED`, Match Result `NO_MATCH`, and runtime failures
produce no Builder edge. A runtime failure is not scientific rejection.

## Observation identity

The only Builder observation identity is the FROZEN pair
`(feature_set_id, feature_index)`. `image_id` is metadata derived for the
one-image invariant, never a replacement identity.

## Edge definition

Edges are **undirected**. Their exact identity is the lexicographically sorted
pair of the two distinct observation identities. Self-edges are invalid input.
The edge carries no scientific weight. Descriptor distance, Lowe ratio,
inlier count, matrix values and `inlier_count / match_count` are not individual
edge quality values.

## Duplicate-edge policy

Exact duplicate edges collapse to one logical edge before component analysis.
Evidence multiplicity is not a vote and does not affect acceptance. Source GVR
IDs may be retained as temporary diagnostic evidence, but edge-level
provenance is not persisted and no duplicate can strengthen a relation.

## Multiple Matcher configurations

**DECISION:** scopes may contain GVRs whose parent Match Results use different
`matcher_kind`, matcher version or matcher fingerprints. This is safe because
the Builder consumes only verified membership relations and never compares
descriptor scores. The parent identity remains available for diagnostics.
The selector selects only the verifier, not a hidden Matcher configuration.
An implementation must still reject malformed parent ownership or asset
metadata; it must not silently substitute another Match Result.

## Feature Set homogeneity

Every published Track component must use one exact Feature Set configuration:
the tuple `(extractor_kind, extractor_version, parameter_fingerprint,
descriptor_type, descriptor_dimension)`. Different Feature Set IDs with that
same tuple are allowed for different images. A component mixing tuples is a
Builder conflict and is unpublished. This is a fixed v1 rule, not a user knob.

## Cross-descriptor semantics

**DECISION: FORBIDDEN.** ORB, SIFT and RootSIFT observations are not fused in a
Track. Their keypoints and descriptor semantics do not establish identical
physical observations merely from 2D proximity, and no such upstream
equivalence contract exists. Cross-descriptor edges are therefore a
heterogeneous component conflict, not an opportunity for spatial merging.

## Graph semantics

Nodes are observations appearing in at least one valid inlier edge. Edges are
unique verified relations. A cycle is valid and contributes membership, not a
requirement to persist every edge. The graph is sparse; no dense image,
observation or co-visibility matrix is created.

## Conflict definition

A component conflicts when either:

1. two observations derive to the same `image_id`; or
2. its Feature Set configuration is not homogeneous under the exact tuple
   above.

Duplicate references to the same observation are not a conflict. A component
with a conflict is not a Track candidate for publication.

## Conflict policy

**DECISION: REJECT WHOLE CONFLICTING COMPONENT.** After all unique edges have
been considered, a component is published only if it has one observation per
image and one Feature Set configuration. Otherwise every membership in that
component is unpublished. There is no `TRACK_REJECTED` persistent state.

This conservative policy does not invent a local winner between contradictory
pairwise evidence. It cannot merge two observations from one image, does not
need incomparable descriptor scores, and has no scientific ranking order.
It can discard a large otherwise useful component after one bad edge; this is
an explicit recall trade-off accepted for v1. A future splitting or weighted
optimisation policy must be a new Builder version and prove equivalence or
declare a new scientific identity.

## Scientific edge ordering

**NONE.** No edge is scientifically better than another. Edges are a set, and
the whole-component policy is evaluated after set construction. Consequently,
DB IDs, GVR IDs, timestamps, feature indices and hashes are never scientific
quality measures.

## Deterministic tie-breaking

Tie-breaking is used only to canonicalize equal logical results. The exact
observation key is `(feature_set_id, feature_index)` in unsigned numeric
lexicographic order. No tie-break changes component acceptance.

## Determinism

The logical result is independent of SQLite row order, GVR enumeration,
pagination, duplicate enumeration, allocation order and future scheduling.
The implementation must canonicalize/deduplicate the complete exact scope
before final component decisions. A future parallel implementation may split
work, but must produce the same set semantics and canonical output.

## Track canonicalization

Within each accepted Track, observations are sorted by
`(feature_set_id, feature_index)` ascending. `position_in_track` is that
sequence starting at zero. DSU roots, pointers, hash-table order and SQL row
order never determine positions.

## Track Set canonicalization

Accepted Tracks are sorted by their ordered observation-membership sequences,
lexicographically: compare observation keys at the first difference, then the
shorter sequence first if one is a prefix. The published Track IDs remain
opaque AUTOINCREMENT IDs; logical tests compare this canonical sequence, not
Track IDs.

## Minimum length and singletons

The minimum is exactly the FROZEN structural minimum of two observations.
Singleton nodes are never produced by an edge component and are dropped if
they arise during future internal conflict handling. No arbitrary minimum of
three and no maximum length is introduced.

## Builder identity

`builder_kind` is conceptually `track_builder` (lowercase ASCII, matching the
project's persisted kind convention). `builder_version` is `1`.

## Builder version

Any change to the scientific conflict, duplicate, descriptor, edge or
canonicalization policy requires a new `builder_version`, unless the change
is explicitly represented by a new parameter fingerprint under the versioning
rules. Runtime, storage or performance changes do not qualify.

## Parameter fingerprint

The fingerprint is SHA-256 of the following 48-byte canonical encoding:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 8 | ASCII domain `L3DTBFP1` |
| 8 | 4 | fingerprint encoding version `1` |
| 12 | 4 | Builder policy version `1` |
| 16 | 4 | whole-component conflict policy `1` |
| 20 | 4 | exact duplicate collapse policy `1` |
| 24 | 4 | mixed Matcher parent configurations allowed `1` |
| 28 | 4 | exact Feature Set tuple homogeneity `1` |
| 32 | 4 | undirected canonical observation edge `1` |
| 36 | 4 | Track/Track Set canonicalization version `1` |
| 40 | 4 | minimum Track length `2` |
| 44 | 4 | reserved, zero |

All integer fields are fixed-width unsigned 32-bit little-endian. There are
no booleans, floats, ABI padding or raw C structs. The hash output is exactly
32 bytes SHA-256. The builder kind and builder version are separate Track Set
identity fields; policy version is nevertheless encoded to make the contract
reviewable.

Excluded runtime-only fields: page size, batch size, thread count, worker
count, RAM budget, CPU/GPU slot, Governor state, PSI, swap, scheduling,
allocation addresses, temporary buffer size, DB transaction duration and
hardware identity.

## Provenance

Persisted provenance is exactly the FROZEN Track Set level: Builder identity,
verifier selector, `input_scope_hash` and `gvr_count`. Edge-to-GVR evidence is
temporary only for deduplication and diagnostics. No new edge provenance table
is required.

## Error ownership

Scientific non-selection means an edge is discarded only by duplicate collapse
or a deterministic conflicting-component result. Runtime error covers I/O,
allocation, cancellation and database failure. Input corruption covers any
invalid published contract. Runtime error or corruption produces no Track Set
publication and never becomes a scientific rejection.

## Corruption handling

The build aborts without publication on: missing GVR, wrong selector, wrong
status, missing Match Result/Candidate Pair/Feature Set/Match File, ownership
mismatch, invalid Match File header or SHA, malformed mask, wrong mask size or
padding, mask/match-count mismatch, out-of-range feature index, missing parent,
inconsistent image ownership, invalid Feature Set metadata, scope hash/count
mismatch, or changed input during final validation. No scope item is skipped.

## Empty input and zero-Track output

An empty scope is invalid because FROZEN Track Model v1 requires
`gvr_count >= 1`; it is an input/constraint error and publishes nothing. A
non-empty, fully valid scope may deterministically produce zero valid Tracks
when every component conflicts or all components are below the structural
minimum. `track_count = 0` is permitted by Track Model v1 and is published as
a complete Track Set with the exact non-empty scope.

## Resource model

**DECISION:** use a full sparse in-memory graph with deterministic edge sort,
not a dense matrix, SQLite temporary graph or external merge in v1. This is
the minimum complexity credible for the 16 GiB target and permits exhaustive
canonical finalization. Input Match Files are read one at a time; they are
never all resident.

Approximate implementation-neutral accounting, including allocator/alignment
headroom:

- observation key: 16–24 bytes;
- unique edge: 16–32 bytes for two compact node references and sort metadata;
- DSU/component node: 16–32 bytes;
- image-membership metadata: 8–24 bytes per component observation;
- temporary evidence: 0–24 bytes per raw edge, bounded and discardable after
  deduplication;
- canonicalization entry: 16–32 bytes per accepted observation.

Indicative totals, not new contractual limits:

- 100k observations: roughly 5–15 MiB plus edges and allocator overhead;
- 1M observations: roughly 50–150 MiB before unusually dense edge evidence;
- 1M unique edges: roughly 20–60 MiB, plus node/component metadata.

The representation remains feasible on the target Ryzen 7 8845HS / about
16 GiB RAM, subject to future measurement. No dense matrix is used.

## Complexity

For `V` unique observations, `E_raw` inlier edges and `E` unique edges:

- input DB/asset I/O: `O(gvr_count + E_raw)` bounded reads;
- edge canonicalization and sort: expected `O(E_raw log E_raw)`;
- duplicate collapse: `O(E_raw)` after sort;
- component construction: `O(V + E)` with a sparse DSU or equivalent;
- conflict detection and membership canonicalization: `O(V log V)` worst case;
- Track Set ordering: `O(V log V)` worst case, with comparison cost included;
- total expected: `O(E_raw log E_raw + V log V)`;
- worst-case memory: `O(V + E_raw)`; no `O(image_count²)` or `O(V²)` storage.

Paged enumeration is deterministic but does not change the asymptotic sort
cost. An external deterministic merge may be introduced only if measurements
show the chosen memory envelope insufficient; it must preserve the same
logical output.

## Publication boundary

The conceptual split is:

1. DB adapter resolves and validates the exact immutable scope and parent
   evidence.
2. SQLite-free scientific core builds the complete canonical logical result.
3. Track Model API validates Feature Set ownership, image uniqueness, bounds,
   counts and publishes the complete set in its short atomic transaction.

No Track, observation or partial set is visible before commit. The frozen
`lardon3d_project_db_create_track_set()` API is the publication boundary.

## Reuse

Before reading Match Files, the orchestration layer performs exact identity
lookup using builder identity, Builder fingerprint, verifier selector and
input scope hash. A found set is reusable only if its stored `gvr_count`
matches the supplied count and the loaded set is coherent. No latest-result
selection and no overwrite are allowed.

## Crash semantics

A crash after compute and before publication leaves no partial scientific
result. Retry recomputes the exact scope in v1. A crash during the frozen
publication transaction rolls back; if commit happened, exact identity lookup
reuses the complete set. Existing immutable upstream data is untouched.

## Incrementality

**FULL REBUILD v1: YES.** Track Sets are immutable. New GVRs require a new
scope hash and a new Track Set identity; the old set remains valid. No delta
Builder or checkpoint graph persistence is introduced here.

## Future Task boundary

A future Task, likely named `track_builder.run`, treats one complete Track Set
as its scientific unit. It may page GVR resolution and pause at safe input or
compute boundaries, but publication occurs only after complete canonical
output and final scope validation. A cancelled or failed run publishes no
incomplete set. The exact Task kind is not implemented or persisted by Gate A.

## Future Resource Governor integration

The future Task estimates bounded graph memory and CPU, obtains reservations
before callbacks, and may reduce batches or pause under pressure. Batch size,
thread count, Governor state, PSI, swap and throttling may affect throughput
only; they must not affect logical edges, conflict decisions or output order.

## Vulkan

**NOT_JUSTIFIED.** The Builder is sparse graph construction and conflict
handling, expected to be CPU/memory-bound. No GPU identity or backend is part
of v1.

## Adversarial corpus

The following expected outputs use `T{...}` for canonical observation lists.
An edge shown as discarded is not persisted; “dropped observations” means
members of an unpublished component.

| Case | Input | Conflict? | Expected tracks | Discarded edges | Dropped observations / why |
|---|---|---|---|---|---|
| 1 chain | A1-B1, B1-C1 | No | T{A1,B1,C1} | none | none |
| 2 cycle | A1-B1, B1-C1, C1-A1 | No | T{A1,B1,C1} | none | none; cycle is valid |
| 3 duplicate | A1-B1 twice | No | T{A1,B1} | duplicate copy | none; multiplicity is not weight |
| 4 same image | A1-B1, B1-C1, A1-C2 | Yes | none | none after collapse | all four; C1/C2 conflict |
| 5 late conflict | A1-B1-B2-C1 then A1-C2 | Yes | none for component | none | whole component rejected |
| 6 disjoint | A1-B1; C1-D1 | No | T{A1,B1}, T{C1,D1} | none | none |
| 7 bridge | A1-B1; C1-D1; B1-C2 | Yes | none for bridged component | none | all bridged members; duplicate image C |
| 8 duplicate GVR | same A1-B1 in GVR1/GVR2 | No | T{A1,B1} | duplicate evidence | none |
| 9 permutation | prior graph, shuffled | same | same canonical tracks | same | order irrelevant |
| 10 single edge | A1-B1 | No | T{A1,B1} | none | none; length 2 is valid |
| 11 singleton | one node after hypothetical split | n/a | none | policy-local | singleton dropped; no rejected row |
| 12 two Feature Sets | A:X1-B:Y1, B:Y1-C:X2 | Yes | none | none | heterogeneous Feature Set tuple |
| 13 Matcher configs | same-descriptor edges from M1/M2 | No | connected Track | none | none; scores unused |
| 14 star | A1-B1, A1-C1, A1-D1 | No | T{A1,B1,C1,D1} | none | none |
| 15 chain large | A1-B1-...-N1 | No | one ordered Track | none | none |
| 16 conflict-heavy | repeated image IDs | per component | valid components | none | conflicts dropped |

## Validation plan

Gate B/C must include exhaustive small graphs over 3–5 images and small
observation counts. Check no duplicate image, no cross-Track observation,
minimum length, no overlap, canonical ordering and permutation invariance.
Run each logical input 100 times in one process and in fresh processes; vary
GVR order, edge order, page size and practical DB row order. Future 1-thread
and N-thread runs must hash the same canonical logical output. A non-persistent
test hash may hash sorted Tracks and sorted observations; it is not Track Set
identity.

Future performance runs should cover 10k, 100k and 1M edges with low-conflict,
high-conflict, tiny-track, huge-component, chain and star distributions.
Measure wall time, CPU, RSS peak, bytes/node, bytes/edge and throughput.

## Explicitly out of scope

Triangulation, camera pose estimation, Sparse SfM, Essential matrix, Bundle
Adjustment, reprojection optimisation, 3D points, co-visibility matrices,
dense reconstruction, Vulkan Builder implementation, persistent edge
provenance, Track Model schema changes, Project DB v15, Track Builder Task
implementation and incremental optimisation implementation.

## Open questions

None critical for Gate A. Gate B must choose concrete in-memory data
structures without changing this scientific contract. A future API is needed
to page an exact selector-filtered GVR ID enumeration for callers that do not
already possess the explicit scope.

## Future transverse updates

**FUTURE TRANSVERSE UPDATE REQUIRED**

- file: `include/lardon3d/project_db.h` and `src/project_db.c`
- reason: expose a bounded, selector-filtered, deterministic GVR ID
  enumeration so orchestration can construct the explicit scope without a
  private SQL query; no schema change is required.
- expected gate: Gate B orchestration/API preparation.

## Gate status

**PASS.** All critical scientific and identity decisions are closed. No Track
Model or Project DB contract is redefined, no partial input is accepted, and
the result is independent of SQL order, pagination and future thread count.

## Implementation status

Gate A contract: **PASS/FROZEN**. Gate B pure core: **PASS**.

The Gate B core is a SQLite-free, single-threaded C-compatible API. The caller
provides resolved observations and edges and retains ownership of those input
arrays. The result owns separately allocated canonical Track arrays and is
released with `lardon3d_track_builder_result_free()`; a zero-initialized result
is safe to free repeatedly. Invalid input returns no partial result.

The implementation validates observation metadata, sorts and deduplicates
undirected edges, builds a deterministic observation table, unions every unique
edge with a sparse DSU, and validates complete components for image uniqueness
and exact Feature Set homogeneity. Invalid components are rejected as a whole;
valid components of at least two observations become lexicographically
canonical Tracks and Track Sets. Hash lookup is used only for index lookup; no
hash iteration order or DSU root is exposed.

The exact 48-byte `L3DTBFP1` encoding is hashed with SHA-256. Runtime values,
input scope and edge provenance are excluded. Measured synthetic chain runs
were 0.002 s, 0.032 s and 0.607 s for 10k, 100k and 1M edges respectively on
the development host, with a 1,000,000-edge process peak of 973,752 KiB. For
that chain the counts are 1,000,000 raw and unique edges, 1,000,001 unique
observations, one Track and zero rejected components. The public input arrays
occupy 152,000,136 bytes (136,000,136 bytes of observations and 16,000,000
bytes of pointer edges). The core uses 16,000,000 bytes of compact normalized
edges, a 136,000,136-byte canonical metadata table, 9,000,009 bytes for DSU
arrays, approximately 32 MiB for component vectors/capacity, and 24,000,024
bytes for compact output observations plus Track storage. The remaining
high-water RSS is allocator/hash-table/vector capacity and benchmark process
overhead; RSS is a process high-water metric, not the sum of live logical
payloads. No full metadata is copied into core edges. The benchmark remains
non-default. Targeted normal and
ASan/UBSan validation passed, including the 32,768-graph oracle and a 301
observation Track.

## Gate B closure audit

OpenSSL was already a project dependency before the Track Builder changes and
is used by the existing asset, Match File, matcher and verifier hashing paths.
Track Builder adds no new external dependency and does not introduce a second
internal SHA-256 helper. Its fingerprint vector remains
`e1f1fae479bcf82001a5b33dda331195617b8751668e46a6cf1eecf2d125df31`.

The 1M benchmark's `973752 KiB` is a process high-water RSS measurement. The
synthetic caller retains 136,000,136 bytes of observations and 16,000,000
bytes of pointer edges while the core retains its 136,000,136-byte canonical
metadata table, 16,000,000-byte normalized edges, DSU/component temporaries
and compact output. Compact edges contain node indices only; they do not copy
136-byte metadata at either endpoint. The remaining high-water gap is
primarily allocator retention plus hash-table buckets/nodes and vector
capacity. No core duplication bug was found or changed in the closure audit.

The targeted corpus explicitly covers reversed endpoints, 100 identical
edges, contradictory metadata, fingerprint/dimension/version conflicts,
301-observation tracks, disjoint length-2 tracks, sorted and reverse-sorted
edges, empty edge lists and self-edges. The 32,768 production-DSU versus
independent DFS exhaustive comparison remains passing. Cross-process
repeatability was not run and is non-blocking for Gate B.

## Gate C — Project DB orchestration

**DECISION:** Gate C exposes an explicit-scope adapter around the SQLite-free
Gate B core. The caller owns a copied, strictly increasing, non-empty GVR ID
list and supplies the exact verifier selector. The adapter computes the frozen
`L3DTSIS1` digest and performs exact Track Set reuse before reading any asset.

On a reuse miss, each selected GVR is loaded and checked as
`GVR → Match Result → Candidate Pair → Feature Set A/B → Image A/B`. The
content-addressed Match File is validated and read one at a time; its entries
are consumed in file order and the persisted LSB-first mask selects inliers.
Only Feature Set metadata is loaded—descriptor payloads are never read. A
small per-build Feature Set cache is permitted, while Match File buffers are
released immediately after edge extraction.

The complete resolved graph is passed unchanged to the Gate B core. Before the
frozen `lardon3d_project_db_create_track_set()` publication transaction, the
same explicit GVR IDs, status, selector, parent chain and scope count/hash are
revalidated. Any missing, rejected, mismatched or corrupt selected input aborts
the whole build; no partial Track Set is published. New matching GVRs are not
discovered or included. The Track Model API owns timestamps, validation,
atomicity and late exact-identity reuse. Project DB remains v14; Task and
Resource Governor orchestration are not part of Gate C.

The implementation does not add a selector discovery API: callers that need
discovery must enumerate and freeze their own explicit list in a separate
operation.

DB orchestration: **GATE C IMPLEMENTED**. Task: **NOT_IMPLEMENTED**. Resource
Governor: **NOT_IMPLEMENTED**. Project DB remains v14 and unchanged.

## Gate C closure evidence (current worktree)

The integration harness now constructs a synthetic Project DB v14 through the
public image, Feature Set, Candidate Pair, Match Result, GVR and Track Model
APIs. C01–C25 and the partial-input proof pass, including corruption,
scientific conflicts, zero-track publication, close/reopen durability,
different-scope identity and late matching-GVR exclusion. The test-only phase
seam is compiled only into the Gate C test executable.

Normal and AddressSanitizer/UndefinedBehaviorSanitizer targeted runs pass for
the core and project executables. The 106,496-edge synthetic resource case
completed in 0.281 seconds with a 26,564 KiB process high-water RSS and one
Match File live at a time. C26 has no clean fault injection in the frozen DB
API and C27 belongs to the future concurrency gate.

Gate C status: **PASS**. Gate A: **PASS**. Gate B: **PASS**. Gate D:
**NOT_IMPLEMENTED**. Gate E: **NOT_DONE**. Track Builder v1 is not marked
fully frozen by this document; the future runtime gates remain separate.
