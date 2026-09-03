#ifndef LARDON3D_CALIBRATION_WORKFLOW_H
#define LARDON3D_CALIBRATION_WORKFLOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_WORKFLOW_VERSION = 1,
  LARDON3D_CALIBRATION_WORKFLOW_MAX_FILE_BYTES = 128 * 1024 * 1024,
  LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS = 4096,
  LARDON3D_CALIBRATION_WORKFLOW_OPTICAL_STATE_TOKEN_CAPACITY = 1025,
};

typedef enum {
  LARDON3D_CALIBRATION_WORKFLOW_OK = 0,
  LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR,
  LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE,
  LARDON3D_CALIBRATION_WORKFLOW_CAPACITY,
  LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE,
  LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH,
} Lardon3DCalibrationWorkflowResult;

/* `campaign_state_path` is a canonical external acquisition manifest:
 *
 * L3DCAL_CAMPAIGN_STATE_V1
 * execution <selected-execution-id>
 * optical_configuration <explicit-v23-configuration-id>
 * optical_state <sha256> <complete-Science-v1-state-token>
 * capture <zero-based-selected-item-index> <capture-id>
 *
 * Capture rows are contiguous and ordered. The later DB-binding stage proves
 * that these IDs are exactly the selected execution and that each Capture owns
 * the declared explicit optical configuration; this input boundary performs no
 * DB access and therefore never treats the manifest alone as that proof. */
typedef struct {
  const char *session_path;
  const char *detection_path;
  const char *solve_path;
  const char *evidence_path;
  const char *producer_path;
  const char *campaign_state_path;
} Lardon3DCalibrationWorkflowInputFiles;

typedef struct {
  unsigned char session_sha256[32];
  unsigned char detection_sha256[32];
  unsigned char solve_sha256[32];
  unsigned char evidence_sha256[32];
  unsigned char producer_sha256[32];
  unsigned char campaign_state_sha256[32];
  unsigned char optical_state_sha256[32];
  unsigned char solver_executable_sha256[32];
  unsigned char solver_configuration_sha256[32];
  uint64_t selected_execution_id;
  uint64_t optical_configuration_id;
  uint32_t capture_count;
  uint64_t capture_ids[LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS];
  char optical_state_token[LARDON3D_CALIBRATION_WORKFLOW_OPTICAL_STATE_TOKEN_CAPACITY];
} Lardon3DCalibrationWorkflowInputBoundary;

/* Validate only the immutable external input boundary. This function performs
 * no Project DB access, no calibration solve, no Tooling import and no
 * selected-execution mutation. Every path is opened O_NONBLOCK/O_NOFOLLOW,
 * must resolve to a bounded regular file, and is SHA-256 checked before its
 * syntax/provenance is consumed. */
Lardon3DCalibrationWorkflowResult lardon3d_calibration_workflow_validate_input_boundary(
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationWorkflowInputBoundary *boundary);

#ifdef __cplusplus
}
#endif

#endif
