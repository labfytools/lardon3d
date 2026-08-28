#ifndef LARDON3D_CALIBRATION_BOOTSTRAP_H
#define LARDON3D_CALIBRATION_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>
#include <lardon3d/sparse_sfm_model.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_BOOTSTRAP_ARTIFACT_VERSION = 1,
  LARDON3D_CALIBRATION_BOOTSTRAP_MAX_BYTES = 600000,
};

typedef enum {
  LARDON3D_CALIBRATION_BOOTSTRAP_OK = 0,
  LARDON3D_CALIBRATION_BOOTSTRAP_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT,
  LARDON3D_CALIBRATION_BOOTSTRAP_PROVENANCE_MISMATCH,
  LARDON3D_CALIBRATION_BOOTSTRAP_SELECTION_CONFLICT,
  LARDON3D_CALIBRATION_BOOTSTRAP_DB_ERROR,
  LARDON3D_CALIBRATION_BOOTSTRAP_OUT_OF_MEMORY,
} Lardon3DCalibrationBootstrapResult;

typedef struct {
  unsigned char artifact_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE];
  Lardon3DSparseCalibrationScope scope;
  uint32_t calibration_count;
} Lardon3DCalibrationBootstrapOutput;

/* Import a complete Calibration Bootstrap v1 artifact for an existing selected
 * execution. All pointers are required and artifact bytes are borrowed only
 * for the call. The artifact is bounded by MAX_BYTES, uses the documented
 * fixed-width little-endian format, and must hash to expected_artifact_sha256.
 * Its ordered image IDs and representation SHA-256 values must exactly match
 * the execution's durable selected images. Only the exact OpenCV-compatible
 * eight-parameter pinhole model is accepted.
 *
 * Successful import creates/reuses immutable IMPORTED_TRUSTED calibrations,
 * creates/reuses their immutable scope, then attaches that scope through the
 * selected-execution contract. Exact retries converge. A failure after an
 * immutable calibration is created may retain that content-addressed evidence,
 * but cannot make the selected execution READY. The function imports no poses,
 * points, tracks, matches, EXIF calibration, or Capture identity. */
Lardon3DCalibrationBootstrapResult lardon3d_calibration_bootstrap_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE],
    Lardon3DCalibrationBootstrapOutput *output);

#ifdef __cplusplus
}
#endif

#endif
