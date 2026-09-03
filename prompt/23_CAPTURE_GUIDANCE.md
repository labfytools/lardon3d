# 23 — Capture Guidance

## Status

```text
CAPTURE_GUIDANCE=PLANNED
AUTO_CAPTURE=IDEA
```

## Authority

`docs/product/product_definition.md` and the future FROZEN coverage/localization contracts.

## REQUIRED_PRODUCT_TARGET

Guidance must transform a weakness into an actionable suggestion containing, where available:

- target region;
- suggested direction;
- viewpoint/position zone;
- angle;
- distance;
- baseline;
- expected evidence improvement;
- confidence;
- reason.

Target loop:

```text
existing reconstruction
-> coverage analysis
-> weak/unseen target
-> live camera localization
-> project target into live view
-> guide operator
-> operator acquires full-resolution capture
-> normal ingestion
-> normal incremental/re-registration scientific update as allowed by existing lineage contracts
-> coverage refresh
```

Minimum live localization states:

```text
UNAVAILABLE
SEARCHING
LOCALIZED
LOW_CONFIDENCE
LOST
```

Never display a stale pose as current after tracking loss without an explicit stale/lost indication.

Guidance must be actionable in operator terms where applicable: left/right/up/down, closer/farther, rotate toward/away from target, change incidence angle and change baseline.

Weak/unseen semantics must not rely on color alone.

When localization or coverage confidence is insufficient, do not assert a precise overlay or a certain `capture here` instruction. Fall back to a truthful search/diagnostic state.

For v1, capture remains user-triggered.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
