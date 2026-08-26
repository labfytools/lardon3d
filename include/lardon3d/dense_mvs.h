#ifndef LARDON3D_DENSE_MVS_H
#define LARDON3D_DENSE_MVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/sparse_sfm_incremental.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_DENSE_MVS_SHA256_SIZE = 32,
  LARDON3D_DENSE_MVS_PARAMETER_RECORD_SIZE = 40,
  LARDON3D_DENSE_MVS_IDENTITY_RECORD_SIZE = 220,
  LARDON3D_DENSE_MVS_BACKEND_MANIFEST_RECORD_SIZE = 148,
  LARDON3D_DENSE_MVS_VERSION_IDENTITY_SIZE = 32,
  LARDON3D_DENSE_MVS_PATH_CAPACITY = 4096,
  LARDON3D_DENSE_MVS_KIND_OPENMVS = 1,
  LARDON3D_DENSE_MVS_VERSION = 1,
  LARDON3D_DENSE_MVS_OPENMVS_VERSION = 0x00020400,
};

typedef enum {
  LARDON3D_DENSE_MVS_OK = 0,
  LARDON3D_DENSE_MVS_INVALID_ARGUMENT,
  LARDON3D_DENSE_MVS_INVALID_SNAPSHOT,
  LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE,
  LARDON3D_DENSE_MVS_IO_ERROR,
  LARDON3D_DENSE_MVS_BACKEND_ERROR,
  LARDON3D_DENSE_MVS_INVALID_OUTPUT,
  LARDON3D_DENSE_MVS_OUT_OF_MEMORY,
} Lardon3DDenseMvsStatus;

typedef struct {
  uint32_t resolution_level;
  uint32_t minimum_resolution;
  uint32_t number_views;
  uint32_t fusion_mode;
} Lardon3DDenseMvsParameters;

/* The version fields identify the pinned version established by bounded --help probes. */
typedef struct {
  unsigned char interface_colmap_version_identity[32];
  unsigned char interface_colmap_binary_sha256[32];
  unsigned char densify_point_cloud_version_identity[32];
  unsigned char densify_point_cloud_binary_sha256[32];
} Lardon3DDenseMvsBackendManifest;

typedef struct {
  unsigned char base_reconstruction_identity[32];
  unsigned char source_image_set_identity[32];
  unsigned char calibration_scope_identity[32];
  unsigned char calibration_binding_identity[32];
  uint32_t dense_kind;
  uint32_t dense_version;
  uint32_t backend_kind;
  uint32_t backend_version;
  /* SHA-256 of the fixed-order OpenMVS v1 backend manifest. */
  unsigned char backend_binary_sha256[32];
  unsigned char parameter_fingerprint[32];
} Lardon3DDenseMvsIdentity;

typedef struct {
  uint64_t image_id;
  unsigned char immutable_sha256[32];
  const char *source_path;
  Lardon3DSparseGeometryCalibration calibration;
} Lardon3DDenseMvsSourceImage;

typedef struct {
  const Lardon3DSparseIncrementalResult *snapshot;
  /* Source-pixel coordinates keyed by the snapshot observation identity. */
  const Lardon3DSparseIncrementalObservation *source_observations;
  size_t source_observation_count;
  const Lardon3DDenseMvsSourceImage *source_images;
  size_t source_image_count;
  unsigned char base_reconstruction_identity[32];
  unsigned char calibration_scope_identity[32];
  Lardon3DDenseMvsParameters parameters;
  uint32_t execution_thread_count;
  const char *staging_directory;
  const char *interface_colmap_executable;
  const char *densify_point_cloud_executable;
} Lardon3DDenseMvsInput;

typedef struct {
  Lardon3DDenseMvsStatus status;
  unsigned char dense_identity[32];
  unsigned char parameter_fingerprint[32];
  unsigned char base_reconstruction_identity[32];
  unsigned char source_image_set_identity[32];
  unsigned char backend_implementation_sha256[32];
  char point_cloud_path[LARDON3D_DENSE_MVS_PATH_CAPACITY];
  uint64_t point_count;
} Lardon3DDenseMvsResult;

bool lardon3d_dense_mvs_parameter_fingerprint_record(
    const Lardon3DDenseMvsParameters *parameters,
    unsigned char record[LARDON3D_DENSE_MVS_PARAMETER_RECORD_SIZE]);
bool lardon3d_dense_mvs_parameter_fingerprint(
    const Lardon3DDenseMvsParameters *parameters, unsigned char digest[32]);
bool lardon3d_dense_mvs_backend_manifest_record(
    const Lardon3DDenseMvsBackendManifest *manifest,
    unsigned char record[LARDON3D_DENSE_MVS_BACKEND_MANIFEST_RECORD_SIZE]);
bool lardon3d_dense_mvs_backend_manifest_digest(
    const Lardon3DDenseMvsBackendManifest *manifest, unsigned char digest[32]);
bool lardon3d_dense_mvs_identity_record(
    const Lardon3DDenseMvsIdentity *identity,
    unsigned char record[LARDON3D_DENSE_MVS_IDENTITY_RECORD_SIZE]);
bool lardon3d_dense_mvs_identity_digest(
    const Lardon3DDenseMvsIdentity *identity, unsigned char digest[32]);
bool lardon3d_dense_mvs_source_image_set_identity(
    const Lardon3DDenseMvsSourceImage *images, size_t count,
    unsigned char digest[32]);
bool lardon3d_dense_mvs_calibration_binding_identity(
    const Lardon3DDenseMvsSourceImage *images, size_t count,
    unsigned char digest[32]);

/* run() replaces result on success and resets it to failure-atomic state on error. */
Lardon3DDenseMvsStatus lardon3d_dense_mvs_run(
    const Lardon3DDenseMvsInput *input, Lardon3DDenseMvsResult *result);
void lardon3d_dense_mvs_result_destroy(Lardon3DDenseMvsResult *result);

#ifdef __cplusplus
}
#endif

#endif
