# 27 — Tests and Validation

## Status

```text
VALIDATION_STRATEGY=DELTA_BASED
```

## Authority

`docs/development/testing.md`, `AGENTS.md`, global-maintenance evidence and specialized architecture documents.

## REQUIRED

Unchanged PASS/FROZEN boundaries inherit retained evidence unless the delta touches their assumptions.

For unchanged globally reviewed foundations, the retained maintenance baseline is tag `global-maintenance-2026-09-01`; the later `real-a6000-pre-sfm-2026-09-02` checkpoint adds real-data evidence without replacing the maintenance authority. Use the relevant delta rather than replaying a global audit.

For each tranche:

1. identify changed contracts;
2. build with safe host-aware parallelism;
3. run focused unit/integration tests;
4. run relevant persistence/restart/resource/concurrency checks;
5. use ASan/UBSan and TSan only where applicable and preserve documented third-party qualifications;
6. validate real-data only when the tranche explicitly requires it and use non-destructive copies/workspaces;
7. run `git diff --check`.

Do not claim blanket TSan validity for Vulkan unless explicitly proven.

Historical schema downgrade fixtures must be structurally truthful. A fixture targeting schema version N must remove every schema object introduced after N before rewriting `schema_version`; changing metadata alone is not a valid historical fixture. In particular, v25-aware downgrade fixtures must account for the additive v24 `raw_development_batch_tasks` and v25 `feature_extract_batch_tasks` overlays when targeting earlier versions.

Do not rerun full historical A-to-Z qualification merely for confidence theater.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
