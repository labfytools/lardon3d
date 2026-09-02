# Lardon3D — Documentation Inventory Audit

## Status

DOCUMENTATION_INVENTORY_AUDIT=PASS_WITH_FINDINGS

Audit branch: docs-audit

Reference checkpoints:

- global-maintenance-2026-09-01
- real-a6000-pre-sfm-2026-09-02

This audit concerns documentation only.

It does not reopen scientific contracts, alter executable behavior, redefine FROZEN evidence, or authorize implementation work.

## Classification vocabulary

Documents/findings use: CURRENT_AND_COMPLETE, CURRENT_BUT_INCOMPLETE, PARTIALLY_STALE, STALE, HISTORICAL_AND_VALID, DUPLICATED_AUTHORITY, AMBIGUOUS_AUTHORITY, NEEDS_REORGANIZATION.

Finding severity: BLOCKING_DOC, IMPORTANT, IMPROVEMENT.

A BLOCKING_DOC finding means documentation could make a future agent work from the wrong project state. It does not mean executable code is known to be incorrect.

## Current facts

Current Project DB head: v25.

Valid historical/additive foundations remain:

- v22 = selected scientific execution foundation
- v23 = generic optical-context overlay
- v24 = raw.develop.batch persistence
- v25 = features.extract.batch persistence

Current production inventory: 16 Task kinds.

Canonical resource policy:

- MAXIMUM SAFE USEFUL THROUGHPUT
- SERIALISM_REQUIRES_PROOF

Reference-host evidence:

- 16 logical CPUs total
- approximately 4 logical CPUs reserved for interactive host use
- approximately 12 logical CPUs available to compute
- approximately 3 GiB MemAvailable hard reserve
- Radeon 780M UMA

These are observations of the reference host, not portable product constants.

Real-data checkpoints:

- REAL_S21_TRACKS=PASS/FROZEN
- REAL_A6000_PRE_SFM=PASS/FROZEN

A6000 final pre-SfM evidence:

- Feature Sets: 689
- Candidate Pairs: 38,420
- Match Results: 38,420
- Applicable GVR: 37,805
- Verified GVR: 10,952
- Rejected GVR: 26,853
- Track Sets: 1
- Tracks: 130,714
- Track observations: 318,944
- Sparse SfM: NOT EXECUTED
- Dense/MVS: NOT EXECUTED

# BLOCKING_DOC findings

## DOC-B01 — Roadmap contains mutually exclusive current states

File: docs/roadmap/roadmap.md

The roadmap contains the acquired REAL_A6000_PRE_SFM=PASS/FROZEN result but also retains obsolete current-next material describing Project DB v24/v25 as validation in progress and instructing a resume from A6000 cursor 259 with 430 RAW representations remaining.

Those operations have already completed.

Required correction:

- preserve useful historical evidence;
- mark the cursor-259 and validation-in-progress material as historical;
- make the final A6000 pre-SfM proof authoritative;
- define one unambiguous CURRENT NEXT state.

## DOC-B02 — Project Database has competing current heads

File: docs/architecture/project_database.md

Different sections present v23, v24 and v25 as current state.

The actual current schema head is v25.

v22, v23 and v24 remain valid historical and additive contracts. They must not be erased or rewritten as though they never existed.

Required correction:

- state once near the top that current Project DB head is v25;
- preserve v22 selected execution, v23 optics and v24 RAW batch as historical additive foundations;
- update v25 Feature batch lifecycle to match acquired real evidence.

## DOC-B03 — Persistence advertises v23 as current

File: docs/architecture/persistence.md

The document currently states that the current schema is v23.

Required correction:

- current schema = v25;
- retain historical v7, v16, v22 and v23 contracts;
- add a concise v24 RAW batch and v25 Feature batch persistence summary.

## DOC-B04 — Resource-aware pipeline describes superseded serial Features

File: docs/architecture/resource_aware_pipeline.md

Current prose still describes Feature Extraction as one durable task per image with batch one and one worker as the complete operational model.

That description is incomplete after features.extract.batch/1, Project DB v25, bounded cross-image participants, coupled CPU/batch scaling and owner-only ordered publication.

The document also retains a fourteen-Task current inventory even though production now contains sixteen Task kinds.

Required correction:

- preserve per-image Feature scientific atomicity;
- document the v25 cross-image batch path;
- distinguish per-item atomicity from cross-item serialism;
- update current Task count to sixteen.

## DOC-B05 — Development testing guide uses obsolete API examples

File: docs/development/testing.md

The guide contains examples based on old-style names including task_estimate_t, task_t, task_create, TASK_STATE_IDLE and g_test_*.

These examples do not represent the current repository API and test style.

Because this is a developer instruction document rather than historical evidence, stale examples are dangerous.

It also retains repeated wipe-build and fixed -j8 assumptions that conflict with current delta-validation and host-aware parallelism policy.

Required correction:

- rewrite examples against actual current repository tests and public APIs;
- use current Meson targets and validation discipline;
- preserve exact sanitizer qualifications;
- avoid mandatory repeated full rebuilds when the delta does not justify them.

## DOC-B06 — Sparse SfM opening lifecycle is historical but presented as current

File: docs/architecture/sparse_sfm.md

The opening still describes numerical Sparse SfM as deferred to Gate C and later gates.

Later sections and other canonical documents establish Gates C, D, E, F and G as acquired.

Required correction:

- retain Gate A and Gate B history;
- label early lifecycle wording as historical;
- add a concise current lifecycle summary;
- distinguish implemented Sparse SfM capability from real Sparse SfM execution on current historical campaigns, which remains unexecuted because known calibration data is unavailable.

## DOC-B07 — README presents obsolete current DB state

File: README.md

README still presents Project DB v23 as the effective current head.

Required correction:

- current Project DB = v25;
- mention RAW batch v24 and Feature batch v25;
- register REAL_S21_TRACKS=PASS/FROZEN;
- register REAL_A6000_PRE_SFM=PASS/FROZEN;
- state clearly that Sparse SfM and Dense/MVS were not executed in the A6000 real pre-SfM proof.

# IMPORTANT findings

## DOC-I01 — AGENTS.md still describes v25 proof as unfinished

File: AGENTS.md

The resource policy and Source Comment Contract are strong and current.

However, lifecycle prose still describes the v25 Feature-batch tranche as not yet eligible for final closure.

Required correction:

- update the v25 lifecycle wording to match acquired evidence;
- register REAL_A6000_PRE_SFM=PASS/FROZEN;
- retain global-maintenance-2026-09-01 as valid historical review evidence;
- document the later A6000 checkpoint without erasing the maintenance checkpoint.

## DOC-I02 — Resource Boundary contains stale current schema and Task count

File: docs/architecture/resource_boundary.md

Present-tense material still contains combinations of current schema v24, Project Database v23 as current identity owner, fourteen current Task kinds and migrations only through v23.

The no-generic-Resource-System decision remains valid and must remain historical authority.

Required correction:

- update only stale present-tense current-state claims;
- current schema = v25;
- current production inventory = 16 Task kinds;
- preserve the original Gate G and no-new-subsystem decisions.

## DOC-I03 — Architecture overview weakens the canonical resource policy

File: docs/architecture/overview.md

The overview states that host stability and TUI responsiveness have priority over maximum throughput without the qualification introduced by the current canonical policy.

The current rule is to preserve the defined interactive host reserve and then maximize safe useful throughput.

Leaving additional safe and useful resources idle is not a stability strategy.

Required correction:

- align the overview with MAXIMUM SAFE USEFUL THROUGHPUT;
- preserve the interactive host reserve as the safety boundary;
- summarize v24 and v25 operational overlays without making reference-host values portable constants.

## DOC-I04 — Candidate resource sections contradict each other

File: docs/architecture/candidate_pair.md

The current Task section describes approximately 256 KiB fixed memory, 8 MiB per admitted item, batch 1..64 and coupled CPU/batch scaling.

A later resource section still describes historical values around a 24-source window and 64 KiB per item.

Required correction:

- reconcile the later resource section with the current validated Task capability;
- preserve Candidate scientific identity, scoring, ordering and persistence;
- keep historical estimates only when explicitly labelled historical.

## DOC-I05 — Feature Store omits the v25 selected-execution batch path

File: docs/architecture/feature_store.md

The scientific Feature File contract is strong, but operational prose still describes only the historical single-image features.extract/1 path as the production model.

Required correction:

- preserve features.extract/1;
- document features.extract.batch/1 as an additional current operational path;
- distinguish per-image scientific atomicity from cross-image execution concurrency;
- keep general DAG planning separately deferred.

## DOC-I06 — Task Kind Registry uses an obsolete 15-kind anchor

File: docs/architecture/task_kind_registry.md

The document correctly states that production contains 16 Task kinds but links to resource_governor.md#audit-des-15-kinds-de-production.

Required correction:

- synchronize the Resource Governor heading and Registry link around the current 16-kind inventory;
- do not alter historical audit counts that were correct at their checkpoint.

## DOC-I07 — Generic Task adaptation prose conflicts with coupled CPU/batch kinds

File: docs/architecture/task_system.md

Generic Compute Governor prose says CPU and batch are never tried together.

Feature Batch and Candidate now have legitimate coupled CPU/batch rungs because additional CPU cannot exercise additional independent work while the admitted item window remains one.

Required correction:

- retain independent-dimension adaptation as the generic rule;
- document explicit coupled cross-item exceptions where the dimensions are operationally inseparable for measurement;
- do not make coupled scaling universal.

## DOC-I08 — Build guide retains historical fixed -j8 examples

File: docs/development/build.md

Build examples repeatedly use -j8 while current engineering policy requires safe host-aware parallelism.

Required correction:

- describe build parallelism as host-derived;
- a reference-host example may use approximately -j12 when clearly labelled as reference-host evidence;
- no fixed job count becomes a portable product constant.

## DOC-I09 — Concurrency guide retains historical build-policy examples

File: docs/development/concurrency.md

The concurrency, lifetime and TSan qualification rules are valuable and mostly current, but the build example still uses fixed -j8 and some illustrative snippets do not represent exact current public API names.

Required correction:

- align build parallelism with host-aware policy;
- identify illustrative pseudo-code as illustrative when it is not exact repository API;
- preserve the external OpenCV and TBB TSan qualification;
- preserve the separate Vulkan validation boundary.

## DOC-I10 — Visual Index describes implemented downstream work as future

File: docs/architecture/visual_index.md

The final future section still describes Candidate Pair Generator and Matcher as future consumers.

Both are implemented.

Required correction:

- replace the stale future wording with the actual current downstream relationship or explicitly mark it as historical design context;
- do not change Visual Index scientific identity or capacity contracts.

## DOC-I11 — Track Model contains stale production selector and pipeline wording

File: docs/architecture/tracks.md

The Track Model scientific contract remains valid, but some prose still presents Geometric Verifier v1 as the production selector and Sparse SfM as future.

Current real Track evidence uses Geometric Verifier v3. Sparse SfM Gates C through G are implemented, while real known-calibration Sparse SfM on the historical real campaigns remains unexecuted.

Required correction:

- preserve Track Model v1 identity and persistence semantics;
- distinguish historical verifier-v1 examples from the current v3 production lineage;
- distinguish implemented Sparse SfM capability from real campaign execution.

# Document classification

## CURRENT_AND_COMPLETE

The following documents are currently strong enough that no major contract rewrite is justified by this audit:

- docs/architecture/calibration_science_v1.md
- docs/architecture/calibration_bootstrap.md
- docs/architecture/calibration_solver_preflight_v1.md
- docs/architecture/photo_quality_triage.md
- docs/architecture/task_queue.md
- docs/architecture/matcher.md
- docs/architecture/track_builder.md
- docs/performance/target_hardware.md

Small consistency edits may still be appropriate later.

## CURRENT_BUT_INCOMPLETE

The following documents have strong core contracts but need limited current-state reconciliation:

- docs/architecture/internal_parallelism.md
- docs/architecture/runtime.md
- docs/architecture/geometric_verification.md
- docs/architecture/geometric_verifier.md

## HISTORICAL_AND_VALID

The following documents must remain historical evidence and must not be mass-modernized:

- docs/architecture/global_maintenance_audit.md
- docs/architecture/foundation_review.md
- docs/concepts/matching_and_tracks.md
- docs/concepts/reconstruction_layers.md

Older Task counts, schema versions, measurements and decisions may be exactly correct for the checkpoint described by those documents.

An old version number is stale only when the prose claims that it is the current state.

# Documentation authority findings

## AUTH-01 — Current Project DB authority is ambiguous

Current-version claims are distributed across:

- README.md
- docs/architecture/project_database.md
- docs/architecture/persistence.md
- docs/architecture/resource_boundary.md
- docs/roadmap/roadmap.md

They must converge on one current-state fact:

Project DB current schema = v25

Detailed v22, v23, v24 and v25 contracts remain owned by the specialized architecture documents.

## AUTH-02 — Resource policy is duplicated across too many authorities

Resource policy currently appears in AGENTS, Governor, Resource Boundary, Resource Aware Pipeline, Internal Parallelism, Overview, Target Hardware, Build and Concurrency documentation.

Desired authority split:

- docs/architecture/resource_governor.md owns runtime resource policy;
- AGENTS.md owns engineering and agent obligations;
- docs/performance/target_hardware.md owns reference-host measurements;
- other documents summarize and link instead of redefining policy.

## AUTH-03 — Latest real checkpoint needs first-class documentation

The repository now contains the real checkpoint real-a6000-pre-sfm-2026-09-02.

Documentation must explain its relationship to global-maintenance-2026-09-01.

The newer checkpoint does not erase the maintenance checkpoint. The maintenance checkpoint remains historical review authority for unchanged frozen systems; the A6000 checkpoint adds later operational and real-data evidence.

## LINK-01 — Task Kind Registry anchor is stale

docs/architecture/task_kind_registry.md states sixteen kinds but points to a Resource Governor anchor named for fifteen kinds.

The heading and cross-link must be synchronized without altering historical fourteen-kind audit evidence.

# Repository language decision

Human authority has selected English as the canonical repository language.

The final repository target is:

- DOCUMENTATION_LANGUAGE=ENGLISH
- SOURCE_COMMENT_LANGUAGE=ENGLISH
- AGENT_CONTRACT_LANGUAGE=ENGLISH
- USER_INTERFACE_LANGUAGE=ENGLISH

All technical documentation, agent contracts, prompt files and production source comments are to converge on English during the remediation passes.

Translation must preserve scientific meaning, historical truth, FROZEN contracts, identities, numeric values, lifecycle state and evidence. A historical document may be translated but must not be silently modernized.

The canonical TUI language is English. Existing non-English UI strings are to be remediated in an explicitly scoped UI-language pass; this documentation audit does not itself authorize executable UI changes.

# Product documentation intentionally deferred

This documentation cleanup must not invent final contracts for:

- Viewer behavior;
- Sony A6000 live acquisition;
- Samsung S21 live acquisition;
- Coverage Analysis;
- Capture Guidance;
- suggested viewpoints;
- video and keyframe ingestion;
- final optics onboarding UX;
- optics profile import and export;
- final mesh, texture and export UX.

Those areas will be defined explicitly by the human during the PRODUCT_DEFINITION phase before the final prompt/ execution contract is frozen.

Already established human product intent includes:

- NEW_CAMERA_REQUIRES_CODE_CHANGE=NO
- NEW_LENS_REQUIRES_CODE_CHANGE=NO
- ELECTRONIC_LENS_WITH_METADATA=SUPPORTED
- MANUAL_LENS_WITHOUT_EXIF=SUPPORTED
- MULTIPLE_LENSES_PER_CAMERA=SUPPORTED
- ZOOM_MULTIPLE_FOCALS=SUPPORTED
- MULTIPLE_OPTICAL_CONFIGURATIONS_PER_PROJECT=SUPPORTED
- SILENT_CALIBRATION_SUBSTITUTION=FORBIDDEN
- SILENT_LENS_IDENTITY_INFERENCE=FORBIDDEN
- OPTICS_TUI_WORKFLOW=REQUIRED
- PROFILE_IMPORT_EXPORT=REQUIRED

These statements describe product intent only. They do not authorize implementation during this documentation audit.

# Documentation remediation order

## D1 — Current-state authority

Correct first:

- README.md
- AGENTS.md
- docs/roadmap/roadmap.md
- docs/architecture/project_database.md
- docs/architecture/persistence.md

Goal: one coherent current lifecycle and one explicit Project DB v25 head.

## D2 — Resource and runtime consistency

Then correct:

- docs/architecture/resource_boundary.md
- docs/architecture/resource_aware_pipeline.md
- docs/architecture/overview.md
- docs/architecture/candidate_pair.md
- docs/architecture/feature_store.md
- docs/architecture/task_kind_registry.md
- docs/architecture/task_system.md

Goal: current operational descriptions obey MAXIMUM SAFE USEFUL THROUGHPUT and SERIALISM_REQUIRES_PROOF without rewriting frozen science.

## D3 — Developer instructions

Then correct:

- docs/development/testing.md
- docs/development/build.md
- docs/development/concurrency.md

Goal: developer instructions match current APIs, build policy and validation discipline.

## D4 — Targeted scientific and current-state wording cleanup

Then inspect and correct only stale lifecycle wording in:

- docs/architecture/sparse_sfm.md
- docs/architecture/tracks.md
- docs/architecture/visual_index.md
- docs/architecture/geometric_verification.md
- docs/architecture/geometric_verifier.md
- docs/architecture/runtime.md

This phase must not reopen FROZEN science.

## D5 — Documentation index and links

Finally reconcile README navigation, historical/current labels and cross-document anchors.

# Finding summary

BLOCKING_DOC = 7

IMPORTANT = 11

Highest-priority files:

1. docs/roadmap/roadmap.md
2. docs/architecture/project_database.md
3. docs/architecture/persistence.md
4. docs/architecture/resource_aware_pipeline.md
5. docs/development/testing.md
6. docs/architecture/sparse_sfm.md
7. README.md

# Audit boundary

This report does not authorize:

- scientific threshold changes;
- new schema versions;
- new Task kinds;
- resource-policy redesign;
- Viewer implementation;
- live-capture implementation;
- Sparse SfM execution;
- Dense/MVS execution.

The next separate audit is SOURCE_COMMENT_AUDIT over include/lardon3d/** and src/**.

Source comments will later be classified as:

- EXCELLENT
- GOOD
- ACCEPTABLE
- UNDER_COMMENTED
- SEVERELY_UNDER_COMMENTED
- STALE_COMMENT
- MISLEADING_CONTRACT_COMMENT

The comment audit will focus on WHY, CONTRACT, INVARIANT, OWNERSHIP, IDENTITY, persistence ordering, restart, cancellation, concurrency, resource accounting and FROZEN boundaries.

No mass source-comment editing is authorized by this inventory.

# Remediation closure — 2026-09-02

The documentation finding-remediation pass is complete.

```text
D1=PASS
D2=PASS
D3=PASS
D4=PASS
D5=PASS

BLOCKING_DOC_RESOLVED=7/7
IMPORTANT_RESOLVED=11/11

AUTHORITY_FINDINGS_RESOLVED=3/3
LINK_FINDINGS_RESOLVED=1/1
```

D1 converged README, AGENTS, roadmap, Project DB and persistence on the current v25 lifecycle.

D2 reconciled resource/runtime documentation with sixteen production Task kinds,
`MAXIMUM_SAFE_USEFUL_THROUGHPUT`, `SERIALISM_REQUIRES_PROOF`, the v24 RAW batch path and the v25
Feature batch path.

D3 replaced stale developer API/build examples, removed fixed build-width policy and preserved the
qualified sanitizer/TSan/Vulkan evidence boundaries.

D4 reconciled Sparse SfM lifecycle, Track verifier lineage, Visual Index downstream status, current
Geometric Verifier v3 and runtime current-state wording without reopening frozen scientific contracts.

D5 adds `docs/README.md` as the navigation/authority map and removes the stale fifteen-kind
cross-document anchor dependency.

The historical `global-maintenance-2026-09-01` checkpoint remains authoritative evidence for the
unchanged boundaries it reviewed. The later `real-a6000-pre-sfm-2026-09-02` checkpoint adds real-data
operational evidence and does not erase the maintenance checkpoint.

No source code, schema, Task Kind, scientific threshold, Sparse SfM execution, Dense/MVS execution,
viewer or live-capture implementation is authorized by this closure.

## Language normalization boundary

Human authority selected English as the repository language.

The documents modified by the finding-remediation pass are English. Other untouched documentation may
still contain historical or current non-English prose. That remaining language-only normalization is
tracked separately and must not be mistaken for an unresolved current-state authority finding.

Translation of historical evidence must preserve the historical checkpoint exactly.

# Phase state

- DOCUMENTATION_INVENTORY_AUDIT=PASS_WITH_FINDINGS
- DOCUMENTATION_FINDING_REMEDIATION=PASS
- DOCUMENTATION_LANGUAGE_NORMALIZATION=IN_PROGRESS
- SOURCE_COMMENT_AUDIT=PASS
- SOURCE_COMMENT_REMEDIATION=PASS
- PRODUCT_DEFINITION=NEXT
- PROMPT_TREE=NOT_STARTED
