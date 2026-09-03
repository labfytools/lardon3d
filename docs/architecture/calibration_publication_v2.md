# Calibration Publication v2

**PASS / FROZEN — additive heterogeneous calibration publication boundary.**

## Authority and scope

Calibration Science v2 is the frozen scientific authority. This document
defines only the additive heterogeneous publication bridge:

```text
caller-owned validated group evidence
-> Calibration Tooling v2
-> L3DCALB2
-> Calibration Bootstrap v2
-> existing SparseCalibrationScope(image_id -> calibration_id)
```

It does not change `L3DCALB1`, Calibration Tooling v1, or Calibration Bootstrap
v1. It does not run a solver, define optical applicability thresholds, resolve
READY outside publication, or add persistence schema.

## L3DCALB2 format

The artifact is deterministic, fixed-width, and little-endian. Integers use
the stated unsigned width. Binary64 fields contain IEEE-754 bits; producers
canonicalize negative zero and importers reject non-finite values. Native
structures, padding, locale text, and trailing bytes are forbidden. The whole
artifact is at most 600,000 bytes, with 1..4,096 groups and 1..4,096 entries;
the byte-size bound may impose a lower combined maximum. SHA-256 fields must be
nonzero.

```text
magic                          8 bytes = ASCII "L3DCALB2"
format_version                 u32 = 2
model_kind                     u32 = PINHOLE(1)
model_version                  u32 = 1
group_count                    u32, 1..4096
entry_count                    u32, 1..4096

for each group in strictly increasing
(group_identity_sha256, group_version) order:
    group_identity_sha256      32 bytes
    group_version              u32, nonzero
    optical_state_sha256       32 bytes
    target_sha256              32 bytes
    solver_executable_sha256   32 bytes
    solver_configuration_sha256 32 bytes
    initialization_evidence_sha256 32 bytes
    validation_evidence_sha256 32 bytes
    member_count               u32, nonzero

    for each member in strictly increasing selected_item_index order:
        selected_item_index    u32
        image_id               u64, nonzero
        representation_sha256  32 bytes
        width, height          u32, u32, nonzero
        fx, fy, cx, cy         f64, f64, f64, f64
        k1, k2, p1, p2         f64, f64, f64, f64
        support_images         u32, nonzero
        support_observations   u32, nonzero
        reprojection_rmse_px   finite nonnegative f64
        maximum_parameter_delta finite nonnegative f64
        validation_flags       u32 = 0x0f
```

The pinhole parameters and four validation bits have exactly the v1 meaning.
V2 introduces no new solver threshold. Positive focal lengths and principal
points inside the declared dimensions are required. Every selected item index
from zero through `entry_count - 1` occurs exactly once across the artifact,
and every `image_id` occurs exactly once. Group order and member order are
validated, not silently normalized.

## Provenance and publication

Each complete serialized group record, including its exact membership and all
group-local evidence, is independently SHA-256 hashed. That group digest is the
`IMPORTED_TRUSTED` provenance fingerprint for each calibration in that group.
The independently supplied whole-artifact SHA-256 protects transport and binds
the combined publication without replacing group-local provenance.

Bootstrap validates the whole artifact and then verifies every entry against
the immutable selected execution's exact item index, `image_id`, and current
representation asset SHA-256 before its first write. It creates or reuses the
existing content-addressed Sparse calibrations per entry, creates or reuses one
scope over every selected image across all groups. The original Bootstrap v2
import attaches that complete scope and retains its existing public behavior.
The separately named `publish_unattached` primitive returns the exact
selected-item/image/calibration mapping without attaching the scope; it exists
only so Workflow v2 can establish v26 compatibility before READY. A
pre-publication error attaches no scope. A later failure may retain immutable
calibration evidence but cannot make the execution READY. Exact retries reuse
calibrations and the scope and converge on the same attachment.

## Heterogeneous Workflow v2 composition

Workflow v2 consumes the L3DCALB2 artifact and a complete caller-owned binding
for every selected item. Each binding repeats the selected execution's durable
`item_index -> capture_id` relation. Capture identity is never recovered from
an image ID, path, SHA-256, filename, artifact group identity, or numeric group
ID.

Before unattached publication, Workflow v2 requires complete observed v26
geometric state for every selected Capture. Automatic resolution reports zero
exact candidates as `CALIBRATION_REQUIRED` and multiple exact candidates as
`SELECTION_REQUIRED`. Explicit existing applicability must be exactly
compatible. Explicit publication creates/reuses deterministic profile metadata
from the immutable artifact calibration and its group-local provenance, then
creates/reuses applicability from the declared exact-state exemplar. It does
not create scientific Capture identity or authorize interpolation.

For every selected item, the final resolved v26 sparse `calibration_id` must
equal that image's calibration member in the complete unattached scope. Only
after all items pass that equality does Workflow v2 attach the scope and return
`READY`. Invalid evidence or a wrong assignment is a distinct non-ready error.
Any earlier failure leaves the selected execution unattached; exact retries are
deterministic and converge through the immutable Bootstrap/profile/
applicability/selection APIs. No Project DB schema change is required.
