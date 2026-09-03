# 30 — Git, Scope and STOP

## Status

```text
GIT_CLOSURE_OWNER=HUMAN
PROTECTED_SCOPE=scan3d/
```

## Authority

`AGENTS.md` and explicit human ticket scope.

## Git rules

Without explicit human authorization, do not:

- stage;
- commit;
- push;
- create/switch branches or continue development from a detached historical tag/checkpoint;
- merge;
- rebase;
- reset;
- restore;
- stash;
- clean;
- amend;
- force a Git operation.

Never use `git add -A`.

If Git closure is explicitly authorized, stage only exact tranche paths and synchronize both mirrors as a pair unless the human explicitly says otherwise.

## Protected scope

- do not modify `scan3d/`;
- do not modify other repositories;
- do not use retained real scientific projects destructively.

## STOP conditions

STOP before:

- reopening PASS/FROZEN;
- changing FROZEN science/identity;
- pseudo-calibration;
- unauthorized DB schema or Task Kind;
- unnecessary generic runtime/backend subsystem;
- destructive operations;
- protected-scope edits;
- unauthorized branch operations;
- force push;
- unilateral mirror overwrite;
- real contradiction between executable code/schema and canonical contract;
- material unresolved product ambiguity.

Difficulty alone is not a STOP condition.
