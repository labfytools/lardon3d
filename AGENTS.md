# Lardon3D — Agent Engineering Contract

## 1. Authority and priority

- The root `README.md` is the index of project documentation.
- Canonical documents under `docs/**` define scientific contracts, architecture,
  FROZEN invariants, persistence semantics, lifecycle state, and roadmap
  ordering.
- Before changing an architectural, scientific, persistence, runtime, or
  resource-sensitive area, identify and read the relevant canonical document
  and its applicable invariants.
- FROZEN documentation and contracts must never be changed silently to fit an
  implementation.
- A lower-level implementation convenience never overrides a higher-level
  canonical contract.
- Lifecycle markers such as `PLANNED`, `IMPLEMENTED`,
  `VALIDATION PENDING`, and `PASS/FROZEN` describe repository state and must
  match actual implementation and validation evidence.
- If requested work, current code, and a FROZEN contract genuinely conflict:
  stop the affected change, report the contradiction, and require an explicit
  human decision. Do not choose a new policy implicitly.
- Do not manufacture a human decision from an ordinary implementation defect.
  Resolve locally determinable engineering problems from the existing code and
  canonical documentation.

## 2. FROZEN integrity

The following current project foundation is protected and may change only
through an explicitly authorized, explicitly scoped human ticket:

- Gates A–G — PASS/FROZEN
- Track Model — PASS/FROZEN
- Track Builder — PASS/FROZEN
- F0 — PASS/FROZEN
- Phase H v1 — PASS/FROZEN
- MVS-M1 — PASS/FROZEN
- Project DB v22 — PASS/FROZEN
- Calibration Bootstrap v1 — PASS/FROZEN
- Selected Scientific Execution — PASS/FROZEN
- Photo Quality Triage / Acquisition Selection — PASS/FROZEN
- S1 Capture / Asset Provenance — PASS/FROZEN
- S2 Capture-safe Standard Ingestion — PASS/FROZEN
- S3 Capture / Acquisition Ingestion — PASS/FROZEN
- Durable Acquisition-Campaign Execution — PASS/FROZEN

Detailed subcontracts remain defined by their canonical documents. This file
does not duplicate every S3 substage, scientific threshold, migration detail,
or persistence format.

Historical references to older Project DB versions remain valid when they
describe the actual historical contract or migration path. Do not rewrite
legitimate v16/v17/v18/v19 history merely because v20 is current.

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
present in the canonical architecture can never be used by later authorized
work.

Never reopen a FROZEN scientific or architectural decision merely to simplify
an implementation.

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

Compiler success alone is not proof of API, ABI, persistence, or scientific
contract correctness.

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
- Extend validated abstractions rather than rewriting validated modules.
- Reuse the existing Task / Queue / Scheduler / Resource Governor ownership
  model rather than creating parallel runtime infrastructure.
- System stability and responsiveness take priority over throughput.
- Bound memory, buffers, files, descriptors, processes, threads, captured
  output, parser work, temporary storage, and staging reasonably for the
  operation.
- Memory shared with an iGPU counts against host RAM.
- zram and swap are pressure/safety mechanisms, not normal working-memory
  budgets.
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
- bound or budget;
- reservation lifetime;
- release point;
- failure cleanup;
- cancellation cleanup;
- whether the bound is operational or scientific.

External USB SSD scratch/swap support remains a planned resource-management
capability. Do not implement ad-hoc mounting, formatting, `swapon`, scratch
ownership, or device cleanup outside an explicitly authorized ticket.

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
- deterministic DERIVED representation;
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

Do not imply that a long campaign reserves resources for its entire lifetime
when execution is group-bounded.

### Resource-sensitive code

Any new or materially modified resource-sensitive path MUST make the relevant
resource ownership understandable from source.

Where applicable document:

- RAM ownership/bound;
- CPU admission;
- GPU admission;
- I/O ownership;
- process/thread count;
- file-descriptor lifetime;
- temporary/scratch storage ownership;
- reservation lifetime;
- release point;
- failure cleanup;
- cancellation cleanup.

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
- resource-pressure/admission condition.

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
7. validate resource ownership and bounds;
8. run applicable build/tests/sanitizers;
9. perform the required review;
10. only then claim completion.

The normalized future implementation standard is:

```text
CODE
+ TESTS
+ REQUIRED SOURCE COMMENTS
+ CANONICAL DOCUMENTATION
+ C17/C++ SYNTAX
+ RESOURCE OWNERSHIP
+ VALIDATION
+ REVIEW
```

A tranche is NOT complete merely because code compiles and tests pass when
required contract/invariant comments or canonical documentation are missing.

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

- preserve current v20 semantics unless a ticket explicitly authorizes a schema
  change;
- schema-version changes require explicit human authorization;
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
- Documentation must describe proven implementation, not desired future
  behavior.
- Roadmap documents may describe future behavior, but future capabilities must
  be clearly marked as planned/later/exploratory.
- Never describe future viewer, Capture Guidance, video/keyframe, SSD scratch,
  or camera-control capabilities as implemented before they are actually
  validated.
- Statuses such as `PLANNED`, `IMPLEMENTED`, `VALIDATION PENDING`, and
  `PASS/FROZEN` are authoritative lifecycle statements.
- Update lifecycle state only when implementation, validation, and review
  evidence support the transition.
- Never mark work `PASS/FROZEN` without the required validation and review.
- Preserve legitimate historical DB/version references where they describe
  frozen history.

## 12. Required validation

For ordinary implementation tickets, before claiming completion, actually run
the applicable commands.

Normal build:

```sh
meson setup --reconfigure build
meson compile -C build -j8
```

Normal tests:

```sh
meson test -C build --num-processes 1 --print-errorlogs
```

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

Run expensive/stress validation only when relevant.

Run heavy validation serially when required to preserve machine stability.

Investigate a timeout or sanitizer failure. Do not repeatedly rerun a failing
test until it happens to pass.

Distinguish third-party sanitizer/environment noise from repository defects
using concrete stack/failure evidence.

Never claim a command, test, sanitizer, review, or real-data validation that
was not actually performed.

For documentation/comment-only changes, do not invent unnecessary sanitizer
work, but still run enough build/syntax/diff validation to prove that the
non-functional boundary was preserved.

## 13. Review discipline

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
2. explicit human decisions;
3. established tranche semantics;
4. documented future-scope boundaries.

Do not invoke an expensive implementation agent merely to satisfy speculative
hardening that is outside the current contract.

Comments and documentation are reviewable implementation artifacts. A
misleading contract comment is a defect even if compiled behavior is unchanged.

## 14. Delivery report / STOP conditions

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
- resource impact where relevant;
- confirmation that unrelated and FROZEN areas were preserved;
- Git state;
- confirmation that `scan3d/` remained untouched when protected.

STOP and request a human decision only when resolution requires:

- changing a FROZEN contract;
- changing Project DB schema/version without prior authorization;
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
- a dependency needs factual investigation;
- documentation status is stale;
- an implementation detail can be resolved safely from existing code and
  contracts.

Advance the project while preserving the contracts.
