# Geometric Verifier v1 / v2 / v3

## Status

```text
HISTORICAL_GEOMETRIC_VERIFIER_V1=FROZEN
HISTORICAL_GEOMETRIC_VERIFIER_V2=FROZEN
CURRENT_GEOMETRIC_VERIFIER_V3=PASS/FROZEN

CURRENT_VERIFIER_KIND=FUNDAMENTAL
CURRENT_VERIFIER_VERSION=3
CURRENT_VERIFIER_FINGERPRINT=6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c

GEOMETRIC_VERIFIER_GPU=NOT_JUSTIFIED
REAL_S21_GV_V3=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

This document owns the scientific execution that turns one valid `MATCHED` Match Result into one
completed Fundamental Geometric Verification Result.

The persistent row model is owned by `geometric_verification.md`.

Tracks, Essential pose, triangulation and Sparse SfM are downstream.

## Version lineage

### v1

Verifier v1 is a frozen historical scientific identity.

Its existing fingerprints and GVR rows remain immutable.

### v2

Verifier v2 preserves the v1 estimator/acceptance path but adds a bounded preflight requiring enough
distinct canonical observations on both sides before estimator execution.

It has its own version/fingerprint.

### v3

Verifier v3 is the current production policy.

It preserves v2 validation and additionally rejects estimator-ineligible parents when:

```text
match_count < min_inlier_count
```

because acceptance is mathematically impossible in that case.

At equality, the parent remains estimator-eligible.

V3 does not relabel or mutate v1/v2 rows.

Project DB v12 already stores verifier version/fingerprint, so no GVR schema migration is needed.

Current project schema head is v25.

## Inputs

The exact Match Result supplies:

- Candidate Pair relation;
- Feature Set A/B identities;
- Match File path/size/SHA;
- `match_count`.

Feature readers provide keypoints for the two immutable Feature Sets.

The verifier does not need descriptor blocks in its normal geometry path.

Feature coordinates are persistent binary32 decoded-image pixels with top-left origin.

They are converted to binary64 `Point2d` for geometric computation.

## Input ordering

Estimator row `i` corresponds exactly to Match File entry `i`.

The verifier does not reorder or deduplicate Match File rows.

The published inlier bit `i` always maps back to Match File entry `i`.

Out-of-range Feature indices or corrupt assets are runtime/input failure, not scientific rejection.

## Canonical observation identity

For preflight counting:

```text
A = (feature_set_id_a, feature_index_a)
B = (feature_set_id_b, feature_index_b)
```

V2/v3 require at least seven distinct A observations and seven distinct B observations.

Failure publishes a zero-inlier `GEOMETRIC_REJECTED` result with a correctly sized all-zero mask and
no Fundamental model.

This preflight does not:

- rewrite Matcher evidence;
- enforce one-to-one matching;
- deduplicate coordinates;
- perform a rank test;
- perform collinearity analysis;
- run Homography competition.

Distinct Feature IDs with identical coordinates remain distinct observations.

## v3 acceptance-feasibility preflight

V3 additionally checks the durable configured `min_inlier_count`.

When:

```text
match_count < min_inlier_count
```

the maximum possible support cannot satisfy acceptance, so V3 publishes the canonical zero rejection
without calling USAC.

The threshold comes from configuration, never a hidden constant.

## Scientific model

The only production model is a 3x3 Fundamental matrix.

Accepted output must be finite, non-zero and canonical.

The current verifier does not add Essential/Homography model competition.

## Production algorithm

The production estimator uses explicit OpenCV USAC/MAGSAC parameters rather than a hidden preset.

Scientific choices include:

```text
model                    FUNDAMENTAL
algorithm                USAC_MAGSAC
point representation     Point2d
threshold                1.5 px
confidence               0.999
max iterations           5000
minimum inlier count     16
minimum inlier ratio     0.20
sampler                   uniform
score                     MAGSAC
isParallel                false
LO iterations             5
LO sample size            14
polisher                  COV
polisher iterations       3
```

The internal scientific estimator remains serial:

```text
UsacParams::isParallel=false
```

Outer Task-level concurrency is separate.

## Random seed

The seed is derived locally from immutable scientific input.

The frozen seed domain is based on:

```text
L3DGVSE1
Match File SHA-256
verifier parameter fingerprint
```

The resulting seed is supplied to the per-call USAC parameter object.

Global `cv::theRNG()` mutation is forbidden.

## Parameter fingerprint

The verifier fingerprint is SHA-256 over the frozen canonical 84-byte encoding.

Domain:

```text
L3DGVFP1
```

It explicitly encodes:

- verifier kind;
- verifier version;
- algorithm;
- threshold;
- confidence;
- iteration bound;
- minimum inlier count;
- minimum inlier ratio;
- seed-policy version;
- canonicalization version;
- Point2d selection;
- sampler;
- score;
- `isParallel`;
- LO settings;
- neighbor mode;
- polisher settings;
- reserved zero byte.

Integers are little-endian.

Binary64 values use explicit IEEE-754 bits encoded little-endian.

No C struct dump participates.

Current v3 production fingerprint:

```text
6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

Historical v1/v2 fingerprints remain distinct and valid.

## Acceptance

A candidate is verified only when the frozen support policy is satisfied:

```text
inlier_count >= 16
inlier_count / match_count >= 0.20
```

Otherwise the result is scientific `GEOMETRIC_REJECTED`.

A scientific rejection is a completed valid result.

Runtime failure publishes no scientific GVR.

## Fundamental canonicalization

The nine coefficients must be finite.

Frobenius norm is computed robustly; zero norm is rejected.

The pivot is the first row-major coefficient having the strictly greatest absolute value.

The matrix is normalized and signed so that pivot is positive.

Signed zeros are normalized to `+0.0`.

No production SVD/rank-2 post-projection is performed by this verifier version.

## Inlier mask

OpenCV output mask is validated for type, length and values.

It is converted without reordering to the persistent LSB-first GVR bitset.

Padding bits are zero and popcount matches `inlier_count`.

## Runtime failure versus rejection

Scientific rejection includes valid cases such as:

- insufficient canonical support;
- v3 impossible acceptance support;
- estimator returns no acceptable model;
- inlier support below frozen acceptance.

Runtime/input failure includes:

- missing/corrupt Match or Feature asset;
- out-of-range Feature index;
- malformed estimator mask;
- non-finite invalid model;
- OOM;
- unexpected OpenCV exception;
- Project DB publication failure.

Runtime failure publishes no fake rejected result.

## Resource bounds

One scientific atomic item is one Match Result with at most 8192 correspondences.

Current outer-parallel Task reservation uses approximately:

```text
8 MiB per admitted parent
batch/window <= 16
useful CPU participants <= 8
GPU 0
```

The window is safe to 16; CPU8 is the retained useful maximum from measurement.

These are operational resource values, not fingerprint fields.

No descriptors or dense A-by-B matrix are loaded by the normal verifier.

## Outer parallel execution

Current validated shape:

```text
one admitted owner Task
-> bounded independent Match Result preparation
-> up to admitted CPU participants
-> join
-> owner publishes canonical contiguous prefix
-> owner advances cursor
-> checkpoint
-> sequence_break
```

Participant preparation writes no GVR rows.

Only the owner publishes after join.

This preserves deterministic parent ordering and restart semantics.

The inner USAC solver remains scientifically serial.

## GPU policy

Current classification:

```text
GEOMETRIC_VERIFIER_GPU=NOT_JUSTIFIED
```

Measured CPU units remained small enough that no production GPU backend was justified.

No Vulkan/OpenCL/CUDA verifier backend is currently part of the production identity.

CPU outer parallelism remains valid and should not be disabled merely because GPU is rejected.

## Durable Task

Task Kind:

```text
geometric_verifier.run/1
```

Project DB v13 adds the typed task payload required because generic checkpoint v1 does not contain the
scientific verifier parameters/cursor.

Durable payload includes the immutable scientific configuration and:

```text
after_match_result_id
```

The fingerprint is revalidated on reconstruction.

The durable payload does not contain:

- `cv::Mat`;
- RNG engine state;
- Governor feedback;
- hardware identity;
- CPU mask;
- transient buffers.

## Restart

Pagination follows increasing `match_result_id` and does not assume contiguous IDs.

A GVR is published before the durable cursor advances.

Crash after publication but before checkpoint may replay that parent.

Restart finds/revalidates the exact GVR identity and reuses it.

No overwrite is performed.

## Historical resource normalization

The exact historical serial resource shape remains accepted only for restart compatibility.

It may be normalized ephemerally to the current outer-parallel capability.

The original checkpoint is not rewritten.

Neighboring resource shapes are rejected rather than guessed.

## Real S21 GV v3

Retained S21 proof:

```text
REAL_S21_GV_V3=PASS/FROZEN

Match Results         172,741
Applicable MATCHED    172,275
Verified               24,065
Rejected              148,210
non-applicable             466
duplicate mapping            0
```

The v3 fingerprint is the current production fingerprint.

A complete replay produced zero new GVRs.

A SIGKILL/restart proof resumed the same Task and converged to the same complete GVR set.

The run stopped before Track Builder at the original GV-only checkpoint.

Later S21 Track evidence exists separately.

## Real A6000 v3

The retained A6000 pre-SfM continuation contains:

```text
Match Results       38,420
Applicable GVRs     37,805
Verified GVRs       10,952
Rejected GVRs       26,853
duplicate mappings       0
```

Current fingerprint:

```text
6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c
```

The Task completed the full cursor.

Restart traversed the cursor and created zero new GVRs.

The continuation then reused/built the frozen Track Set and stopped before real Sparse SfM.

```text
REAL_A6000_PRE_SFM=PASS/FROZEN
```

## Validation boundaries

The scientific verifier validation covers:

- canonical fingerprint/seed;
- bit-exact mask mapping;
- acceptance boundaries;
- corruption;
- publication/reuse;
- maximum Match File cardinality;
- restart;
- deterministic canonicalization;
- outer-parallel owner publication.

TSan qualification must preserve the external OpenCV/TBB boundary described by the concurrency/global
maintenance documents.

Do not claim Vulkan verifier validation: there is no production Vulkan verifier backend.

## Summary

```text
CURRENT_GEOMETRIC_VERIFIER=FUNDAMENTAL_V3
CURRENT_VERIFIER_VERSION=3
CURRENT_VERIFIER_FINGERPRINT=6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c

GEOMETRIC_VERIFIER_TASK=geometric_verifier.run/1
PROJECT_DB_GEOMETRIC_VERIFICATION=v12
PROJECT_DB_GEOMETRIC_VERIFIER_TASK=v13

INNER_USAC_PARALLEL=false
OUTER_PARALLEL=VALIDATED
USEFUL_CPU_MAX=8
SAFE_WINDOW_MAX=16
PER_ITEM_RAM=8_MiB
GPU=NOT_JUSTIFIED

REAL_S21_GV_V3=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
CURRENT_PROJECT_DB_SCHEMA=v25
```
