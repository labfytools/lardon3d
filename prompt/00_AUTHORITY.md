# 00 — Authority

## Status

```text
AUTHORITY_CONTRACT=REQUIRED
```

## Authority

Primary authority is the repository on canonical branch `main`.

Read and apply, in order of role rather than by blindly overriding specialized contracts:

- `AGENTS.md`: global agent obligations and repository limits.
- `docs/product/product_definition.md`: final product destination.
- specialized `docs/architecture/**`: acquired scientific, persistence, runtime, identity and resource contracts.
- `docs/roadmap/roadmap.md`: lifecycle and current sequencing.
- `README.md` and `docs/README.md`: current navigation and summary.
- historical audits/tags/checkpoints: evidence for their own checkpoint only.
- executable code/schema: authority when exact APIs, constants, DDL or runtime behavior are being quoted.


## CURRENT

The canonical working branch is `main`.

```text
CANONICAL_WORKING_BRANCH=main
CURRENT_STATE_AUTHORITY=main
OTHER_BRANCHES_ARE_NOT_AUTHORITY
```

## FROZEN

A specialized PASS/FROZEN contract is not superseded by this prompt tree. This tree references and constrains execution around those contracts; it does not duplicate them.

## Git authority

The local working tree is the current working authority. GitHub (`github`) and Forgejo (`origin`) are mirrors of the same project, not independent variants.

After an explicitly authorized Git closure:

```text
local main
  -> github/main
  -> origin/main
```

should normally identify the same commit.

## Required Git inspection before Git-sensitive work

```sh
git branch --show-current
git status --porcelain
git remote -v
git rev-parse HEAD
git rev-parse github/main
git rev-parse origin/main
```

If a remote is missing, inaccessible, or unexpectedly divergent: STOP. Do not choose one mirror as the winner by assumption.
