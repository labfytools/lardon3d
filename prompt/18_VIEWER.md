# 18 — Viewer

## Status

```text
VIEWER=PLANNED
VIEWER_ROLE=PASSIVE_SNAPSHOT_CONSUMER
VIEWER_CAN_BE_DISABLED=REQUIRED
VIEWER_BLOCKS_ENGINE=NO
```

## Authority

`docs/architecture/viewer.md` plus `docs/product/product_definition.md`.

## REQUIRED_PRODUCT_TARGET

The graphical viewer is required but does not replace the TUI.

It never owns mutable scientific truth, Task scheduling or Resource Governor policy.

It must be able to slow down, drop frames, close or crash without corrupting or blocking the engine.

Target visualization includes:

- sparse landmarks;
- dense cloud;
- mesh;
- textured mesh;
- registered cameras/frustums;
- reconstruction components;
- Track support;
- reprojection diagnostics;
- campaign/ScanSet contribution;
- coverage heatmap;
- weak/unseen areas and holes;
- live camera pose;
- suggested capture targets.

Required interaction includes orbit/pan/zoom, reset/focus, element/region selection, camera selection, visibility toggles, diagnostic inspection, valid-scale measurement and region-of-interest selection for coverage/guidance.

Visual snapshot buffering must remain bounded. When the viewer falls behind, obsolete snapshots are dropped rather than accumulated without bound.

Viewer annotations do not silently become scientific input. They may enter science only through an explicit supported operation such as a future control-point registration workflow.

Functional isolation is mandatory. A separate Unix process is allowed if it improves isolation without duplicating authority.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
