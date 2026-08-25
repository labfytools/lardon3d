#ifndef LARDON3D_INCREMENTAL_RECONSTRUCTION_H
#define LARDON3D_INCREMENTAL_RECONSTRUCTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/resource_governor.h>
#include <lardon3d/sparse_sfm_incremental.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_INCREMENTAL_RECONSTRUCTION_KIND = 1,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_VERSION = 1,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_SHA256_SIZE = 32,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_FINGERPRINT_RECORD_SIZE = 80,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_IDENTITY_RECORD_SIZE = 76,
};

typedef enum {
  LARDON3D_INCREMENTAL_RECONSTRUCTION_OK = 0,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_NO_CHANGE,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_ARGUMENT,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_OUT_OF_MEMORY,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MISSING,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_SPLIT,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MERGE,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_DUPLICATE,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_CROSS_COMPONENT_BRIDGE,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_DESCENDANT_UNREGISTERED_OBSERVATION,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_COMPONENT_KEY_VIOLATION,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_GEOMETRY_FAILED,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_BUNDLE_ADJUSTMENT_FAILED,
  LARDON3D_INCREMENTAL_RECONSTRUCTION_CANCELLED,
} Lardon3DIncrementalReconstructionStatus;

typedef struct {
  uint64_t base_reconstruction_id;
  uint64_t extension_track_set_id;
  uint64_t calibration_scope_id;
  uint32_t incremental_kind;
  uint32_t incremental_version;
  unsigned char parameter_fingerprint[32];
} Lardon3DIncrementalReconstructionIdentity;

typedef struct {
  uint64_t base_camera_count;
  uint64_t base_landmark_count;
  uint64_t base_observation_count;
  uint64_t extension_image_count;
  uint64_t extension_track_count;
  uint64_t extension_observation_count;
} Lardon3DIncrementalReconstructionShape;

typedef struct {
  uint64_t base_reconstruction_id;
  const Lardon3DSparseIncrementalResult *base;
  const Lardon3DSparseIncrementalObservation *base_track_observations;
  size_t base_track_observation_count;
  const Lardon3DSparseIncrementalInput *extension;
  const Lardon3DSparseIncrementalParameters *parameters;
  bool (*checkpoint)(void *context);
  void *checkpoint_context;
} Lardon3DIncrementalReconstructionInput;

typedef struct {
  Lardon3DIncrementalReconstructionStatus status;
  bool changed;
  Lardon3DSparseIncrementalResult snapshot;
} Lardon3DIncrementalReconstructionResult;

bool lardon3d_incremental_reconstruction_parameter_fingerprint(
    unsigned char digest[32]);
bool lardon3d_incremental_reconstruction_parameter_fingerprint_record(
    unsigned char record[
        LARDON3D_INCREMENTAL_RECONSTRUCTION_FINGERPRINT_RECORD_SIZE]);
bool lardon3d_incremental_reconstruction_identity_record(
    const Lardon3DIncrementalReconstructionIdentity *identity,
    unsigned char record[LARDON3D_INCREMENTAL_RECONSTRUCTION_IDENTITY_RECORD_SIZE]);
bool lardon3d_incremental_reconstruction_identity_digest(
    const Lardon3DIncrementalReconstructionIdentity *identity,
    unsigned char digest[32]);
bool lardon3d_incremental_reconstruction_resource_estimate(
    const Lardon3DIncrementalReconstructionShape *shape,
    Lardon3DResourceEstimate *estimate);

/* result must be zero-initialized or previously destroyed before run(). */
Lardon3DIncrementalReconstructionStatus lardon3d_incremental_reconstruction_run(
    const Lardon3DIncrementalReconstructionInput *input,
    Lardon3DIncrementalReconstructionResult *result);
void lardon3d_incremental_reconstruction_result_destroy(
    Lardon3DIncrementalReconstructionResult *result);

#ifdef __cplusplus
}
#endif

#endif
