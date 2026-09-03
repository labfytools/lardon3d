# 19 — Live Capture Foundation

## Status

```text
LIVE_CAMERA_SOURCE=PLANNED
```

## Authority

`docs/product/product_definition.md`; Capture/provenance contracts remain authoritative downstream.

## REQUIRED_PRODUCT_TARGET

Create one generic live-acquisition adapter boundary before device-specific integrations.

An adapter may own:

- discovery;
- connection;
- preview transport;
- optional shutter/control;
- metadata retrieval;
- full-resolution file transfer;
- disconnect/reconnect.

It may not redefine scientific identities, calibration, Features, Tracks, reconstruction or resource accounting.

Preview frames are ephemeral observations until explicitly promoted through the supported Capture/provenance path.

Heavy reconstruction compute remains PC-side.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
