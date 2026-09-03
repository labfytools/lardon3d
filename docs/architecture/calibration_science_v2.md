# Calibration Science v2 — heterogeneous optics and adaptive acquisition

**Status: PASS/FROZEN — generic additive scientific contract.**

Calibration Science v1 remains `PASS/FROZEN`. Science v2 does not weaken,
reinterpret or retroactively replace v1 evidence. It defines the next
scientific generation required by the product requirement that a Lardon3D
project may contain photographs from heterogeneous cameras, lenses, focal
configurations and normal autofocus operation.

No device-specific autofocus envelope is validated by this document. Numeric
focus-domain limits require dedicated physical evidence.

```text
CALIBRATION_SCIENCE_V2=PASS/FROZEN
```

This freezes the generic heterogeneous-optics, adaptive-exposure and
applicability contract only. It does not validate a device-specific optical
state, focus envelope or calibration. In particular,
`A6000_E_PZ_16_50_AF_APPLICABILITY=BLOCKED_BY_PHYSICAL_VALIDATION` remains
unchanged until retained physical evidence satisfies the applicable rules.

## 1. Product/science problem

A photogrammetry product must not require the operator to remember a rigid
exposure recipe or split one physical subject into artificial projects merely
because acquisition state changed.

The required user experience is:

```text
user frames and captures
-> Lardon3D records/observes acquisition state
-> Lardon3D measures actual image quality
-> Lardon3D resolves exact calibration applicability
-> Capture remains usable when quality is good
-> reconstruction becomes READY only when calibration coverage is complete
```

A normal supported camera body or rectilinear lens is equipment data. A new
brand/model name alone must not require source-code modification.

Source changes are justified only by a genuinely unsupported transport,
encoded representation, geometric camera model or required device-state
mechanism.

## 2. Compatibility with Science v1

The following remain frozen compatibility contracts:

```text
CALIBRATION_SCIENCE_V1=PASS/FROZEN
CALIBRATION_TOOLING_V1=PASS/FROZEN
CALIBRATION_BOOTSTRAP_V1=PASS/FROZEN
CALIBRATION_WORKFLOW_V1=PASS/FROZEN
```

Historical S21 and A6000 Engine Bay campaigns remain:

```text
CALIBRATION_UNAVAILABLE
BLOCKED_BY_KNOWN_CALIBRATION_DATA
```

Science v2 does not authorize retro-calibration by inference.

## 3. Fundamental separation of states

Science v2 separates three concepts that must never be conflated.

### 3.1 Capture existence

A Capture is durable acquisition evidence. Unsupported or unresolved optics do
not erase it.

### 3.2 Photo quality

Quality describes whether the decoded representation is useful image evidence.
It is evaluated from the image itself and may include:

- sharpness;
- defocus;
- motion blur;
- black/white clipping;
- usable shadow/highlight information;
- contrast;
- texture;
- noise or local signal-to-noise evidence when implemented;
- downstream feature usability.

Photo Quality may recommend `GOOD`, `SUSPECT` or `REJECT` without assigning any
calibration identity.

### 3.3 Calibration readiness

Calibration readiness answers whether a selected scientific representation has
exact compatible geometric calibration evidence.

Therefore this is valid:

```text
QUALITY=GOOD
CALIBRATION=CALIBRATION_REQUIRED
```

The image remains in the project.

## 4. Heterogeneous selected executions

A single project and one selected scientific execution may contain images from:

- different camera bodies;
- different lenses;
- different focal configurations of one zoom;
- different validated focus states or focus applicability domains;
- different validated aperture applicability states;
- different acquisition sessions.

The scientific requirement is per selected image:

```text
selected image
-> exactly one compatible calibration
```

It is not:

```text
selected execution
-> one global calibration
```

A truthful READY state requires complete calibration coverage of the exact
selected-image set.

The current Sparse calibration scope already represents members as
`image_id -> calibration_id`; v2 should reuse that per-image model unless
repository inspection proves a real missing invariant.

## 5. No silent substitution

Science v2 preserves:

```text
NO_SILENT_SCIENTIFIC_SUBSTITUTION
```

The following are forbidden unless a later explicit validated v2 transfer model
authorizes them:

- another camera body's calibration;
- another lens's calibration;
- a nearby focal calibration;
- a guessed focus calibration;
- a guessed aperture compatibility;
- a guessed stabilization/crop/pipeline equivalence;
- fuzzy metadata matching that silently changes scientific identity;
- interpolation or extrapolation outside a validated model/domain.

If exact compatibility cannot be proven:

```text
CALIBRATION_REQUIRED
```

If more than one valid choice remains unresolved:

```text
SELECTION_REQUIRED
```

## 6. Exposure and photometric controls

Purely photometric controls are adaptive acquisition variables.

Science v2 does not reject an image merely because of the numeric value of:

- shutter speed;
- ISO;
- white balance;
- exposure compensation;
- other controls proven not to change image geometry.

Examples such as `1/125`, `ISO 800` or `f/8` from historical campaigns are
observations, not universal validity thresholds.

For shutter/ISO/white balance, actual decoded-image evidence decides whether
the result is usable. A faster shutter with higher ISO may be preferable to a
blurred low-ISO image; a slower shutter may be valid when the camera is stable.

Missing photometric metadata must not cause fabricated values.

## 7. Aperture

Aperture is not automatically classified as purely photometric because some
lenses can exhibit focus shift or geometric changes with aperture.

The product goal remains that the operator does not manually manage scientific
grouping.

Science v2 must allow either:

1. one calibration applicability proven valid across an aperture domain; or
2. distinct automatically classified aperture applicability states.

Unknown compatibility produces `CALIBRATION_REQUIRED`, not Capture rejection.

No aperture-domain threshold is frozen without physical validation.

## 8. Focal length and zoom

Multiple focal configurations may coexist in one project and one selected
execution.

Exact calibrated focal states are the baseline v2 mechanism.

Electronic focal metadata may select an exact compatible state only when its
identity and trust rules are satisfied.

Manual lenses or missing metadata remain normal product paths through explicit
bounded state assignment.

Calibration at one focal state is never silently borrowed for another focal
state.

A future continuous focal transfer/interpolation model is allowed only if it is
a separately versioned Science v2 model with fit-domain evidence, hold-out
validation, deterministic evaluation and hard rejection outside its validated
domain.

## 9. Autofocus and focus applicability

Autofocus is a normal acquisition mode in Science v2.

Normal product use must not require the user to lock manual focus for every
project merely to satisfy calibration.

Science v2 supports the following scientific concepts:

### 9.1 Exact focus state

A calibration may apply to one exact observed/declared focus state.

This remains useful for manual lenses, fixed-focus systems and strict
measurements.

### 9.2 Validated focus applicability domain

A calibration may apply across a bounded autofocus/focus domain only when
physical evidence proves that applicability.

Required evidence includes:

- exact camera body and objective identities;
- exact focal/geometric pipeline state;
- explicit focus-state observation representation and provenance;
- repeated physical calibration samples across the intended focus domain;
- independent hold-out focus states;
- comparison in image-space geometric error, not only parameter percentage;
- bounded deterministic applicability;
- explicit rejection outside the validated domain.

Historical EXIF/MakerNote values such as Sony `FocusPosition2` or derived
`FocusDistance2` may inform experiment design. They are not by themselves
physical calibration evidence.

### 9.3 Discrete focus bands

If one calibration is not valid across the complete autofocus range, v2 may
use multiple validated discrete focus bands or exact focus states.

Automatic classification is required where trusted observed state permits it.

Ambiguous overlap must not silently choose a calibration.

### 9.4 Continuous focus transfer models

Continuous interpolation across focus is not assumed.

If future physical evidence justifies it, such a model must be:

- explicitly versioned;
- deterministic;
- bounded to a validated domain;
- fitted from retained physical evidence;
- tested on excluded hold-out focus states;
- rejected outside the domain;
- incorporated into scientific identity.

No extrapolation is silent.

## 10. Stabilization, crop, orientation and processing pipeline

Only state capable of changing geometry belongs to calibration applicability.

Examples requiring explicit treatment unless equivalence is proven:

- electronic stabilization;
- geometric crop;
- perspective correction;
- resize;
- computational geometry correction;
- in-camera lens correction that changes pixel coordinates;
- orientation normalization.

Purely cosmetic color processing does not create a new geometric state after
coordinate equivalence has been proven.

Unknown geometric state is incompatible until resolved.

## 11. Observed optical state and provenance

Science v2 requires a bounded, versioned observed state sufficient to decide
calibration applicability.

Conceptually it may contain:

```text
camera_body_identity
lens_identity
focal_state
focus_state_or_domain_observation
aperture_state_when_geometrically_relevant
stabilization_and_geometric_pipeline_state
decoded_oriented_width_height
crop_resize_transform_identity
representation/decode_pipeline_identity
observation_provenance
```

The exact public ABI and persistence layout are implementation-contract work.

Important rules:

- unknown is explicit;
- unknown is never replaced with a guessed default;
- electronic metadata is evidence with defined trust, not automatic truth for
  every scientific field;
- manual assignment is explicit and durable where metadata cannot identify a
  state;
- exact retry is idempotent.

## 12. Automatic calibration resolution

For each selected image:

```text
load exact Capture and representation
-> load/derive trusted observed optical state
-> enumerate validated compatible calibration applicability
-> exactly one: calibration resolved
-> none: CALIBRATION_REQUIRED
-> multiple unresolved: SELECTION_REQUIRED
```

A selection must remain reviewable and durable.

Calibration readiness does not rewrite Photo Quality evidence.

## 13. Artifact / Tooling / Bootstrap evolution

Science v2 does not require a new artifact merely because a project is
heterogeneous.

Implementation must first inspect and reuse existing primitives.

Current architecture already has:

```text
SparseCalibrationScope member = image_id -> calibration_id
L3DCALB1 entry = per-image intrinsics
```

However Tooling/Workflow v1 binds one global optical-state/provenance bundle.
If heterogeneous independent calibration groups cannot be represented without
losing provenance, an additive `L3DCALB2` / Tooling v2 / Bootstrap v2 may be
defined.

Any v2 artifact must preserve per-group:

- optical/applicability identity;
- target identity;
- solver executable/configuration identity;
- initialization evidence;
- validation evidence;
- model/dimensions;
- exact published intrinsics;
- exact selected-image membership.

V1 artifact semantics remain unchanged.

## 14. READY invariant

A selected execution is READY only if:

- every selected image is covered exactly once;
- every calibration is scientifically compatible with that image's observed
  geometric state;
- the final calibration scope covers the exact selected image set;
- no unresolved or ambiguous selected image remains;
- publication/attachment follows the existing atomicity/idempotence contract.

A project may retain any number of other non-selected or
`CALIBRATION_REQUIRED` Captures.

## 15. Device independence

Normal equipment onboarding is data-driven.

Required product properties:

```text
NEW_CAMERA_REQUIRES_CODE_CHANGE=NO
NEW_LENS_REQUIRES_CODE_CHANGE=NO
MULTIPLE_LENSES_PER_CAMERA=SUPPORTED
ZOOM_MULTIPLE_FOCALS=SUPPORTED
MULTIPLE_OPTICAL_CONFIGURATIONS_PER_PROJECT=SUPPORTED
```

A genuinely different geometric model, unsupported file format or unsupported
transport may require new code. A different manufacturer/model string does not.

## 16. Acquisition control philosophy

Science v2 defines what must be observed and proven; it does not require one
specific control transport.

Future adapters should follow:

```text
if hardware permits control:
    Lardon3D may choose/control settings automatically
else if state can be observed:
    Lardon3D verifies/classifies automatically
else:
    request only the minimum explicit user action required
```

HDMI, USB remote control and device-specific adapters remain separate future
implementation layers.

## 17. A6000 + E PZ 16-50 current evidence

Historical A6000 engine-bay images provide operational design evidence only.

Observed facts include:

- E PZ 16-50mm F3.5-5.6 OSS;
- heavy real use of 16-20 mm focal lengths;
- AF-S across the historical JPEG set;
- many distinct focus positions even within the 16 mm / f/8 subset;
- exposure values varied with scene conditions.

These observations demonstrate that a locked-focus single-state workflow is
not an acceptable general product UX for this use case.

They do not validate any reusable calibration or autofocus envelope.

Current device-specific state:

```text
A6000_E_PZ_16_50_AF_APPLICABILITY=BLOCKED_BY_PHYSICAL_VALIDATION
```

## 18. Required future physical study

Before claiming a real validated autofocus domain, capture dedicated ChArUco
evidence across representative focus states and repeats while preserving the
other relevant geometric state.

The study must determine whether the lens supports:

- one validated focus envelope;
- multiple discrete focus bands;
- exact focus-only calibration; or
- a future explicitly validated continuous transfer model.

Numeric domains and thresholds must come from the Science v2 validation design
and retained physical evidence. They are not inferred from historical scene
photos.

## 19. Implementation dependency

Current order:

```text
Calibration Science v2 design
-> heterogeneous optical-state persistence/applicability foundation
-> v2 Tooling/Bootstrap/artifact only if required
-> heterogeneous Workflow v2 READY proof
-> physical AF/optical applicability study
-> dedicated calibrated real campaign
-> real Sparse SfM
```

Do not start Sparse SfM or Dense/MVS as part of this design tranche.

## 20. STOP conditions

Stop rather than guess if:

- a required geometric state cannot be observed or explicitly assigned;
- a proposed compatibility relation lacks physical validation;
- an AF/focal/aperture transfer model needs an invented threshold;
- a new schema would force inference into historical rows;
- heterogeneous calibration would require silently changing a FROZEN v1
  scientific meaning;
- a new camera requires a genuinely different geometric model not covered by
  the current pinhole/distortion science.

Uncertainty remains visible as `CALIBRATION_REQUIRED` or
`SELECTION_REQUIRED`.
