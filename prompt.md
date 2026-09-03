# Lardon3D — Final Execution Contract

This file is the entry point for the collective Lardon3D execution contract.

```text
CONTRACT_SCOPE=ENTIRE_PROMPT_TREE
CONTRACT_LANGUAGE=ENGLISH
WORK_FIRST_RETURN_LAST=REQUIRED
CANONICAL_WORKING_BRANCH=main
CURRENT_STATE_AUTHORITY=main
GIT_CLOSURE_OWNER=HUMAN
IMPLEMENTATION_AUTHORIZATION=NO
```

## Mandatory reading order

Before any major implementation tranche, read:

1. `AGENTS.md`
2. `README.md`
3. `docs/README.md`
4. `docs/product/product_definition.md`
5. `docs/roadmap/roadmap.md`
6. every file under `prompt/` in numeric order from `00_AUTHORITY.md` through `33_DEFINITION_OF_DONE.md`
7. the specialized canonical `docs/architecture/**` documents cited by the tranche

The files under `prompt/` form one collective contract. No child file may be interpreted in isolation when another child file defines a complementary constraint.

## Absolute rules

- Preserve all applicable PASS/FROZEN scientific and architectural boundaries.
- Never invent calibration, lens identity, metric scale, campaign equivalence, provenance, or scientific identity.
- Never create a Project DB version or production Task Kind without explicit human authorization.
- Never create a second Task Runtime, Queue, scheduler, Resource Governor, generic executor, or backend framework merely for convenience.
- Never modify `scan3d/` without explicit human authorization.
- Never use retained real scientific projects as destructive scratch/test workspaces.
- Never reinterpret historical checkpoints as current state.
- Never treat GitHub and Forgejo as different project authorities; they are mirrors of the same canonical `main`.
- Never stage with `git add -A`.
- Never commit or push unless the human explicitly authorizes Git closure.
- Never force-push unless the human explicitly authorizes that exact operation.

## STOP

STOP before any change that would:

- reopen a PASS/FROZEN boundary;
- change a FROZEN scientific threshold or identity;
- introduce pseudo-calibration or silent calibration substitution;
- introduce an unauthorized Project DB schema version;
- introduce an unauthorized production Task Kind;
- create an unnecessary parallel runtime/scheduler/backend framework;
- modify `scan3d/`;
- perform a destructive operation;
- create/switch/merge/rebase branches without authorization;
- resolve an unexplained mirror divergence by overwriting one remote;
- contradict executable code/schema and a canonical contract;
- resolve a material product ambiguity by assumption.

A STOP report contains only the relevant authority, the exact contradiction, and the minimum human decision required.

## Execution discipline

For an authorized tranche:

1. identify the governing authority;
2. inspect only the context needed to execute correctly;
3. define the exact file and subsystem scope;
4. implement the complete tranche;
5. run proportional delta-based validation;
6. fix defects within scope;
7. run `git diff --check`;
8. report only after the tranche is complete or a real STOP condition is reached.

Do not return after each file. Do not repeat global A-to-Z audits when unchanged FROZEN systems already have retained evidence.

## Current head assumptions

This contract was prepared against the repository state that declares:

```text
CURRENT_PROJECT_DB_SCHEMA=v27
PRODUCTION_TASK_KINDS=16
PRODUCT_DEFINITION_V1=PASS/FROZEN
PROMPT_TREE=NEXT
REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Before implementation, verify these assumptions against current `main`. If current `main` has legitimately advanced, update only the CURRENT statements that are stale; never rewrite historical or FROZEN meaning silently.
