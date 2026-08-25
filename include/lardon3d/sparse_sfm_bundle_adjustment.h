#ifndef LARDON3D_SPARSE_SFM_BUNDLE_ADJUSTMENT_H
#define LARDON3D_SPARSE_SFM_BUNDLE_ADJUSTMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/sparse_sfm_incremental.h>

#ifdef __cplusplus
extern "C" {
#endif

/* COMPLETE means every eligible component was accepted, PARTIAL means accepted
 * and rejected eligible components coexist, and FAILED means none was
 * accepted. E1 defines the ABI but produces no final status before E2. */
typedef enum {
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE = 0,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_PARTIAL,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_FAILED
} Lardon3DSparseBundleAdjustmentStatus;

/* Execution errors are separate from the scientific result status. An OK
 * execution may produce a scientific FAILED result when no eligible component
 * is accepted. */
typedef enum {
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK = 0,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INVALID_ARGUMENT,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OUT_OF_MEMORY,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR
} Lardon3DSparseBundleAdjustmentExecutionStatus;

typedef enum {
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_NONE = 0,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_X,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_Y,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_Z
} Lardon3DSparseBundleAdjustmentScaleAxis;

typedef enum {
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_NOT_RUN = 0,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_CONVERGED,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_NO_CONVERGENCE,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_FAILURE
} Lardon3DSparseBundleAdjustmentTermination;

typedef enum {
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONE = 0,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_INELIGIBLE,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_GAUGE_DEGENERATE,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_INPUT,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_UNDERCONSTRAINED,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NO_CONVERGENCE,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_SOLVER_FAILURE,
  LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_COST_REGRESSION
} Lardon3DSparseBundleAdjustmentRejectionReason;

/* Both views are caller-owned and immutable for the future synchronous Gate E
 * call. incremental_result is authoritative for components, registered
 * image_id values, world-to-camera poses, landmarks and final associations.
 * images/observations are the same resolved scientific view used for Gate D.
 * An observation is identified by (feature_set_id, feature_index); its image_id
 * must agree with the final association. Source binary32 x/y become binary64
 * source-image pixels, origin top-left, +x right and +y down. Calibrations are
 * known, fixed and immutable. Each pointer is paired with its size_t count. */
typedef struct {
  const Lardon3DSparseIncrementalResult *incremental_result;
  const Lardon3DSparseIncrementalImage *images;
  size_t image_count;
  const Lardon3DSparseIncrementalObservation *observations;
  size_t observation_count;
} Lardon3DSparseBundleAdjustmentInput;

/* Counts describe one component. pose_anchor_image_id fixes the complete pose;
 * scale_anchor_image_id and scale_axis identify the fixed camera-center
 * coordinate. Costs are 0.5*sum(Huber(dx*dx+dy*dy)) with delta 2 source pixels. RMSE is
 * sqrt(sum(dx*dx+dy*dy)/observation_count) in source pixels. has_costs,
 * has_rmse and has_anchors determine whether the corresponding fields are
 * available; unavailable doubles are zero, never NaN sentinels. */
typedef struct {
  uint64_t component_key;
  uint64_t camera_count;
  uint64_t landmark_count;
  uint64_t observation_count;
  bool eligible;
  bool has_anchors;
  uint64_t pose_anchor_image_id;
  uint64_t scale_anchor_image_id;
  Lardon3DSparseBundleAdjustmentScaleAxis scale_axis;
  bool has_costs;
  double initial_robust_cost;
  double final_robust_cost;
  bool has_rmse;
  double initial_reprojection_rmse_px;
  double final_reprojection_rmse_px;
  uint32_t iteration_count;
  Lardon3DSparseBundleAdjustmentTermination termination;
  bool accepted;
  Lardon3DSparseBundleAdjustmentRejectionReason rejection_reason;
} Lardon3DSparseBundleAdjustmentComponentDiagnostic;

/* Future E2 output. All arrays are owned by the result and ordered by the
 * canonical component/image/Track/observation identities. Poses remain
 * world-to-camera binary64. A destruction function is added with the E2 run;
 * E1 intentionally exposes no fake execution function. */
typedef struct {
  Lardon3DSparseBundleAdjustmentStatus status;
  Lardon3DSparseIncrementalComponent *components;
  size_t component_count;
  Lardon3DSparseIncrementalCamera *cameras;
  size_t camera_count;
  Lardon3DSparseIncrementalLandmark *landmarks;
  size_t landmark_count;
  Lardon3DSparseIncrementalLandmarkObservation *observations;
  size_t observation_count;
  Lardon3DSparseBundleAdjustmentComponentDiagnostic *diagnostics;
} Lardon3DSparseBundleAdjustmentResult;

/* Runs synchronous final per-component Bundle Adjustment. input remains
 * caller-owned and immutable. result must be zero-initialized or previously
 * destroyed. On EXECUTION_OK, result owns all non-null arrays and its
 * scientific status may be COMPLETE, PARTIAL or FAILED. On every other
 * execution status, result is left in its canonical zero state. */
Lardon3DSparseBundleAdjustmentExecutionStatus
lardon3d_sparse_bundle_adjustment_run(
    const Lardon3DSparseBundleAdjustmentInput *input,
    Lardon3DSparseBundleAdjustmentResult *result);

/* Releases every result-owned array and restores the canonical zero state.
 * The operation is NULL-safe and repeat-safe; it never releases input data. */
void lardon3d_sparse_bundle_adjustment_result_destroy(
    Lardon3DSparseBundleAdjustmentResult *result);

#ifdef __cplusplus
}
#endif

#endif
