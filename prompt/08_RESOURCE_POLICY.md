# 08 — Resource Policy

## Status

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

## Authority

`AGENTS.md`, `docs/architecture/resource_governor.md`, `docs/architecture/internal_parallelism.md`, `docs/architecture/resource_aware_pipeline.md`.

## FROZEN

First preserve normal interactive workstation use. Then use all remaining resources that are both safe and useful.

Reference-host evidence only:

```text
16 logical CPUs
~4 logical CPUs interactive reserve
~12 logical CPUs compute
~3 GiB MemAvailable hard reserve
Radeon 780M UMA
```

These are not product constants.

## Rules

- per-item atomicity does not imply cross-item serialization;
- owner-only publication does not imply serial preparation;
- bounded internal fan-out is expected when independent useful work exists;
- all participants are bounded, accounted, cancellable and joined;
- UMA is charged once against host RAM;
- swap and zram are pressure mechanisms, not admitted RAM;
- scratch is storage, not RAM;
- pressure may reduce admission;
- safe useful capacity must be readmitted when pressure clears;
- I/O or measured scaling knees may justify under-utilization;
- fixed historical `CPU1`, `-j8`, batch sizes or worker counts never become universal policy;
- normal users observe but do not need to select authoritative CPU width, batch/window, inflight depth, participant count, GPU backend, scratch mode or RAM budget;
- no stage may introduce an unbounded whole-project load merely for implementation convenience.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
