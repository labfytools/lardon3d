# 11 — Optics Onboarding

## Status

```text
OPTICS_ONBOARDING=PLANNED
```

## Authority

`docs/product/product_definition.md`; current v23 optical-context architecture and persistence remain authoritative.

## REQUIRED_PRODUCT_TARGET

```text
NEW_CAMERA_REQUIRES_CODE_CHANGE=NO
NEW_LENS_REQUIRES_CODE_CHANGE=NO
ELECTRONIC_LENS_WITH_METADATA=SUPPORTED
MANUAL_LENS_WITHOUT_EXIF=SUPPORTED
MULTIPLE_LENSES_PER_CAMERA=SUPPORTED
ZOOM_MULTIPLE_FOCALS=SUPPORTED
MULTIPLE_OPTICAL_CONFIGURATIONS_PER_PROJECT=SUPPORTED
SILENT_CALIBRATION_SUBSTITUTION=FORBIDDEN
SILENT_LENS_IDENTITY_INFERENCE=FORBIDDEN
OPTICS_TUI_WORKFLOW=REQUIRED
PROFILE_IMPORT_EXPORT=REQUIRED
OPTICS_PROFILE_ONBOARDING_TARGET<=5_MINUTES
```

The time target excludes physical calibration acquisition.

Aliases must be exact and reviewable. Fuzzy metadata matching may assist discovery but may never silently create scientific identity.

Profile import/export must be bounded and versioned, support preview/dry-run, reject incompatible required semantics, preserve identities, and never be implemented as a raw SQLite dump.


## Portable profile encoding decision

The v1 portable equipment-profile encoding is a single deterministic bounded binary container:

```text
OPTICS_PROFILE_PORTABLE_FORMAT=L3DOPRF1
OPTICS_PROFILE_PORTABLE_VERSION=1
BYTE_ORDER=LITTLE_ENDIAN
NATIVE_STRUCT_SERIALIZATION=FORBIDDEN
RAW_SQLITE_EXPORT=FORBIDDEN
UNBOUNDED_JSON=FORBIDDEN
```

`L3DOPRF1` carries camera-body profiles, lens profiles, explicit aliases and optical configurations. All integers/floating-point fields use explicit fixed widths; variable UTF-8 strings and record arrays are length/count-prefixed and must have explicit hard maxima in the public format contract before parser implementation. Unknown required semantics or unsupported versions are rejected before database mutation.

Calibration scientific payloads are not re-encoded as profile data. Portable calibrations remain exact `L3DCALB1` artifacts and continue through Calibration Bootstrap v1. The optics-profile import preview may report compatible/missing calibration requirements but may not synthesize or merge calibration.

Import is two-phase:

```text
bounded parse + validation + conflict preview
-> explicit human acceptance
-> one failure-atomic database mutation
```

Conflicting profile identities are never silently merged.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
