# Lardon3D Documentation Index

## Status

```text
DOCUMENTATION_INDEX=CURRENT
CURRENT_PROJECT_DB_SCHEMA=v25
CURRENT_PRODUCTION_TASK_KINDS=16

REAL_S21_TRACKS=PASS/FROZEN
REAL_A6000_PRE_SFM=PASS/FROZEN
```

This index separates current authority, historical evidence and future product-definition work.

Repository documentation is read in context: an older schema number, Task count or resource measurement
inside a historical checkpoint is not stale merely because the current repository has moved forward.

## Authority order

When current-state prose disagrees, use this order:

1. `AGENTS.md` for repository-wide engineering/agent obligations and protected boundaries.
2. Specialized architecture documents for the subsystem contract they own.
3. `docs/roadmap/roadmap.md` for current lifecycle / next work.
4. `README.md` and architecture overview for concise summaries.
5. Historical audits only for the checkpoint they explicitly record.

Executable source and schema implementation remain authoritative when a documentation excerpt claims to
quote an exact API, DDL or constant.

## Current repository state

```text
Project DB head          v25
Production Task kinds    16

v22 selected scientific execution foundation
v23 generic optical-context overlay
v24 raw.develop.batch/1 persistence
v25 features.extract.batch/1 persistence
```

Canonical resource objective:

```text
MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF
```

Reference-host values are measurements, not portable constants.

## Current real-data checkpoints

### S21

```text
REAL_S21_TRACKS=PASS/FROZEN
```

The retained S21 proof reaches Tracks.

### A6000

```text
real-a6000-pre-sfm-2026-09-02
REAL_A6000_PRE_SFM=PASS/FROZEN
```

Retained A6000 counts:

```text
Feature Sets        689
Candidate Pairs     38,420
Match Results       38,420
Applicable GVRs     37,805
Verified GVRs       10,952
Rejected GVRs       26,853
Track Sets          1
Tracks              130,714
Track observations  318,944
```

The A6000 checkpoint stops before real Sparse SfM and Dense/MVS.

Sparse SfM capability exists through frozen Gates C-G. Real Sparse SfM on the historical S21/A6000
campaigns remains blocked by known-calibration data.

## Current architecture

Start here:

- [Architecture overview](architecture/overview.md)
- [Reconstruction pipeline](architecture/reconstruction_pipeline.md)
- [Runtime](architecture/runtime.md)
- [Persistence](architecture/persistence.md)
- [Project Database](architecture/project_database.md)

### Task / resource execution

- [Task System](architecture/task_system.md)
- [Task Queue](architecture/task_queue.md)
- [Task Kind Registry](architecture/task_kind_registry.md)
- [Resource Governor](architecture/resource_governor.md)
- [Resource Boundary](architecture/resource_boundary.md)
- [Resource-aware Pipeline](architecture/resource_aware_pipeline.md)
- [Internal Parallelism](architecture/internal_parallelism.md)
- [Scheduler / Resource Integration](architecture/scheduler_resource_integration.md)

Resource authority split:

```text
resource_governor.md   runtime policy
AGENTS.md              engineering / agent obligations
target_hardware.md     reference-host evidence
other documents        scoped summaries and links
```

## Scientific pipeline

- [Photo Quality Triage](architecture/photo_quality_triage.md)
- [Feature Store](architecture/feature_store.md)
- [Precision Feature Pipeline](architecture/precision_feature_pipeline.md)
- [Visual Index](architecture/visual_index.md)
- [Candidate Pair](architecture/candidate_pair.md)
- [Match Result](architecture/match_result.md)
- [Matcher](architecture/matcher.md)
- [Geometric Verification Model](architecture/geometric_verification.md)
- [Geometric Verifier](architecture/geometric_verifier.md)
- [Track Model](architecture/tracks.md)
- [Track Builder](architecture/track_builder.md)
- [Sparse SfM](architecture/sparse_sfm.md)
- [Vulkan ORB Matcher](architecture/vulkan_matcher.md)

Current production Geometric Verifier lineage:

```text
Fundamental v1  historical valid identity
Fundamental v2  historical valid identity
Fundamental v3  current production identity
```

Historical verifier results are never relabelled.

## Calibration

- [Calibration Science v1](architecture/calibration_science_v1.md)
- [Calibration Bootstrap v1](architecture/calibration_bootstrap.md)
- [Calibration Solver Preflight v1](architecture/calibration_solver_preflight_v1.md)

Historical S21/A6000 geometry evidence must not be retroactively given invented calibration.

## Development

- [Build](development/build.md)
- [Testing](development/testing.md)
- [Concurrency](development/concurrency.md)
- [Target Hardware](performance/target_hardware.md)

Build/test parallelism is host-aware. Fixed historical `-j8`, `-j12` or serialized-suite evidence does
not become a portable default.

TSan/OpenCV/TBB qualifications and Vulkan validation remain separate evidence boundaries.

## Roadmap

- [Roadmap](roadmap/roadmap.md)

The roadmap owns the current lifecycle / next-work cursor.

Historical progress cursors inside other records do not override it.

## Audits

- [Documentation Inventory Audit](audits/documentation_inventory.md)
- [Global Maintenance Audit](architecture/global_maintenance_audit.md)
- [Foundation Review](architecture/foundation_review.md)

### Historical audit rule

`global_maintenance_audit.md` and `foundation_review.md` are historical evidence.

Do not mechanically modernize their old:

- schema version;
- Task count;
- compiler/build count;
- resource measurement;
- checkpoint status.

The later A6000 checkpoint adds new operational evidence; it does not erase the maintenance checkpoint.

## Concepts

- [Scan Sets](concepts/scan_sets.md)
- [Visual Index concept](concepts/visual_index.md)
- [Matching and Tracks](concepts/matching_and_tracks.md)
- [Reconstruction Layers](concepts/reconstruction_layers.md)
- [Geometric Constraints](concepts/geometric_constraints.md)

Concept documents may be historical or explanatory. They do not outrank the current specialized
architecture contract.

## Future product-definition boundary

The documentation audit does not invent final contracts for:

- viewer;
- A6000 live acquisition;
- S21 live acquisition;
- coverage analysis;
- capture guidance;
- suggested viewpoints;
- video/keyframe ingestion;
- final optics onboarding UX;
- profile import/export UX;
- dense/mesh/texture/export UX.

Those belong to the separate Product Definition and final prompt-tree phases.

## Repository language

Canonical target:

```text
DOCUMENTATION_LANGUAGE=ENGLISH
SOURCE_COMMENT_LANGUAGE=ENGLISH
AGENT_CONTRACT_LANGUAGE=ENGLISH
USER_INTERFACE_LANGUAGE=ENGLISH
```

The current-state documentation findings have been remediated in English.

Some untouched historical/current documents may still require a mechanical language-only normalization
pass. Such translation must preserve historical facts and must not silently modernize scientific or
lifecycle state.

Executable UI strings are changed only in an explicitly scoped UI-language implementation pass.

## Navigation rule for future agents

Before implementation:

```text
read AGENTS.md
-> read README.md
-> read this index
-> read the specialized architecture contract
-> read the current roadmap
-> inspect historical audits only when their evidence is relevant
```

Never use an older historical checkpoint as a substitute for current authority.
