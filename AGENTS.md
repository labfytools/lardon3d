# Lardon3D — Agent Engineering Contract

## 1. Authority and priority

- The root `README.md` is the index of project documentation.
- Canonical documents under `docs/**` define scientific contracts, architecture,
  FROZEN invariants, persistence semantics, lifecycle state, resource policy,
  and roadmap ordering.
- Before changing an architectural, scientific, persistence, runtime, or
  resource-sensitive area, identify and read the relevant canonical document
  and its applicable invariants.
- FROZEN documentation and contracts must never be changed silently to fit an
  implementation.
- A lower-level implementation convenience never overrides a higher-level
  canonical contract or an explicit human decision.
- Lifecycle markers such as `PLANNED`, `IMPLEMENTED`,
  `VALIDATION PENDING`, and `PASS/FROZEN` describe repository state and must
  match actual implementation and validation evidence.
- If requested work, current code, and a FROZEN contract genuinely conflict:
  stop the affected change, report the contradiction, and require an explicit
  human decision. Do not choose a new policy implicitly.
- Do not manufacture a human decision from an ordinary implementation defect.
  Resolve locally determinable engineering problems from the existing code and
  canonical documentation.

## Repository language policy

The canonical language of the Lardon3D repository is English.

This applies to:

- README.md;
- AGENTS.md;
- all current and future files under docs/**;
- prompt.md and all files under prompt/**;
- source comments under include/lardon3d/** and src/**;
- developer-facing technical documentation, diagnostics and engineering contracts where applicable.

The repository must not intentionally mix French and English technical prose.
Existing French documentation and source comments are to be converted to English during the planned documentation and source-comment remediation passes, without changing scientific meaning, FROZEN facts, historical evidence, numeric values, identities, lifecycle truth or contract authority.

Historical documents may be translated, but translation must not modernize or reinterpret the historical state they record.

User-interface language is a separate product concern. Existing TUI labels may remain in their current language until an explicit localization/product decision is made. Repository language policy does not require the UI to be English-only.

Canonical markers:

DOCUMENTATION_LANGUAGE=ENGLISH

SOURCE_COMMENT_LANGUAGE=ENGLISH

AGENT_CONTRACT_LANGUAGE=ENGLISH

USER_INTERFACE_LANGUAGE=INDEPENDENT

## 2. FROZEN integrity

The following current project foundation is protected and may change only
through an explicitly authorized, explicitly scoped human ticket:

- Gates A–G — PASS/FROZEN
- Track Model — PASS/FROZEN
- Track Builder scientific contract — PASS/FROZEN
- F0 — PASS/FROZEN
- Phase H v1 — PASS/FROZEN
- MVS-M1 — PASS/FROZEN
- Project DB v22 scientific/persistence foundation — PASS/FROZEN
- Project DB v23 generic optical-context overlay — PASS/FROZEN
- Calibration Bootstrap v1 — PASS/FROZEN
- Selected Scientific Execution — PASS/FROZEN
- Photo Quality Triage / Acquisition Selection — PASS/FROZEN
- S1 Capture / Asset Provenance — PASS/FROZEN
- S2 Capture-safe Standard Ingestion — PASS/FROZEN
- S3 Capture / Acquisition Ingestion — PASS/FROZEN
- Durable Acquisition-Campaign Execution — PASS/FROZEN
- Global Maintenance Audit — PASS/FROZEN
- Real S21 Tracks scientific result — PASS/FROZEN

Detailed subcontracts remain defined by their canonical documents. This file
must not duplicate every S3 substage, scientific threshold, migration detail,
or persistence format.

Project DB v25 is the current additive operational schema in the active Feature
batch tranche. It preserves v24 RAW batch, the v23 optical overlay and the v22
scientific/persistence foundation. Its authorized purpose is limited to typed
durable persistence for `features.extract.batch/1` through
`feature_extract_batch_tasks`; it adds no scientific identity and must not
reinterpret historical rows. Until the v25/Feature-batch tranche has completed
its real-data proof and final review, its lifecycle must remain truthful rather
than being marked `PASS/FROZEN` prematurely. Historical references to older
Project DB versions remain valid where they describe the actual historical
contract or migration path.

The global maintenance implementation, fresh portable/Vulkan/sanitizer/
concurrency validation and independent final review are acquired. Its lifecycle
is `GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN`. The review independently passed the
portable build, complete suites, focused matrices, strict public-header probes,
ABI and production-seam checks, retained-manifest verification and diff
validation with zero blocking findings. Do not reopen this boundary or infer a
new scientific policy from the freeze.

The canonical review checkpoint is tag `global-maintenance-2026-09-01` at
commit `b84f860d868c66d9ee84b85ceb1bc6480b95aca5`; its detailed evidence is
[`docs/architecture/global_maintenance_audit.md`](docs/architecture/global_maintenance_audit.md).
Future reviews are delta-based from this checkpoint: begin with
`git diff global-maintenance-2026-09-01...HEAD` and examine changed files,
their directly affected contracts, tests, documentation, and crossed dependency
boundaries. Unchanged PASS/FROZEN systems inherit this evidence and are reopened
only by concrete evidence; do not repeat a global A-to-Z audit.

When a ticket declares `NO_NEW_SUBSYSTEM`, do not introduce an unrelated:

- Task Runtime;
- Queue;
- Scheduler;
- Resource Governor;
- worker pool;
- persistence subsystem;
- viewer;
- mesh/texturing subsystem;
- scratch/SSD manager;
- generic backend framework;
- message bus;
- daemon.

This restriction is ticket-scoped. It does not mean that a subsystem already
present in the canonical architecture can never be used or extended by later
authorized work.

Never reopen a FROZEN scientific decision merely to simplify an
implementation. Operational/resource contracts may be reopened only by explicit
human authority and must preserve the scientific result exactly unless the
human ticket explicitly says otherwise.

## 3. Scope and Git discipline

- Inspect `git status --short` before editing.
- Preserve unrelated user and worktree changes.
- Modify only files authorized by the current ticket.
- Never modify or add anything under `scan3d/`, especially
  `scan3d/tri_photos.py`, unless a future human ticket explicitly removes that
  protection.
- Never use `git add -A` against the real repository index.
- Without explicit human authorization, never:
  - stage;
  - commit;
  - push;
  - reset;
  - restore;
  - checkout files or another branch;
  - stash;
  - clean;
  - rebase;
  - merge;
  - amend;
  - force any Git operation.
- Never discard unrelated modifications.
- At delivery, list the exact files modified by the ticket.

For review of untracked files, a temporary `GIT_INDEX_FILE` may be used when
necessary. Add paths explicitly, keep the real index untouched, and remove the
temporary index afterward.

Do not use temporary-index machinery when a normal diff or `--no-index` check
is sufficient.

A dirty worktree may be legitimate during an implementation tranche. Do not
require a clean worktree unless the ticket explicitly requires one.

## 4. Language / API / ABI rules

- Public C APIs must remain valid C17.
- C++ must remain within the standard configured by Meson.
- The repository is intentionally mixed-language and must not be treated as
  C-only.
- Preserve public API and ABI unless the ticket explicitly authorizes a change.
- No C++ exception may cross an `extern "C"` or other public C ABI boundary.
- Keep ownership and lifetime rules explicit.
- Do not introduce undefined behavior.
- Reject unchecked narrowing where the destination cannot represent the full
  accepted input domain.
- Reject unchecked integer overflow.
- Reject silent path truncation.
- Deterministic serialization must use explicit fixed widths and byte order.
- Never serialize native structs or depend on native struct padding or native
  endianness.
- Do not expose C++ standard-library types through public C17 headers.
- Public buffers and fixed-capacity strings must have explicit bounds and
  termination semantics.
- If a public function may be retried, its idempotency or conflict behavior
  must be explicit where non-obvious.
- Sparse SfM relative-pose and PnP `max_iterations`/`minimum_inliers` remain
  fixed-width `uint32_t` scientific fields, but their public OpenCV boundary is
  operationally limited to `INT_MAX`. Reject a larger value before narrowing,
  allocation, solver execution or output mutation; do not alter the FROZEN
  defaults, encodings or fingerprints to accommodate an unsafe cast.

Compiler success alone is not proof of API, ABI, persistence, resource, or
scientific contract correctness.

## 5. Identity discipline

Identity boundaries are architectural contracts.

Never silently equate:

- Capture and file;
- Capture and asset;
- Capture and `image_id`;
- Capture and SHA-256;
- Capture and path;
- Capture and filename/basename;
- Capture and Task ID;
- Capture and campaign group ID;
- Task ID and scientific acquisition identity;
- campaign group ID and scientific Capture identity.

Operational identifiers may legitimately reference scientific objects, but
they do not redefine their identity.

In particular:

- SHA-256 identifies immutable asset bytes.
- `capture_id` identifies a physical acquisition representation in Project DB.
- `image_id` identifies a scientific image representation.
- Task ID identifies durable execution work.
- campaign group ID identifies a stable operational group within a campaign
  request.

Never recover a Capture by guessing from path, SHA, basename, timestamp,
metadata, asset, image, Task ID, or group ID unless a future FROZEN contract
explicitly defines such an identity.

## 6. Architecture and resources

- Keep TUI/ncurses ownership separate from business logic and layout according
  to canonical architecture.
- Keep ncurses on its designated/main thread wherever the canonical contract
  requires it.
- The current TUI is a validated operational observatory/control center. Keep
  Queue/Task/host observation coalesced and bounded, keep durable scientific
  progress distinct from generic runtime percentage, and keep unknown
  provenance visibly UNKNOWN. Full layout starts at 100x30, compact is
  supported through 60x15 (72x20 is the reference compact boundary), and only
  the bounded "Terminal trop petit" fallback is allowed below that minimum.
- Preserve the contextual key contract. `F10 SSD` remains literally visible at
  60 columns in idle, text-input and import-running modes; text input owns only
  Enter/Escape/F10 and a running import owns only cancel (`X`) and F10 while
  quit/Escape are visibly disabled. ncurses input and rendering remain on the
  main thread.
- Opening, closing, or switching a project is a Queue/DB lifetime boundary:
  destroy and join the sole Queue, including finished callbacks, before Project
  DB close; then recreate one empty Queue and rebind observers. Never sample
  running/pending counts as a substitute for that boundary.
- Extend validated abstractions rather than rewriting validated modules.
- Reuse the existing Task / Queue / Scheduler / Resource Governor ownership
  model rather than creating parallel runtime infrastructure.

### Canonical utilization objective — MAXIMUM SAFE USEFUL THROUGHPUT

Lardon3D is a throughput-oriented workstation application. The Resource
Governor MUST maximize safe and useful utilization for every production Task
after preserving only the interactive host reserve needed for normal use of:

- Arch Linux / Sway and normal desktop services;
- Firefox;
- normal audio/music playback;
- lightweight interactive use of the workstation.

All CPU, RAM, I/O capacity and validated accelerator capacity beyond that
interactive reserve belongs to the active Lardon3D workload when useful work
exists.

On the current reference host, the normal observed policy outcome is
approximately:

```text
16 logical CPUs total
4 logical CPUs reserved for the interactive host
12 logical CPUs available to the compute pool
~3 GiB MemAvailable preserved as the hard RAM reserve
Radeon 780M UMA available to validated/useful GPU backends
```

These values are reference-host outcomes, NOT portable constants. Topology,
process affinity, smaller hosts, memory pressure, I/O pressure and future
hardware must be handled dynamically. A future 32-thread host must not inherit a
12-thread product ceiling merely because the reference host exposed 12 compute
threads.

The optimization target is:

```text
MAXIMUM SAFE USEFUL THROUGHPUT
```

It is NOT:

```text
minimum resource usage
fixed CPU counts
CPU/GPU utilization percentage for its own sake
preserving historical CPU1/batch1 descriptors without evidence
```

The Governor remains the sole production resource authority. Normal users do
not choose CPU count, worker count, batch, inflight depth, GPU backend, scratch
mode, or RAM budget for authoritative execution.

A production Task may deliberately use less than the currently available
compute pool only when concrete evidence establishes at least one relevant
constraint, such as:

- true scientific or dependency serialism;
- measured useful-scaling knee;
- memory bound;
- I/O saturation;
- validated GPU execution making additional CPU work useless;
- deterministic publication constraint that cannot be separated safely from
  preparation;
- another explicit and measured resource limitation.

The reason must be represented by the Task/Governor contract or canonical
resource documentation where non-obvious. A historical descriptor by itself is
not evidence.

### SERIALISM_REQUIRES_PROOF

`SERIALISM_REQUIRES_PROOF` is canonical operational policy.

Per-item atomicity does NOT imply cross-item serialization. Owner-only or
ordered durable publication does NOT imply serial preparation. When multiple
independent work units exist and exact science/persistence semantics are
preserved, the Task must expose bounded concurrency to the Governor.

A long-running `CPU1` or `batch1` path with independent executable work and
available safe resources is an operational defect until one of the documented
measured limitations above proves otherwise.

Do not solve this by creating another scheduler, uncontrolled worker pool,
unbounded `std::async`, detached threads, or resources outside Governor
accounting. Prefer the established bounded pattern where appropriate:

```text
one admitted owner Task
    -> bounded independent participants/preparation
    -> join all participants
    -> deterministic owner-only publication
```

Every participant must be accounted, bounded, cancellable and joined. Shared
SQLite access must preserve the existing ownership/serialization contracts.

### CPU, GPU, RAM and I/O policy

- CPU: expose truthful minimum/useful/safe concurrency and allow Governor to use
  the largest currently safe/useful value. Do not hardcode the reference-host
  12-thread result.
- GPU: a validated AND useful production GPU backend MUST be preferred when it
  is available and Governor-safe. Do not invent or promote an unvalidated GPU
  path merely to make the GPU busy.
- RAM: use available memory aggressively for useful work while preserving the
  hard interactive reserve, approximately 3 GiB `MemAvailable` on the reference
  host. The 3–4 GiB band is a pressure/conservatism zone, not a permanent extra
  1 GiB subtraction from every Task.
- UMA: iGPU allocations count exactly once against host RAM.
- swap/zram: pressure and safety mechanisms, never admitted working RAM.
- scratch SSD: optional external storage, never RAM. A Task may consume it only
  through an explicitly validated Governor-owned scratch contract.
- I/O: if additional concurrency no longer improves useful throughput because
  storage or another I/O boundary is saturated, the measured knee is the right
  useful limit. Do not force CPU saturation for appearance.
- Pressure: CPU/memory/I/O PSI and active swap-in/out deltas may reduce
  admission. When pressure clears, useful resources must be re-admitted rather
  than remaining permanently throttled.

System responsiveness is therefore protected by the explicit host reserve and
pressure feedback; it is not a justification for leaving additional safe/useful
compute idle.

- Bound memory, buffers, files, descriptors, processes, threads, captured
  output, parser work, temporary storage, and staging honestly for the
  operation.
- An operational hardware/resource bound must not accidentally become a
  scientific dataset-size limit.
- Do not invent a new global resource subsystem inside a ticket that defers it.
- Contractually atomic outputs must remain atomic.
- Failure paths must clean all resources owned by the current operation without
  deleting unrelated or shared data.
- Shared immutable assets must never be deleted merely because one operation
  fails.

Resource-sensitive work must identify, where relevant:

- resource owner;
- admission point;
- minimum/useful/safe CPU and batch capability;
- fixed/per-participant/transient memory budget;
- reservation lifetime;
- release point;
- failure cleanup;
- cancellation cleanup;
- I/O/scratch ownership;
- validated GPU capability;
- whether a bound is operational or scientific;
- measured reason for intentional under-utilization.

The reviewed external USB SSD controller is the authorized physical-lifecycle
boundary for the exact UDisks Drive/label/UUID contract. Its validated snapshot
is registered with the Resource Governor; the Governor wrappers are the sole
production orchestrator for scratch-lease acquire/release. The controller does
not replace the Governor or invent Task scratch eligibility, and swap/scratch
never become RAM. The application lifetime order is strict: destroy/join the
Queue so every Task lease is released, checked-join/unregister the SSD binding,
destroy the controller, then destroy the Governor. SSD availability is a
capability, not fabricated Task usage. Do not add ad-hoc discovery, mounting,
formatting, `swapon`, cleanup, force-drain, shell commands, or a second
resource/scheduling subsystem outside the reviewed controller/Governor APIs.

Snapshot conversion is fail-closed by physical state. Any pairing or authority
requires current detection of the Drive and both UUID-bearing partitions,
positive known partition extents, and coherent mount/activity/capability facts;
partial `DETECTED` state is observable but non-actionable. A disconnected sticky
hazard may retain identity only as non-allocating `ERROR`, and drain authority
requires the exact reconnected original tuple. Controller generation
`UINT64_MAX` is legal saturation: arbitrary equal-generation public updates
remain stale and cannot regrant authority, while only the serialized Governor
lease wrapper may reconcile its own exact completion and address-backed lease
count at that watermark.

## 7. Code quality and readability

- Clang and Meson are the reference build tools unless a ticket explicitly
  requires another supported compiler for validation.
- Do not add global mutable state unless an established contract explicitly
  permits it.
- Clean allocations, file descriptors, process handles, mutexes, conditions,
  reservations, and threads explicitly.
- Do not use `system()` or `popen()` in production where direct process
  execution is required.
- Do not leave accidental TODOs, dead code, debug paths, or dependencies in a
  completed ticket.
- Prefer minimal evolution to rewrites.
- Target approximately 100 columns; normally do not exceed 120 without local
  justification.
- Do not use code golf.
- Do not compress several logical operations onto one line merely to reduce
  line count.
- Split complex code when doing so materially improves auditability.
- Temporary probes, benchmarks, diagnostics, C/C++/GLSL files, and scripts
  under `/tmp` must also remain readable and auditable.
- Formatting-only changes must not alter behavior.
- Do not run a whole-repository formatting sweep inside an unrelated ticket.
- Do not reorder includes, rename symbols, or normalize whitespace without a
  technical reason in scope.

Required source comments and canonical documentation are part of the
implementation, not optional cleanup work.

## 8. Source Comment Contract — MANDATORY

All new or materially modified production code MUST include the comments
required to make its non-obvious contracts understandable from the source.

This requirement is part of the Definition of Done.

Comments MUST document the relevant WHY / CONTRACT / INVARIANT when code
introduces or materially modifies any of the following:

- public C17 API semantics;
- ownership or lifetime;
- persistence or transaction boundaries;
- serialization formats;
- deterministic or scientific behavior;
- coordinate or pose conventions;
- identity semantics;
- idempotency or retry behavior;
- Task / Queue / Scheduler / Resource Governor ownership;
- resource admission, reservation, and release;
- RAM / CPU / GPU / I/O / temporary-storage bounds;
- concurrency or synchronization;
- C/C++ ABI and exception-containment boundaries;
- malformed-input or corruption handling;
- non-obvious algorithmic assumptions;
- crash/restart ordering;
- FROZEN scientific or architectural invariants.

Comments MUST explain WHY a rule exists or WHAT CONTRACT must remain true.

Do NOT add comments that merely narrate obvious code.

Bad:

```c
// Increment index.
index++;
```

Good:

```c
// Advance progress only after the group_id -> capture_id mapping is durable.
// Recovery must never observe a completed group without a retained Capture ID.
```

Another good example:

```c
// SHA-256 identifies immutable asset bytes only. It must never be used as
// physical Capture identity.
```

### Public APIs

New or materially modified public declarations under `include/lardon3d/` MUST
document the relevant subset of:

- purpose;
- valid inputs;
- nullable arguments;
- output semantics;
- ownership;
- lifetime;
- bounded capacities;
- deterministic behavior;
- idempotency;
- error/result semantics;
- runtime or thread assumptions;
- persistence implications where relevant.

Public comments must remain valid C17 documentation and must not expose C++
implementation details unnecessarily.

Use the repository's existing documentation-comment style consistently. Do not
introduce verbose Doxygen ceremony where the repository does not need it.

### Persistence and serialization

Every new or materially modified persistent format MUST have a concise format
contract documenting the relevant subset of:

- magic;
- serialization version;
- byte order;
- fixed-width field semantics;
- string representation;
- count bounds;
- maximum encoded size;
- malformed/truncated rejection;
- compatibility/migration behavior;
- checksums/fingerprints where applicable.

Never serialize native structs or rely on native padding or endianness unless
an existing FROZEN contract explicitly requires it.

Document a format contract once at the correct abstraction boundary. Do not
comment every individual `encode_u32()` or equivalent call.

### Scientific code

Scientific comments MUST preserve the distinction between:

- scientific identity;
- operational identity;
- resource policy;
- implementation detail.

Where relevant, comments should make explicit non-obvious conventions such as:

- coordinate frame direction;
- camera/world pose convention;
- reprojection units;
- calibration assumptions;
- deterministic RNG behavior;
- cheirality/parallax constraints;
- gauge handling;
- threshold provenance;
- strong vs weak evidence;
- `CALLER_EXPLICIT` vs scientifically `STRONG`.

Do not silently strengthen or weaken a FROZEN scientific rule through a
comment.

### RAW / JPEG / MPF

Where relevant, comments must preserve the established distinction between:

- immutable SOURCE RAW;
- camera JPEG SOURCE;
- deterministic DERIVED RAW representation;
- RAW Policy v1;
- L3DRAWD1 identity;
- metadata-only acquisition processing;
- structural JPEG validation;
- JPEG marker payload boundaries;
- entropy/stuffing/restart behavior;
- structural outer EOI;
- primary MPF APP2 evidence;
- bounded secondary JPEG validation;
- zero-only permitted MPF gaps/trailer.

For the current A6000 selected scientific execution, paired camera JPEGs are
valid Capture source assets and preferred fast proxies for Photo Quality, but
the FROZEN geometry representation remains deterministic RAW-derived PNG. Do
not substitute camera-JPEG geometry without a separately authorized and proven
scientific/calibration equivalence tranche.

Do not imply that metadata validation performs pixel decoding when it does not.

### Task / Queue / Scheduler / Governor

Where non-obvious, comments must make runtime ownership explicit.

In particular:

- Task owns execution state.
- Queue owns bounded dispatch/backpressure.
- Scheduler responsibilities are represented by the established runtime/Queue
  architecture; do not invent a second scheduler.
- Resource Governor owns admission and hardware-resource budgets.
- Resource Reservation belongs to the currently admitted bounded execution.
- `sequence_break` is an execution boundary that releases/re-establishes
  admission according to runtime semantics.
- Per-item atomicity does not imply cross-item serialization.
- Serial publication may coexist with Governor-admitted parallel preparation.

Do not imply that a long campaign reserves resources for its entire lifetime
when execution is group-bounded.

### Resource-sensitive code

Any new or materially modified resource-sensitive path MUST make the relevant
resource ownership understandable from source.

Where applicable document:

- RAM ownership/bound;
- CPU minimum/useful/safe admission;
- GPU admission;
- I/O ownership;
- process/thread count;
- per-participant memory;
- file-descriptor lifetime;
- temporary/scratch storage ownership;
- reservation lifetime;
- release point;
- failure cleanup;
- cancellation cleanup;
- measured scaling knee or reason for serial execution.

An operational hardware/resource bound MUST NOT accidentally become a
scientific dataset-size limit.

### Crash / restart / idempotency

Where restart ordering matters, document the durable invariant.

For durable campaign execution, the current ordering is conceptually:

```text
S3-E returns capture_id
    ↓
group_id -> capture_id mapping + durable cursor commit
    ↓
generic Task progress/checkpoint advances
    ↓
next group may execute
```

Do not document a guarantee stronger than production provides.

The residual pre-return S3-E crash window remains intentional and documented:
if S3-E creates a Capture internally and the process dies before S3-E returns
the `capture_id`, campaign execution must not guess that Capture identity.

For `raw.develop.batch/1`, typed `task_id -> selected_execution_id` persistence
must remain distinct from generic Task runtime state. Independent RAW
participants may prepare concurrently, but owner publication and selected-item
cursor advancement remain deterministic and ordered by the selected execution
contract.

### C / C++ ABI boundaries

Where C++ implements a public C interface or C Task callback, comments should
make exception containment understandable when non-obvious.

No C++ exception may escape a C ABI boundary.

Do not duplicate the same statement above every trivial wrapper when one local
contract comment clearly governs a set of wrappers.

### Tests

Tests should comment only non-obvious fixtures or failure scenarios.

Comments should state WHAT CONTRACT the fixture proves, for example:

- intentional malformed input;
- crash window;
- rollback injection;
- ambiguity;
- scientific identity conflict;
- cross-ScanSet rejection;
- resource-pressure/admission condition;
- deliberate CPU1 baseline used only for a scaling comparison.

Do not narrate ordinary test mechanics.

### Documentation synchronization

Source comments do NOT replace canonical architecture documentation.

Canonical FROZEN documentation remains authoritative.

If code, source comments, and canonical documentation disagree:

1. do not silently change a FROZEN contract;
2. identify the contradiction;
3. correct stale comments/documentation only when actual behavior and authority
   are already established;
4. report an implementation defect if code violates the canonical contract.

### Mandatory implementation workflow

For every future implementation tranche:

1. implement the bounded production change;
2. add/update targeted tests;
3. add/update required source comments in the SAME tranche;
4. update canonical documentation when behavior/contracts changed;
5. validate every modified public C17 header;
6. validate relevant C++ syntax/build integration;
7. validate resource ownership, accounting, useful scaling and bounds;
8. run applicable build/tests/sanitizers efficiently;
9. perform the required bounded review;
10. only then claim completion.

The normalized future implementation standard is:

```text
CODE
+ TESTS
+ REQUIRED SOURCE COMMENTS
+ CANONICAL DOCUMENTATION
+ C17/C++ SYNTAX
+ RESOURCE OWNERSHIP
+ MAXIMUM SAFE USEFUL THROUGHPUT
+ VALIDATION
+ REVIEW
```

A tranche is NOT complete merely because code compiles and tests pass when
required contract/invariant comments, canonical documentation, or required
resource behavior are missing.

There should be no future project-wide comment-cleanup pass for newly written
code: comment debt must be handled when the code is introduced.

## 9. External processes and filesystem

- Prefer direct process execution over shell command strings.
- Explicitly own, monitor, and reap child processes.
- Use process groups when required and prevent orphaned children.
- Bound retained stdout/stderr where applicable.
- Drain child output when required to prevent pipe deadlocks.
- Clean operation-owned resources on every success and failure path.
- Do not assume an external CLI flag exists without evidence.
- Authoritative upstream documentation or source may be inspected for external
  dependencies.
- Temporary probes, clones, and builds under `/tmp` are allowed when useful.
- Reuse valid temporary upstream evidence instead of repeatedly downloading or
  rebuilding the same dependency.
- Do not use `sudo`.
- Do not modify the host persistently.
- Do not install system packages without explicit authorization.
- Do not add a permanent repository dependency without explicit authorization.
- A missing local executable alone is not proof that a required capability is
  unavailable.

## 10. Persistence and transaction discipline

Persistence changes require explicit attention to:

- atomic creation;
- rollback;
- idempotency;
- retry convergence;
- migration ordering;
- durable publication;
- cross-object consistency;
- restart state;
- corruption/malformed-input handling.

For Project DB:

- preserve the FROZEN v22 scientific/persistence semantics and v23 optical
  overlay;
- v24 is the explicitly authorized additive operational RAW-batch migration and
  must not reinterpret scientific history;
- future schema-version changes beyond the currently authorized v24 require
  explicit human authorization;
- migrations must be additive unless a different migration is explicitly
  authorized;
- existing projects must remain recoverable;
- do not silently reinterpret historical rows;
- typed Task business payload must not be confused with generic Task runtime
  state;
- transaction boundaries must preserve the documented crash/restart invariant.

Where a strict API returns a constraint and bounded exact reconciliation is
part of a FROZEN higher-level operation, do not weaken the strict API merely to
make retries convenient.

## 11. Documentation discipline

- Do not create duplicate canonical documents for one contract.
- Link every new canonical document under `docs/**` from the root `README.md`.
- For changes to API, architecture, ownership, concurrency, persistence,
  pipeline, resources, limits, or scientific semantics, check whether
  canonical documentation must be updated.
- `MAXIMUM SAFE USEFUL THROUGHPUT` and `SERIALISM_REQUIRES_PROOF` are canonical
  human resource-policy decisions. Any resource document that contradicts them
  is stale unless it describes an explicitly historical measurement.
- Documentation must describe proven implementation, not desired future
  behavior. Human policy may be marked as policy while implementation work is
  still `VALIDATION PENDING`.
- Roadmap documents may describe future behavior, but future capabilities must
  be clearly marked as planned/later/exploratory.
- Never describe future viewer, Capture Guidance, video/keyframe, Task scratch
  consumption, or camera-control capabilities as implemented before they are
  actually validated.
- Statuses such as `PLANNED`, `IMPLEMENTED`, `VALIDATION PENDING`, and
  `PASS/FROZEN` are authoritative lifecycle statements.
- Update lifecycle state only when implementation, validation, and review
  evidence support the transition.
- Never mark work `PASS/FROZEN` without the required validation and review.
- Preserve legitimate historical DB/version/resource references where they
  describe frozen history; label superseded operational envelopes as historical
  rather than silently rewriting the evidence.

## 12. Required validation and test resource policy

For ordinary implementation tickets, before claiming completion, actually run
the applicable commands. Builds, tests, benchmarks and real-proof workloads
MUST follow the same resource philosophy as production: preserve the
interactive host reserve, then use the maximum safe/useful remaining resources.
Artificially serial engineering work wastes both elapsed time and agent budget.

### Normal build

Do not use a fixed historical `-j8` as a product rule. Derive a safe parallel
job count from the current host/affinity. On the current 16-logical-CPU
reference host, where four logical CPUs are reserved for interactive use, the
normal build target is approximately:

```sh
meson setup --reconfigure build
meson compile -C build -j12
```

On another host, choose the analogous available-compute value rather than
hardcoding 12.

### Normal tests

Independent tests should run concurrently within the same host reserve and
memory constraints. On the current reference host, a typical command is:

```sh
meson test -C build --num-processes 12 --print-errorlogs
```

This is not permission to race tests that share mutable fixtures, real projects,
fixed ports, exclusive GPU state, or other process-global resources. Such tests
must be grouped or serialized for the concrete dependency, and that reason must
be understood rather than inherited from an old `--num-processes 1` default.

A CPU1 run is valid when CPU1 is the actual test case or baseline measurement;
it is not the authoritative production configuration after higher safe/useful
concurrency has been proven.

### Validation efficiency

- Do not rebuild unchanged targets between proof iterations without a concrete
  need.
- Do not rerun already-acquired expensive validation when the relevant code and
  dependency boundary did not change.
- Run focused tests first; widen only when the changed dependency boundary
  requires it.
- Expensive/stress validation must be relevant to the current delta.
- Heavy validation is parallel by default when independent and resource-safe;
  serialize only when scientific determinism, mutable shared fixtures, memory,
  I/O, GPU exclusivity, sanitizer behavior or measured host pressure requires
  it.
- A test timeout or sanitizer failure must be investigated; do not repeatedly
  rerun until green.
- Real-data proofs must use normal Governor admission. Do not manually force
  CPU1 merely for reproducibility unless CPU1 itself is the intended comparison
  cohort.

Diff validation:

```sh
git diff --check
```

For every new or modified public C header, also run a C17 syntax check, for
example:

```sh
cc -x c -std=c17 -fsyntax-only -Iinclude \
  -include lardon3d/<header>.h /dev/null
```

For memory/lifetime-sensitive changes, use a separate ASan/UBSan build and do
not overwrite the normal build.

For relevant concurrency changes, run TSan when supported and meaningful.

If a sanitizer is unavailable or invalid because of the environment/toolchain,
report that explicitly rather than claiming PASS.

Distinguish third-party sanitizer/environment noise from repository defects
using concrete stack/failure evidence.

Never claim a command, test, sanitizer, review, benchmark, or real-data
validation that was not actually performed.

For documentation/comment-only changes, do not invent unnecessary sanitizer
work, but still run enough build/syntax/diff validation to prove that the
non-functional boundary was preserved.

## 13. Engineering execution and credit economy

Engineering time and agent budget are finite project resources and must not be
wasted.

- Default to the configured economical parent/orchestrator and existing agent
  roles. Do not invent a new agent hierarchy.
- Use expensive/high-reasoning scientific or final-review roles only when the
  current delta genuinely requires scientific equivalence, difficult
  concurrency/persistence reasoning, or final sensitive review.
- Do not spawn multiple agents to restate the same architecture or repeat the
  same review.
- Do not delegate ordinary implementation defects upward when a worker,
  mechanic or resolver can fix them directly within authority.
- Prefer one focused independent review after the implementation is stable to
  repeated speculative reviews during ordinary coding.
- Reuse acquired maintenance, sanitizer, benchmark and real-data evidence when
  the affected boundary is unchanged.
- Do not wait on an expensive agent when the next authorized executable action
  can proceed independently.
- Keep tool/log output bounded; inspect targeted portions instead of repeatedly
  dumping whole logs.
- Do not rerun a global A-to-Z audit after the maintenance checkpoint.
- `WORK FIRST. RETURN LAST.` Ordinary in-scope findings are repaired and
  validated before returning to the human.

Low remaining agent/credit budget is a reason to eliminate redundant work, not
a reason to weaken required correctness evidence. If a genuinely required
validation cannot be afforded/executed, report it honestly rather than claiming
PASS.

## 14. Review discipline

A normal review should verify the bounded current delta and direct regressions.

Do not turn every review into a redesign of the repository.

Review findings must distinguish:

- blocking defect;
- non-blocking issue;
- future scope;
- unrelated pre-existing observation.

A reviewer request does not automatically define new policy.

Compare findings against:

1. canonical FROZEN documentation;
2. explicit human decisions, including the resource-utilization policy;
3. established tranche semantics;
4. documented future-scope boundaries.

Do not invoke an expensive implementation agent merely to satisfy speculative
hardening that is outside the current contract.

Comments and documentation are reviewable implementation artifacts. A
misleading contract or resource-policy comment is a defect even if compiled
behavior is unchanged.

Resource review must specifically flag accidental serialism when independent
work exists and safe/useful resources are idle without measured justification.

## 15. Delivery report / STOP conditions

Every completed ticket report must include:

- baseline branch/HEAD where relevant;
- exact files modified;
- concise implementation description;
- source-comment/documentation changes where required;
- validations actually executed and exact results;
- tests/checks not executed;
- known blockers;
- non-blocking findings;
- deliberately deferred/future-scope items;
- resource impact where relevant, including admitted/useful concurrency for
  modified resource-sensitive Tasks;
- confirmation that unrelated and FROZEN areas were preserved;
- Git state;
- confirmation that `scan3d/` remained untouched when protected.

STOP and request a human decision only when resolution requires:

- changing a FROZEN scientific contract;
- changing Project DB schema/version beyond already authorized v24 without
  prior authorization;
- introducing a genuinely new subsystem outside authorized scope;
- files outside the authorized scope;
- destructive Git action;
- `sudo`/root/system-wide installation;
- credentials/secrets;
- a materially different scientific policy not determined by canonical
  documentation;
- an infrastructure failure that prevents required work after the configured
  retry/fallback policy is exhausted.

Do NOT stop merely because:

- compilation fails;
- a normal test fails;
- an ordinary implementation bug exists;
- a reviewer identifies a bounded repairable defect;
- a resource descriptor is historically conservative and explicit human
  authority already allows operational correction while preserving science;
- a dependency needs factual investigation;
- documentation status is stale;
- an implementation detail can be resolved safely from existing code and
  contracts.

Advance the project while preserving scientific contracts and maximizing safe,
useful throughput.
