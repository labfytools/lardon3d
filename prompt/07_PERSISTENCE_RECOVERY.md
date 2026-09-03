# 07 — Persistence and Recovery

## Status

```text
PERSISTENCE_RECOVERY=REQUIRED
```

## Authority

`docs/architecture/persistence.md`, `docs/architecture/project_database.md`, `docs/architecture/runtime.md`, `docs/architecture/task_system.md`.

## FROZEN

Existing persistence semantics, task recovery and schema lineage are consumed as-is.

## REQUIRED_PRODUCT_TARGET

New long-running product stages must be:

- durable;
- restartable;
- failure-atomic;
- bounded;
- explicit about immutable inputs and published generations;
- recoverable without private solver/process state.

Do not persist transient library internals merely to resume work.

External-process orchestration must durably preserve enough exact input/output identity and checkpoint state to restart safely.

### Project portability

The durable project must not depend on:

- absolute temporary paths;
- current CPU topology;
- current GPU device;
- current external-scratch mount path;
- a live camera connection;
- a previous process ID;
- a specific Task worker instance.

Hardware-specific operational state is rediscovered; scientific identities remain stable.

### Recovery cases

Final-product long-running stages must define recovery behavior for application close, system reboot, Task cancellation, process crash, external-process failure, safe-drained scratch disconnect and partial physical-asset publication where existing orphan semantics permit it.

Restart must never infer the scientific input from filename, timestamp or another heuristic.

## Schema rule

```text
NEW_PROJECT_DB_VERSION_REQUIRES_HUMAN_AUTHORIZATION
NEW_PRODUCTION_TASK_KIND_REQUIRES_HUMAN_AUTHORIZATION
```

A planned capability does not itself grant that authorization.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
