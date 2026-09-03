# 25 — External SSD and Scratch

## Status

```text
EXTERNAL_SSD_CONTROLLER=VALIDATED
TASK_SCRATCH_CONSUMERS=PLANNED
PROJECT_SCRATCH_OPT_IN=REQUIRED
SWAP_OPT_IN=REQUIRED
```

## Authority

Current SSD controller/resource documents and `docs/product/product_definition.md`.

## REQUIRED_PRODUCT_TARGET

Expose:

- physical device identity;
- mount state;
- scratch state;
- swap state;
- capacity/usage;
- active leases;
- drain state;
- safe-to-unplug state.

Project scratch use is explicit opt-in. Swap enable/disable is also explicit user choice when supported by the validated controller. A newly connected device is never adopted automatically.

Dense/mesh/refinement/texturing scratch consumers acquire storage only through the existing Governor-owned scratch lease boundary.

```text
SCRATCH!=RAM
SWAP!=RAM
```

## REJECTED

Automatic destructive repartitioning, formatting, destructive fsck, overwriting unknown filesystems, or force-unmounting an active lease.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
