# Calibration AF Study Evidence v1

**Status: PASS / FROZEN.**

This boundary is an offline scientific-evidence helper for Calibration Science
v2. It does not perform camera calibration, Project DB mutation, autofocus
control, calibration selection, or physical-validity decisions.

## Purpose

Project DB v27 can persist a physically validated exact-token focus domain, but
it deliberately does not decide whether a set of focus observations is
physically compatible.

`Calibration AF Study Evidence v1` supplies the missing measurement artifact:

```text
independent calibration results at observed focus states
-> deterministic pairwise image-space projection deltas
-> L3DAFST1 artifact
-> SHA-256 retained evidence
-> later human/scientific applicability decision
-> v27 focus-domain creation only after that decision
```

The real A6000 + E PZ 16-50 autofocus applicability remains:

```text
A6000_E_PZ_16_50_AF_APPLICABILITY=BLOCKED_BY_PHYSICAL_VALIDATION
```

This tool cannot change that state by itself.

## Input contract

The caller supplies one exact study context:

- nonzero SHA-256 of the retained body/lens/focal/non-focus geometric context;
- decoded/oriented width and height;
- 2..64 already acquired calibration samples.

Each sample contains:

- `FIT` or `HOLDOUT` role;
- one bounded, nonempty opaque exact focus token;
- nonzero SHA-256 of that calibration's retained evidence;
- exact binary64 `fx, fy, cx, cy, k1, k2, p1, p2`.

Repeated samples at one focus token are allowed only when they identify distinct
calibration evidence. Repeating the exact same `(focus token, calibration
evidence SHA-256)` is rejected and cannot masquerade as repeatability evidence.

The API performs no metadata interpretation. A Sony MakerNote value, for
example, must first be converted by the future acquisition/evidence layer into
the exact retained token policy selected for that study.

## Probe model

Version 1 measures the same frozen pinhole + `k1/k2/p1/p2` forward projection
model used by calibration/Sparse SfM.

Nine normalized ideal rays are evaluated:

```text
centre
(0, 0)

edge probes
(-0.7, 0)  (+0.7, 0)  (0, -0.7)  (0, +0.7)

corner probes
(-0.7, -0.7)  (+0.7, -0.7)
(-0.7, +0.7)  (+0.7, +0.7)
```

For every pair of calibration samples, the artifact stores:

- centre delta in pixels;
- maximum cardinal edge-probe delta;
- maximum corner-probe delta;
- maximum over all nine probes;
- whether both samples use the same focus token;
- whether the pair crosses FIT/HOLDOUT roles.

These are measurements, not acceptance thresholds.

## L3DAFST1

The binary artifact is little-endian and bounded to 128 KiB.

It contains:

```text
magic = L3DAFST1
artifact version
probe-model version
study-context SHA-256
width / height
sample / pair / role counts
canonical sample records
all canonical pair records and projection metrics
```

Sample order is canonicalized by:

```text
focus-token bytes
calibration-evidence SHA-256
sample role
```

Therefore caller input order does not change artifact bytes or SHA-256.

Every floating value is finite binary64. Negative zero is normalized to positive
zero before serialization.

The artifact SHA-256 is suitable as retained evidence for a later v27
`lardon3d_optical_focus_domain_v2_create(...)` call only after the physical
study has been reviewed and its scientific applicability decision has been made.

## Summary output

The API also returns bounded aggregate measurements:

- FIT/HOLDOUT/sample/pair counts;
- same-focus / cross-focus / FIT-HOLDOUT pair counts;
- maximum centre, edge-probe, corner-probe and global pairwise delta;
- maximum same-focus global delta;
- maximum cross-focus global delta;
- maximum FIT/HOLDOUT global delta.

The summary is for inspection and orchestration. It does not encode PASS/FAIL.

## Non-goals

This v1 boundary does not:

- solve ChArUco calibration;
- parse EXIF/MakerNotes;
- invent physical focus distances;
- derive autofocus envelopes;
- define an acceptance pixel threshold;
- interpolate or extrapolate focus;
- open Project DB;
- create a v27 focus domain;
- make an execution READY.

The future physical study supplies the evidence needed to decide whether the
A6000 + E PZ 16-50 supports one domain, discrete domains, or exact focus only.

## Materialized Workflow bridge v1

**Status: PASS / FROZEN.**

`CALIBRATION_AF_STUDY_WORKFLOW_BRIDGE_V1` is the additive conversion boundary
between the already-FROZEN Calibration Workflow materializer and AF-study
samples.

It accepts a `Lardon3DCalibrationWorkflowExternalEvidence` that has already
passed the existing immutable-file, solver-bundle and provenance checks. It
does not parse `solve.json` or any other solver file again.

The bridge:

```text
materialized Calibration Workflow evidence
+ caller-supplied exact focus token
+ caller-supplied FIT/HOLDOUT role
-> Lardon3DCalibrationAfStudySample
```

The sample publishes `repeated_parameters[0]` only after checking that all three
retained full solves remain exactly equal.

Its `calibration_evidence_sha256` is independent of the focus token and
FIT/HOLDOUT role and binds:

- target identity;
- optical-state identity;
- solver executable/configuration identity;
- initialization evidence;
- validation evidence;
- exact oriented dimensions;
- exact published binary64 intrinsics.

Therefore relabelling one calibration result cannot manufacture independent
calibration evidence.

The bridge performs no DB access, metadata interpretation, physical AF decision,
thresholding, interpolation or extrapolation.
