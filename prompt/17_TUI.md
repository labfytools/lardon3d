# 17 — TUI

## Status

```text
TUI=PRIMARY_CONTROL_SURFACE
USER_INTERFACE_LANGUAGE=ENGLISH
USER_FACING_TUI_LANGUAGE_NORMALIZATION=PASS
```

## Authority

`AGENTS.md`, `README.md`, current TUI architecture and `docs/product/product_definition.md`.

## CURRENT/FROZEN

ncurses remains on its designated/main thread. Reuse the current business-logic / TUI / layout separation and current Queue/Project lifetime boundary.

Validated sizing includes:

```text
full >=100x30
reference compact 72x20
minimum supported 60x15
```

Below minimum, render only the bounded terminal-too-small fallback.

## REQUIRED_PRODUCT_TARGET

The TUI progressively controls:

- project;
- acquisition;
- optics;
- calibration;
- quality;
- pipeline;
- Tasks;
- Governor;
- SSD/scratch;
- reconstructions;
- viewer;
- coverage;
- capture guidance;
- export;
- diagnostics/help.

Displayed actions and actual handlers must remain consistent.

Diagnostics must distinguish at least:

- scientific rejection;
- invalid input;
- missing prerequisite;
- resource wait/throttle;
- runtime failure;
- corruption;
- unsupported version/backend;
- user cancellation.

A generic unowned `failed` state is insufficient for final-product workflows.

Direct TUI/control labels and the user-facing project/import/catalog/runtime-session surface have been normalized to English and validated.

Historical persisted labels, path names and deliberate UTF-8 test fixtures are not renamed merely to satisfy a text scan. Internal subsystem diagnostics are not to be mechanically rewritten across FROZEN or persistence-sensitive boundaries; any diagnostic intentionally exposed to the final user-facing product must converge to English when its owning subsystem is explicitly scoped.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
