# 26 — Performance

## Status

```text
PERFORMANCE_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
```

## Authority

`AGENTS.md`, resource-governor and internal-parallelism documents, plus measured tranche-specific evidence.

## REQUIRED

Performance work is evidence-driven and must preserve science, identity, publication order and boundedness.

Use host-aware build/test parallelism and Governor-aware production execution.

Do not freeze reference-host values such as `12` CPUs, `-j8`, a historic batch size, or `--num-processes 1` as universal policy.

A long serial path with independent work and safe idle resources is a defect unless there is concrete proof of:

- true dependency/scientific serialism;
- useful-scaling knee;
- memory bound/pressure;
- I/O saturation;
- validated GPU execution;
- unavoidable deterministic publication constraint;
- another measured bottleneck.

Validated useful GPU backends are preferred automatically when eligible. Backend failure/fallback behavior must be explicit where the scientific stage supports fallback; do not silently change scientific identity.

Optimize useful throughput, not utilization graphs.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
