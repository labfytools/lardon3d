#ifndef LARDON3D_INCREMENTAL_RECONSTRUCTION_INTERNAL_H
#define LARDON3D_INCREMENTAL_RECONSTRUCTION_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/sparse_sfm_bundle_adjustment.h>

struct Lardon3DSparseBundleAdjustmentAnchor {
  uint64_t component_key;
  uint64_t pose_anchor_image_id;
  uint64_t scale_anchor_image_id;
  Lardon3DSparseBundleAdjustmentScaleAxis scale_axis;
};

Lardon3DSparseBundleAdjustmentExecutionStatus
lardon3d_sparse_bundle_adjustment_run_with_anchors(
    const Lardon3DSparseBundleAdjustmentInput *input,
    const Lardon3DSparseBundleAdjustmentAnchor *anchors, size_t anchor_count,
    Lardon3DSparseBundleAdjustmentResult *result);

#endif
