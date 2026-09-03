# 09 — Task, Queue and Governor

## Status

```text
TASK_RUNTIME_AUTHORITY=SINGLE
QUEUE_AUTHORITY=SINGLE
RESOURCE_GOVERNOR_AUTHORITY=SINGLE
```

## Authority

`docs/architecture/task_system.md`, `task_queue.md`, `task_kind_registry.md`, `resource_governor.md`, `scheduler_resource_integration.md`.

## CURRENT/FROZEN

The current production Queue executes one active callback at a time. Reuse the existing Task -> Queue -> Resource Governor ownership model.

Do not introduce:

- a second scheduler;
- a second production queue;
- a generic executor;
- an uncontrolled worker pool;
- a parallel resource authority.

General inter-Task parallelism remains deferred unless a future dependency and correctness proof requires it.

Bounded internal parallelism within one admitted Task is allowed and expected when scientifically independent work exists and publication semantics remain deterministic.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
