#ifndef LARDON3D_CALIBRATION_TOOLING_V2_H
#define LARDON3D_CALIBRATION_TOOLING_V2_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/calibration_bootstrap_v2.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_TOOLING_V2_VERSION = 2,
  LARDON3D_CALIBRATION_TOOLING_V2_VALIDATION_FLAGS = 15,
};

typedef enum {
  LARDON3D_CALIBRATION_TOOLING_V2_OK = 0,
  LARDON3D_CALIBRATION_TOOLING_V2_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED,
  LARDON3D_CALIBRATION_TOOLING_V2_CAPACITY,
  LARDON3D_CALIBRATION_TOOLING_V2_ENCODING_ERROR,
  LARDON3D_CALIBRATION_TOOLING_V2_IMPORT_ERROR,
} Lardon3DCalibrationToolingV2Result;

/* One selected-image member of a caller-declared scientific group. The item
 * index is the immutable selected-execution order, not an operational group
 * index. Parameters use the exact v1/OpenCV pinhole meaning. All storage is
 * caller-owned and borrowed only while the API call is active. */
typedef struct {
  uint32_t selected_item_index;
  uint64_t image_id;
  unsigned char representation_sha256[32];
  uint32_t width;
  uint32_t height;
  double fx, fy, cx, cy, k1, k2, p1, p2;
  uint32_t support_images;
  uint32_t support_observations;
  double reprojection_rmse_px;
  double maximum_parameter_delta;
  uint32_t validation_flags;
} Lardon3DCalibrationToolingV2Entry;

/* Group identity is the exact bounded state identifier plus its explicit
 * version. The six nonzero digests independently bind optical applicability,
 * physical target, solver executable/configuration, initialization, and
 * validation evidence. Entries must be strictly increasing by selected item
 * index; groups must be strictly increasing by (group_identity_sha256,
 * group_version). These canonical orders make identical evidence byte-stable
 * without hiding caller mistakes by silently reordering it. */
typedef struct {
  unsigned char group_identity_sha256[32];
  uint32_t group_version;
  unsigned char optical_state_sha256[32];
  unsigned char target_sha256[32];
  unsigned char solver_executable_sha256[32];
  unsigned char solver_configuration_sha256[32];
  unsigned char initialization_evidence_sha256[32];
  unsigned char validation_evidence_sha256[32];
  const Lardon3DCalibrationToolingV2Entry *entries;
  size_t entry_count;
} Lardon3DCalibrationToolingV2Group;

/* A bounded heterogeneous publication manifest. Every item index in
 * [0, entry_count) must occur exactly once across all groups, and every image
 * ID must be globally unique. Tooling validates only explicit evidence and
 * model-domain safety; it does not run a solver, invent acceptance thresholds,
 * or decide optical compatibility. */
typedef struct {
  const Lardon3DCalibrationToolingV2Group *groups;
  size_t group_count;
  size_t entry_count;
} Lardon3DCalibrationToolingV2Evidence;

/* Validate borrowed evidence and its borrowed nested group/entry arrays for
 * bounded counts, canonical ordering, and exact selected-index/image coverage.
 * Validation does not mutate evidence, artifact bytes, or Project DB state.
 * INVALID_ARGUMENT reports a missing top-level pointer/groups array or
 * out-of-range top-level counts; EVIDENCE_REJECTED reports invalid nested or
 * scientific evidence within an otherwise bounded top-level manifest. */
Lardon3DCalibrationToolingV2Result lardon3d_calibration_tooling_v2_validate(
    const Lardon3DCalibrationToolingV2Evidence *evidence);

/* Encode deterministic L3DCALB2 into caller storage and return its SHA-256.
 * `written` may be NULL; no partial artifact is reported as written. */
Lardon3DCalibrationToolingV2Result lardon3d_calibration_tooling_v2_produce(
    const Lardon3DCalibrationToolingV2Evidence *evidence, unsigned char *artifact,
    size_t artifact_capacity, size_t *written, unsigned char artifact_sha256[32]);

/* Validate, encode, then call the production v2 bootstrap importer. The DB is
 * untouched when validation or encoding fails. */
Lardon3DCalibrationToolingV2Result lardon3d_calibration_tooling_v2_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const Lardon3DCalibrationToolingV2Evidence *evidence, unsigned char *artifact,
    size_t artifact_capacity, size_t *written,
    Lardon3DCalibrationBootstrapV2Output *output);

#ifdef __cplusplus
}
#endif

#endif
