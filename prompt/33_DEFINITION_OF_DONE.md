# 33 — Definition of Done

## Status

```text
PRODUCT_DONE=OBJECTIVE_EVIDENCE_REQUIRED
```

## Authority

`docs/product/product_definition.md` and all specialized FROZEN contracts.

## Final objective evidence

Lardon3D v1 is complete only when retained evidence demonstrates all of the following:

- ordinary new camera-body onboarding requires no code change;
- ordinary new-lens onboarding requires no code change;
- manual lens without EXIF is supported;
- zoom/multiple focal optical configurations are supported;
- profile import/export is bounded, versioned and conflict-safe;
- calibration workflow reaches truthful `READY`;
- a dedicated calibrated real campaign completes real Sparse SfM;
- Dense/OpenMVS execution is durable, restartable and failure-atomic;
- mesh generation works;
- refinement works;
- texturing works;
- interoperable exports are traceable;
- arbitrary monocular scale is never mislabeled metric;
- viewer displays sparse evidence;
- viewer displays dense evidence;
- viewer displays mesh;
- viewer displays textured mesh;
- registered cameras/frustums and components are inspectable;
- viewer can be disabled, lag or close/crash without corrupting or blocking the engine;
- offline Coverage Analysis classifies `UNKNOWN/UNSEEN/WEAK/ADEQUATE` with versioned validated science and confidence;
- the system produces an actionable supplementary viewpoint;
- live localization exposes truthful confidence and loss;
- stock Sony A6000 works through native HDMI capture without camera modification;
- a supplementary full-resolution capture returns through the normal provenance/scientific pipeline;
- S21 integration requires no root and does not fork the scientific core;
- independent multi-campaign reconstruction can produce an explicit durable registration transform with quality/provenance and accepted fusion without raw Feature/Track merging;
- video ingestion produces deterministic bounded keyframes traceable to exact source timeline identity, algorithm/version and parameter fingerprint before normal Capture ingestion;
- restart/recovery works across long-running stages;
- safe useful CPU/GPU execution is demonstrated on representative hardware;
- optional SSD scratch has complete lease/drain/safe-unplug lifecycle when used and swap remains explicit opt-in;
- project portability does not depend on temporary paths, current hardware topology/device IDs, scratch mount path, live-device connection, process ID or worker instance;
- core workflows remain local-first with no required cloud upload;
- cleanup previews destructive removals and never automatically deletes FROZEN/historical evidence;
- user-visible diagnostics distinguish scientific rejection, invalid input, missing prerequisite, resource wait/throttle, runtime failure, corruption, unsupported version/backend and cancellation;
- no calibration, lens, scale, campaign relationship or provenance is invented;
- historical identities and PASS/FROZEN checkpoints remain interpretable;
- documentation and TUI behavior match the acquired product state.

## Not sufficient

The product is not done merely because it compiles, unit tests pass, Sparse SfM exists, OpenMVS can be launched manually, or a mesh can be produced by hand.

## Closure

Final closure requires a delta-based validation report showing how every REQUIRED_V1 item above is satisfied or explicitly identifying any remaining blocker. Only then may `PRODUCT_DEFINITION_V1` product implementation be considered fulfilled.
