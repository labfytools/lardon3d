# 06 — Identity Model

## Status

```text
IDENTITY_DISCIPLINE=REQUIRED
```

## Authority

`AGENTS.md`, persistence/project database documents, Capture/provenance and scientific model documents.

## FROZEN

Never silently equate:

```text
Capture != file
Capture != asset
Capture != image_id
Capture != SHA-256
Capture != path
Capture != filename/basename
Capture != Task ID
Capture != campaign group ID
Task ID != scientific acquisition identity
campaign group ID != Capture identity
```

Current meanings:

- SHA-256: immutable asset bytes.
- `capture_id`: physical acquisition representation in Project DB.
- `image_id`: scientific image representation.
- Task ID: durable operational work.
- campaign group ID: stable operational grouping within an acquisition request.

Future Dense, mesh, texture, coverage, registration, live-promotion and video-keyframe identities must be explicit, versioned, durable and provenance-linked. They must not be inferred from paths or timestamps.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
