# Lardon3D — mandatory agent guardrails

## Authority
- Current explicit user instructions have highest authority.
- Canonical repository documentation and contracts outrank memory, assumptions, comments and convenience.
- Memory is advisory only. Never use memory to override repository truth.
- Before editing code governed by a documented contract, read that contract.

## FROZEN and scope
- Anything marked FROZEN is read-only unless the user explicitly authorizes changing that exact contract.
- Never reinterpret, weaken, migrate or "clean up" a FROZEN contract.
- Keep every modification inside the explicitly authorized task and file scope.
- No opportunistic refactoring.
- Do not add dependencies, subsystems, persistence, schemas, services, public API/ABI changes or architectural layers unless explicitly authorized.

## STOP
Stop and report instead of guessing if:
- the task conflicts with a FROZEN contract;
- canonical documents materially disagree;
- scope is ambiguous;
- the required solution crosses authorized scope;
- a new dependency/subsystem/architecture change appears necessary;
- existing user modifications make a safe edit uncertain.

## Codex quota
- Never switch, fall back or escalate to Codex automatically.
- Codex is an explicit user-controlled escalation only.
- Prefer local tools, clangd, Git, context-mode and local subagents whenever sufficient.
- Subagents are local-only and read-only by default.
- Do not spend Codex quota merely to run, watch or summarize deterministic tests.

## Editing
- The parent agent is the only code writer by default.
- Make the smallest correct change satisfying the contract.
- Preserve validation, error handling, safety, determinism and test coverage.
- Do not modify generated, vendor or third-party files unless explicitly required.

## Git
- Inspect `git status --short` before editing.
- Never discard or overwrite user changes.
- Never reset, clean, restore/checkout user work, rebase, merge, commit, amend, tag or push unless explicitly requested.
- Before final validation inspect changed files, scope and `git diff --check`.

## Validation
- Determine authoritative build/test instructions from repository evidence.
- Use LSP diagnostics as intermediate feedback only.
- Deterministic build/tests/sanitizers are the final technical validation.

## Tool discipline
- Do not use any tool when the user's request can be answered from the current conversation and already-loaded context.
- Files listed by Pi under `[Context]` are already available. Never call `read` merely to re-read `.pi/APPEND_SYSTEM.md`, `~/AGENTS.md` or `AGENTS.md`.
- Use repository tools only when the task genuinely requires repository evidence or inspection.
- Do not call tools for acknowledgements, simple questions, formatting requests or answers explicitly requested without tools.
- Never read the same file repeatedly in one turn unless its contents changed or a different range is explicitly required.
- Prefer the minimum number of tool calls needed to answer the request.
