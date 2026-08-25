#ifndef LARDON3D_SPARSE_SFM_GATE_F_INTERNAL_H
#define LARDON3D_SPARSE_SFM_GATE_F_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/resource_governor.h>
#include <lardon3d/sparse_sfm_incremental.h>

bool lardon3d_sparse_sfm_fingerprint_record(
    const Lardon3DSparseIncrementalParameters *parameters, unsigned char record[372]);
bool lardon3d_sparse_sfm_parameter_fingerprint(
    const Lardon3DSparseIncrementalParameters *parameters, unsigned char digest[32]);
bool lardon3d_sparse_sfm_resource_estimate(uint64_t participating_image_count,
                                           uint64_t track_count, uint64_t observation_count,
                                           Lardon3DResourceEstimate *estimate);
bool lardon3d_sparse_sfm_component_persistable(uint64_t registered_image_count,
                                                uint64_t landmark_count);
bool lardon3d_sparse_sfm_publication_metrics(const double *squared_errors, size_t count,
                                              double *rmse, double *median);
bool lardon3d_sparse_sfm_squared_reprojection_error(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPose *pose,
    const Lardon3DSparseGeometryPoint3 *point,
    const Lardon3DSparseGeometryPoint2 *observed, double *squared_error);

#endif
