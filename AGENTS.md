# Lardon3D — Agent Engineering Contract

## 1. Authority and priority

- The root `README.md` is the index of project documentation.
- Canonical documents under `docs/**` define scientific contracts, architecture,
  FROZEN invariants, and roadmap ordering.
- Before changing an architectural or scientific area, identify and read the
  relevant canonical document and its applicable invariants.
- FROZEN documentation and contracts must never be changed silently to fit an
  implementation. A lower-level convenience never overrides a higher-level
  canonical contract.
- If requested work, code, and a FROZEN contract genuinely conflict: stop the
  affected change, report the contradiction, and require an explicit human
  decision. Do not choose a new policy implicitly.

## 2. FROZEN integrity

The following historical foundation is protected and may change only through
an explicitly authorized, explicitly scoped human ticket:

- Gates A–G — PASS/FROZEN
- Track Model — PASS/FROZEN
- Builder — PASS/FROZEN
- F0 — PASS/FROZEN
- Phase H v1 — PASS/FROZEN
- MVS-M1 — PASS/FROZEN
- Project DB v18

When a ticket declares `NO_NEW_SUBSYSTEM`, do not introduce Task Runtime,
Queue, Governor, scheduler, pools, persistence, viewer, mesh, texturing,
scratch/SSD, or another deferred subsystem. This file states invariants and
working rules; it does not duplicate a roadmap snapshot or claim that a
deferred subsystem can never exist.

## 3. Scope and Git discipline

- Inspect `git status` before editing.
- Preserve unrelated user and worktree changes.
- Modify only files explicitly authorized by the ticket.
- Never modify or add anything under `scan3d/`, especially
  `scan3d/tri_photos.py`.
- Never use `git add -A` against the real repository index.
- Without explicit human authorization, never commit, push, reset, restore,
  checkout files, stash, clean, or perform another destructive Git operation.
- Never discard unrelated modifications.
- At delivery, list the exact files modified by the ticket.

For review of untracked files, a temporary `GIT_INDEX_FILE` may be used when
necessary. Add paths explicitly; do not use `git add -A`; keep the real index
untouched; remove the temporary index afterward. Do not use this machinery when
a normal diff is sufficient.

## 4. Language / API / ABI rules

- Public C APIs must remain valid C17.
- C++ must remain within the standard configured by Meson; the repository is
  intentionally mixed-language and must not be treated as C-only.
- Preserve public API and ABI unless the ticket explicitly authorizes change.
- No C++ exception may cross an `extern "C"` or other public C ABI boundary.
- Keep ownership and lifetime rules explicit.
- Do not introduce undefined behavior, unchecked narrowing, or unchecked
  integer overflow.
- Reject silent path truncation.
- Deterministic serialization must use explicit fixed widths and byte order;
  never depend on native struct padding or native endianness.

## 5. Architecture and resources

- Keep TUI/ncurses ownership separate from business logic and layout according
  to canonical architecture.
- Keep ncurses on its designated/main thread wherever the canonical contract
  requires it.
- Extend validated abstractions rather than rewriting validated modules.
- System stability and responsiveness take priority over throughput.
- Bound memory, buffers, files, descriptors, processes, threads, and captured
  output reasonably for the operation.
- Memory shared with an iGPU counts against host RAM.
- zram and swap are pressure/safety mechanisms, not normal working-memory
  budgets.
- Do not invent a global resource subsystem inside a ticket that defers it.
- Contractually atomic outputs must remain atomic; failure paths must clean all
  resources owned by the current operation without deleting unrelated data.

## 6. Code quality and readability

- Clang and Meson are the reference build tools.
- Do not add global mutable state unless an established historical contract
  explicitly permits it.
- Clean allocations, file descriptors, processes, mutexes, conditions, and
  threads explicitly.
- Do not use `system()` or `popen()` in production where direct process
  execution is required.
- Do not leave TODOs, dead code, or accidental dependencies in a completed
  ticket.
- Prefer minimal evolution to rewrites.
- Target approximately 100 columns; normally do not exceed 120 without local
  justification.
- Do not use code golf or compress multiple logical operations onto a line.
- Split complex code when that materially improves auditability.
- Temporary probes, benchmarks, diagnostics, C/C++/GLSL files, and scripts
  under `/tmp` must also be readable and auditable.
- Comments should explain non-obvious invariants, not paraphrase code.
- Formatting-only changes must not alter behavior.

## 7. External processes and filesystem

- Prefer direct process execution over shell command strings.
- Explicitly own, monitor, and reap child processes.
- Use process groups when required and prevent orphaned children.
- Bound retained stdout/stderr where applicable and drain output when required
  to avoid deadlocks.
- Clean owned resources on every success and failure path.
- Do not assume a CLI flag exists without evidence.
- Authoritative upstream documentation or source may be inspected for external
  dependencies.
- Temporary probes, clones, and builds under `/tmp` are allowed when useful.
- Do not use `sudo`, modify the host persistently, install system packages
  without explicit authorization, or add a permanent repository dependency
  without explicit authorization.

## 8. Documentation discipline

- Do not create duplicate canonical documents for one contract.
- Link every new canonical document under `docs/**` from the root `README.md`.
- For changes to API, architecture, ownership, concurrency, persistence,
  pipeline, or limits, check whether canonical documentation must be updated.
- Documentation must describe proven implementation, not desired future
  behavior.
- Statuses such as PLANNED, IMPLEMENTED, VALIDATION PENDING, and PASS/FROZEN
  are authoritative lifecycle statements: update them only when the required
  implementation, validation, and review evidence supports the state.
- Never mark work PASS/FROZEN without the required validation and review.

## 9. Required validation

For ordinary implementation tickets, before claiming completion, actually run
the applicable commands:

```sh
meson compile -C build -j8
meson test -C build --num-processes 1 --print-errorlogs
git diff --check
```

If build configuration must be regenerated, follow the documented Clang/Meson
setup and do not blindly wipe a valid build tree.

For a new or modified public C header, also run a C17 syntax check, for example:

```sh
cc -x c -std=c17 -fsyntax-only -Iinclude \
  -include lardon3d/<header>.h /dev/null
```

For memory/lifetime-sensitive changes, use a separate ASan/UBSan build and do
not overwrite the normal build. For relevant concurrency changes, run TSan
when supported. If a sanitizer is unavailable or invalid because of the
environment/toolchain, report that explicitly rather than claiming PASS.

Run expensive or stress validation only when relevant, one heavy validation at
a time. Investigate a timeout; do not rerun it until it happens to pass.
Distinguish third-party sanitizer noise from repository defects using concrete
stack and failure evidence. Never claim a command, test, sanitizer, or review
that was not actually performed.

## 10. Delivery report / STOP conditions

Every completed ticket report must include:

- exact files modified;
- concise implementation description;
- validations actually executed and exact results;
- tests or checks not executed;
- known blockers;
- non-blocking findings;
- deliberately deferred/future-scope items;
- confirmation that unrelated and FROZEN areas were preserved.

STOP and request a human decision if:

- requested work contradicts a FROZEN contract;
- satisfying it requires files outside the authorized scope;
- a scientific contract or identity must change without authorization;
- a new subsystem or dependency is required contrary to scope;
- destructive Git action is required;
- incompatible policies remain unsettled by canonical documentation.

Do not stop merely because an ordinary local implementation detail can be
resolved safely from existing code and documentation.
