# Geometric Verification Model

## Status

```text
GEOMETRIC_VERIFICATION_MODEL=IMPLEMENTED
PROJECT_DB_GEOMETRIC_VERIFICATION=v12

HISTORICAL_VERIFIER_V1=VALID
HISTORICAL_VERIFIER_V2=VALID
CURRENT_PRODUCTION_VERIFIER_V3=FROZEN

CURRENT_PROJECT_DB_SCHEMA=v25
REAL_S21_GV_V3=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

This document owns the **persistent Geometric Verification Result model**.

It does not own the numerical estimator implementation. The current executable scientific verifier is
documented in `geometric_verifier.md`.

The persistence model was deliberately version-ready from Project DB v12: `verifier_version` and
`parameter_fingerprint` already belong to exact result identity. Therefore historical verifier v1/v2
and current v3 results coexist without a schema reinterpretation.

## Pipeline position

```text
Feature Set
-> Candidate Pair
-> Match Result
-> Geometric Verification Result
-> Track Builder
-> Track Model
```

The inlier mask indexes the canonical Match File entry order.

It does not directly index:

- Feature Store physical order;
- Candidate Pair order;
- temporary backend order.

## Scientific parent

The exact parent is:

```text
match_result_id
```

A Geometric Verification Result may be created only for a valid `MATCHED` parent with positive
`match_count`.

`NO_MATCH` and runtime failures do not produce a scientific geometric result.

## Persistent identity

Exact identity:

```text
(
  match_result_id,
  verifier_kind,
  verifier_version,
  parameter_fingerprint
)
```

No selection by timestamp or "latest" is permitted.

The fingerprint is an opaque canonical SHA-256 scientific parameter identity.

It excludes:

- Task ID;
- PID;
- elapsed time;
- CPU count;
- batch size;
- GPU identity;
- hardware identity.

## Verifier kind

The persistent supported model kind is:

```text
FUNDAMENTAL = 1
```

Do not reserve fictitious `ESSENTIAL` or `HOMOGRAPHY` values in prose without an explicit versioned
implementation decision.

## Scientific states

Completed scientific states are:

```text
GEOMETRIC_REJECTED = 1
GEOMETRIC_VERIFIED = 2
```

Runtime states such as RUNNING, FAILED, PAUSED or CANCELLED belong to Task Runtime, not this model.

A rejected result may still contain non-zero inlier support.

## Fundamental matrix representation

A verified Fundamental result contains nine SQLite `REAL` values:

```text
m00 ... m22
```

in row-major order.

The persistent representation is binary64 through SQLite numeric semantics, not a C ABI struct dump.

A verified row requires nine finite values.

A rejected row contains no model.

Rank/canonicalization/scientific-estimator rules belong to the versioned verifier contract.

## Inlier mask

The mask is a required SQLite BLOB of exact size:

```text
ceil(match_count / 8)
```

Bit convention for Match File entry `i`:

```text
byte = i / 8
bit  = i % 8
mask[byte] & (1u << bit)
```

The mask is LSB-first inside each byte.

Padding bits in the final byte are zero.

The mask popcount must equal `inlier_count`.

The mask exists for both verified and rejected scientific results.

With the current Match File bound of 8192 matches, the mask is at most 1024 bytes.

## Persistent invariants

For every row:

```text
0 <= inlier_count <= parent.match_count <= 8192
mask size is canonical
padding bits are zero
mask popcount == inlier_count
```

Additionally:

```text
REJECTED -> no Fundamental matrix
VERIFIED -> exactly nine finite matrix coefficients
```

A published row is immutable.

## Publication

Numerical estimation completes before the short Project DB publication transaction.

Publication inserts:

- exact parent;
- exact verifier identity;
- completed state;
- canonical mask;
- optional verified Fundamental model.

No external asset is required because the bounded mask/model fit naturally in SQLite.

Rollback leaves no partial scientific result.

## Reuse

Exact reuse uses only the full persistent identity.

Existing exact result:

```text
find
-> validate
-> reuse
```

Never:

```text
INSERT OR REPLACE
latest result
closest fingerprint
same parent with different version
```

A new scientific verifier version creates another result identity.

## Parent deletion

The parent FK uses delete-cascade semantics.

Explicit deletion of a Match Result deletes its dependent geometric results.

No parallel invalidation engine is required.

## Schema

Project DB v12 introduced `geometric_verification_results`.

The current schema head is v25.

Later schema additions do not redefine the v12 row format or identity.

## Public API

The model provides bounded create/load/find/list APIs for Geometric Verification Results.

The list API is paged and ordered by increasing ID.

In-memory result storage remains bounded: the inlier mask has fixed maximum capacity and no result-owned
heap destructor is required for the core row object.

Exact function declarations in the public headers remain authoritative.

## Error semantics

Creation distinguishes invalid local arguments from parent/identity constraints.

Loaders return corruption rather than a partially interpreted result if:

- parent is missing or invalid;
- stored mask length is wrong;
- padding is non-canonical;
- popcount disagrees;
- model/state nullability is inconsistent;
- a verified matrix contains non-finite values.

Scientific rejection is not a database/runtime failure.

Runtime OOM, exception, cancellation, estimator failure or device failure are not persisted as
`GEOMETRIC_REJECTED`.

## Current verifier lineage

The model stores all supported versions through the same identity fields.

### v1

Historical Fundamental verifier v1 remains immutable and valid.

### v2

Historical Fundamental verifier v2 remains immutable and valid.

V2 added the distinct-canonical-observation preflight in the scientific execution contract.

### v3

Current production verifier is Fundamental v3.

Production fingerprint:

```text
6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

V3 preserves the persistent model and adds its versioned scientific preflight before the unchanged
eligible estimator path.

No Project DB migration was needed for v3 because v12 already stores verifier version and fingerprint.

## Task relationship

The production Task Kind is:

```text
geometric_verifier.run/1
```

Project DB v13 adds only its typed durable Task payload.

The Task:

```text
pages Match Results
-> validates eligibility
-> computes or reuses exact GVR identity
-> owner publishes in canonical parent order
-> advances typed cursor
-> checkpoints
-> sequence_break
```

Task/runtime state remains separate from GVR scientific state.

## Current resource boundary

One Match Result is the scientific atomic item.

Current validated outer-parallel Task execution may prepare independent parents concurrently.

The owner publishes the contiguous canonical prefix.

Current validated bounds include:

```text
useful CPU participants <= 8
safe parent/window size <= 16
per-item reservation approximately 8 MiB
GPU = 0
```

The internal USAC/MAGSAC scientific solver remains `isParallel=false`.

These operational values do not enter GVR identity.

## Real S21 v3 evidence

Retained S21 proof:

```text
REAL_S21_GV_V3=PASS/FROZEN

Match Results         172,741
Applicable MATCHED    172,275
Verified GVRs          24,065
Rejected GVRs         148,210
non-applicable             466
duplicate mappings            0
```

The source Matcher project was retained unchanged and GV ran only from the Match Result boundary.

Restart/idempotence evidence preserved the complete GVR result set.

No Track/Sparse work belonged to the original GV-only boundary.

## Real A6000 v3 evidence

Retained current A6000 pre-SfM continuation:

```text
Match Results       38,420
Applicable GVRs     37,805
Verified GVRs       10,952
Rejected GVRs       26,853
duplicate mappings       0
```

Fingerprint:

```text
6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

The continuation then built Tracks and stopped before real Sparse SfM.

Checkpoint:

```text
REAL_A6000_PRE_SFM=PASS/FROZEN
```

## Out of scope

This persistence model does not define:

- RANSAC/USAC/MAGSAC implementation;
- GPU kernels;
- Task scheduling;
- Track construction;
- Essential pose;
- triangulation;
- Sparse SfM;
- Homography competition.

Those belong to their versioned scientific/runtime contracts.

## Summary

```text
GEOMETRIC_VERIFICATION_MODEL=IMPLEMENTED
PROJECT_DB_GEOMETRIC_VERIFICATION=v12
PROJECT_DB_GEOMETRIC_VERIFIER_TASK=v13

CURRENT_PRODUCTION_VERIFIER=FUNDAMENTAL_V3
CURRENT_VERIFIER_FINGERPRINT=6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c

REAL_S21_GV_V3=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN

CURRENT_PROJECT_DB_SCHEMA=v25
```
