#ifndef LARDON3D_SPARSE_SFM_MODEL_H
#define LARDON3D_SPARSE_SFM_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_SPARSE_SFM_SHA256_SIZE = 32,
  LARDON3D_SPARSE_SFM_PAGE_MAX = 64,
  LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE = 1,
  LARDON3D_SPARSE_SFM_CALIBRATION_VERSION = 1,
  LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT = 1,
  LARDON3D_SPARSE_SFM_PROVENANCE_IMPORTED_TRUSTED = 2,
  LARDON3D_SPARSE_SFM_KIND_INCREMENTAL = 1,
  LARDON3D_SPARSE_SFM_VERSION = 1,
};

typedef struct {
  uint64_t calibration_id;
  unsigned char scientific_hash[32];
  uint32_t model_kind;
  uint32_t model_version;
  uint32_t width;
  uint32_t height;
  double fx;
  double fy;
  double cx;
  double cy;
  double k1;
  double k2;
  double p1;
  double p2;
  uint32_t provenance_kind;
  unsigned char provenance_fingerprint[32];
} Lardon3DSparseCalibration;

typedef struct {
  uint64_t image_id;
  uint64_t calibration_id;
  unsigned char calibration_hash[32];
} Lardon3DSparseCalibrationMember;

typedef struct {
  uint64_t scope_id;
  unsigned char scientific_hash[32];
  uint64_t member_count;
} Lardon3DSparseCalibrationScope;

typedef struct {
  uint64_t reconstruction_id;
  uint64_t track_set_id;
  uint64_t calibration_scope_id;
  uint32_t sfm_kind;
  uint32_t sfm_version;
  unsigned char parameter_fingerprint[32];
  uint64_t component_count;
  uint64_t registered_image_count;
  uint64_t landmark_count;
  double reprojection_rmse_px;
  double reprojection_median_px;
} Lardon3DSparseReconstruction;

typedef struct {
  uint64_t component_id;
  uint64_t component_key;
  uint64_t registered_image_count;
  uint64_t landmark_count;
} Lardon3DSparseComponent;

typedef struct {
  uint64_t image_id;
  uint64_t component_key;
  double rotation_cw[9];
  double translation_cw[3];
} Lardon3DSparseRegisteredImage;

typedef struct {
  uint64_t landmark_id;
  uint64_t track_id;
  uint64_t component_key;
  double x;
  double y;
  double z;
  double reprojection_rmse_px;
  double reprojection_median_px;
  uint64_t observation_count;
} Lardon3DSparseLandmark;

typedef struct {
  uint64_t landmark_id;
  uint64_t track_id;
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint32_t position_in_track;
} Lardon3DSparseLandmarkObservation;

typedef struct {
  uint64_t track_set_id;
  uint64_t calibration_scope_id;
  uint32_t sfm_kind;
  uint32_t sfm_version;
  unsigned char parameter_fingerprint[32];
  const Lardon3DSparseComponent *components;
  size_t component_count;
  const Lardon3DSparseRegisteredImage *registered_images;
  size_t registered_image_count;
  const Lardon3DSparseLandmark *landmarks;
  size_t landmark_count;
  const Lardon3DSparseLandmarkObservation *observations;
  size_t observation_count;
  double reprojection_rmse_px;
  double reprojection_median_px;
  int64_t created_at;
} Lardon3DSparsePublication;

typedef struct {
  uint64_t after_id;
  size_t capacity;
  size_t count;
  uint64_t next_after_id;
  Lardon3DSparseCalibration *items;
} Lardon3DSparseCalibrationPage;

typedef struct {
  uint64_t after_id;
  size_t capacity;
  size_t count;
  uint64_t next_after_id;
  Lardon3DSparseReconstruction *items;
} Lardon3DSparseReconstructionPage;

typedef struct {
  uint64_t after_component_key;
  size_t capacity;
  size_t count;
  uint64_t next_component_key;
  Lardon3DSparseComponent *items;
} Lardon3DSparseComponentPage;

typedef struct {
  uint64_t after_image_id;
  size_t capacity;
  size_t count;
  uint64_t next_image_id;
  Lardon3DSparseRegisteredImage *items;
} Lardon3DSparseRegisteredImagePage;

typedef struct {
  uint64_t after_track_id;
  size_t capacity;
  size_t count;
  uint64_t next_track_id;
  Lardon3DSparseLandmark *items;
} Lardon3DSparseLandmarkPage;

typedef struct {
  uint64_t after_landmark_id;
  uint32_t after_position_in_track;
  size_t capacity;
  size_t count;
  uint64_t next_landmark_id;
  uint32_t next_position_in_track;
  Lardon3DSparseLandmarkObservation *items;
} Lardon3DSparseObservationPage;

Lardon3DProjectDbResult
lardon3d_sparse_calibration_create(Lardon3DProjectDb *database,
                                   const Lardon3DSparseCalibration *input,
                                   Lardon3DSparseCalibration *output);
Lardon3DProjectDbResult
lardon3d_sparse_calibration_load(Lardon3DProjectDb *database,
                                 uint64_t calibration_id,
                                 Lardon3DSparseCalibration *output);
Lardon3DProjectDbResult lardon3d_sparse_calibration_find_by_hash(
    Lardon3DProjectDb *database, const unsigned char scientific_hash[32],
    Lardon3DSparseCalibration *output);

Lardon3DProjectDbResult lardon3d_sparse_calibration_scope_create(
    Lardon3DProjectDb *database, const Lardon3DSparseCalibrationMember *members,
    size_t member_count, Lardon3DSparseCalibrationScope *output);
Lardon3DProjectDbResult
lardon3d_sparse_calibration_scope_load(Lardon3DProjectDb *database,
                                       uint64_t scope_id,
                                       Lardon3DSparseCalibrationScope *output);
Lardon3DProjectDbResult lardon3d_sparse_calibration_scope_find_by_hash(
    Lardon3DProjectDb *database, const unsigned char scientific_hash[32],
    Lardon3DSparseCalibrationScope *output);
Lardon3DProjectDbResult lardon3d_sparse_calibration_scope_list_members(
    Lardon3DProjectDb *database, uint64_t scope_id, uint64_t after_image_id,
    Lardon3DSparseCalibrationMember *items, size_t capacity, size_t *count,
    uint64_t *next_after_image_id);

Lardon3DProjectDbResult lardon3d_sparse_reconstruction_publish(
    Lardon3DProjectDb *database, const Lardon3DSparsePublication *publication,
    Lardon3DSparseReconstruction *output);
Lardon3DProjectDbResult lardon3d_sparse_reconstruction_find_exact(
    Lardon3DProjectDb *database, uint64_t track_set_id,
    uint64_t calibration_scope_id, uint32_t sfm_kind, uint32_t sfm_version,
    const unsigned char parameter_fingerprint[32],
    Lardon3DSparseReconstruction *output);
Lardon3DProjectDbResult
lardon3d_sparse_reconstruction_load(Lardon3DProjectDb *database,
                                    uint64_t reconstruction_id,
                                    Lardon3DSparseReconstruction *output);
Lardon3DProjectDbResult
lardon3d_sparse_reconstruction_list(Lardon3DProjectDb *database,
                                    uint64_t after_id, size_t capacity,
                                    Lardon3DSparseReconstructionPage *page);
Lardon3DProjectDbResult
lardon3d_sparse_component_list(Lardon3DProjectDb *database,
                               uint64_t reconstruction_id,
                               uint64_t after_component_key, size_t capacity,
                               Lardon3DSparseComponentPage *page);
Lardon3DProjectDbResult
lardon3d_sparse_registered_image_list(Lardon3DProjectDb *database,
                                      uint64_t reconstruction_id,
                                      uint64_t after_image_id, size_t capacity,
                                      Lardon3DSparseRegisteredImagePage *page);
Lardon3DProjectDbResult lardon3d_sparse_landmark_list(
    Lardon3DProjectDb *database, uint64_t reconstruction_id,
    uint64_t after_track_id, size_t capacity, Lardon3DSparseLandmarkPage *page);
Lardon3DProjectDbResult lardon3d_sparse_observation_list(
    Lardon3DProjectDb *database, uint64_t reconstruction_id,
    uint64_t after_landmark_id, uint32_t after_position_in_track,
    size_t capacity, Lardon3DSparseObservationPage *page);

#ifdef __cplusplus
}
#endif

#endif
