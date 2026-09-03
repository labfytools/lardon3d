# 28 — Documentation and Comments

## Status

```text
DOCUMENTATION_LANGUAGE=ENGLISH
SOURCE_COMMENT_LANGUAGE=ENGLISH
```

## Authority

`AGENTS.md` repository language policy.

## REQUIRED

Current repository technical prose and production source comments converge on English.

Comments should document mainly:

- why;
- contract;
- invariant;
- ownership/lifetime;
- identity;
- persistence ordering;
- restart;
- cancellation;
- concurrency;
- resource accounting;
- FROZEN boundaries.

Do not comment obvious statements line by line.

Historical documents may be translated, but translation must not silently modernize their lifecycle, schema version, identities, numbers, evidence or conclusions.

This prompt tree must not become a duplicate shadow architecture. Reference canonical documents and restate only execution-critical invariants.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
