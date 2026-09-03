# 24 — Video and Deterministic Keyframes

## Status

```text
VIDEO_INGESTION=PLANNED
KEYFRAME_SCIENCE=NOT_YET_FROZEN
```

## Authority

`docs/product/product_definition.md`.

## REQUIRED_PRODUCT_TARGET

Video is an acquisition source, not a second SfM pipeline.

```text
SOURCE video asset
-> deterministic timeline/frame identity
-> bounded deterministic keyframe extraction
-> quality/blur/redundancy analysis
-> explicit selected keyframes
-> normal Capture/provenance
-> existing scientific pipeline
```

Every retained keyframe must preserve:

- source video asset identity;
- exact frame/timestamp identity;
- extraction algorithm/version;
- parameter fingerprint.

## STOP

Define and validate the versioned keyframe scoring/selection science before implementation. Do not hide heuristic thresholds in unversioned code.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
