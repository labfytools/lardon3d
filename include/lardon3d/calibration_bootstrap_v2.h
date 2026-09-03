#ifndef LARDON3D_CALIBRATION_BOOTSTRAP_V2_H
#define LARDON3D_CALIBRATION_BOOTSTRAP_V2_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>
#include <lardon3d/sparse_sfm_model.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_ARTIFACT_VERSION = 2,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_BYTES = 600000,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_GROUPS = 4096,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_ENTRIES = 4096,
};

typedef enum {
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK = 0,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_PROVENANCE_MISMATCH,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_SELECTION_CONFLICT,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_DB_ERROR,
  LARDON3D_CALIBRATION_BOOTSTRAP_V2_OUT_OF_MEMORY,
} Lardon3DCalibrationBootstrapV2Result;

typedef struct {
  unsigned char artifact_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE];
  Lardon3DSparseCalibrationScope scope;
  uint32_t calibration_count;
  uint32_t group_count;
} Lardon3DCalibrationBootstrapV2Output;

typedef struct {
  uint32_t selected_item_index;
  uint64_t image_id;
  uint64_t calibration_id;
} Lardon3DCalibrationBootstrapV2Member;

/* Validate and publish one complete artifact without attaching its scope.
 * `members` is caller-owned storage of at least V2_MAX_ENTRIES only when that
 * many entries are accepted; `member_capacity` may be smaller and yields
 * INVALID_ARGUMENT before database mutation when the artifact will not fit.
 * Successful members are ordered by selected_item_index and expose the exact
 * immutable calibration created/reused for each selected image.
 *
 * This primitive exists so Workflow v2 can establish exact Capture-owned v26
 * applicability and selection before the single READY attachment. It never
 * infers Capture identity from image, path, digest, or group identity, and it
 * never attaches the returned complete scope. Exact retry is deterministic;
 * immutable calibrations and the unattached scope may survive later workflow
 * failure under the existing publication contract. */
Lardon3DCalibrationBootstrapV2Result
lardon3d_calibration_bootstrap_v2_publish_unattached(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE],
    Lardon3DCalibrationBootstrapV2Member *members, size_t member_capacity,
    Lardon3DCalibrationBootstrapV2Output *output);

/* Import one complete L3DCALB2 artifact for an immutable selected execution.
 * All pointers are required and borrowed only for the call; artifact_size is
 * bounded by V2_MAX_BYTES and must match expected_artifact_sha256 before any
 * parsing or database access. The fixed-width little-endian format accepts
 * only the existing eight-parameter PINHOLE model and complete, nonoverlapping
 * coverage of selected item indexes, image IDs, and representation SHA-256s.
 *
 * Each serialized group is hashed independently and that digest becomes every
 * member calibration's IMPORTED_TRUSTED provenance fingerprint. Thus target,
 * optical-state, solver, initialization, validation, and exact membership
 * evidence remain group-local even when one scope contains heterogeneous
 * groups. Validation completes before publication. Existing immutable
 * calibrations and one exact all-image scope are reused on retry; the scope is
 * attached only after complete publication, so a failure cannot make the
 * execution READY. No Capture identity, solver result beyond intrinsics, or
 * compatibility relation is inferred by this C ABI. */
Lardon3DCalibrationBootstrapV2Result lardon3d_calibration_bootstrap_v2_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE],
    Lardon3DCalibrationBootstrapV2Output *output);

#ifdef __cplusplus
}
#endif

#endif
