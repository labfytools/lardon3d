#include <lardon3d/calibration_bootstrap.h>

#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  BOOTSTRAP_HEADER_SIZE = 152,
  BOOTSTRAP_ENTRY_SIZE = 140,
  BOOTSTRAP_VALIDATION_FLAGS = 15,
};

static const unsigned char bootstrap_magic[8] = {'L', '3', 'D', 'C', 'A', 'L', 'B', '1'};

typedef struct {
  const unsigned char *bytes;
  size_t remaining;
} BootstrapReader;

static bool take(BootstrapReader *reader, void *output, size_t count) {
  if (count > reader->remaining) return false;
  if (output) memcpy(output, reader->bytes, count);
  reader->bytes += count;
  reader->remaining -= count;
  return true;
}

static bool read_u32(BootstrapReader *reader, uint32_t *output) {
  unsigned char bytes[4];
  if (!take(reader, bytes, sizeof(bytes))) return false;
  *output = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
            ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  return true;
}

static bool read_u64(BootstrapReader *reader, uint64_t *output) {
  unsigned char bytes[8];
  if (!take(reader, bytes, sizeof(bytes))) return false;
  *output = 0;
  for (size_t index = 0; index < sizeof(bytes); ++index)
    *output |= (uint64_t)bytes[index] << (8u * index);
  return true;
}

static bool read_f64(BootstrapReader *reader, double *output) {
  uint64_t bits;
  if (!read_u64(reader, &bits)) return false;
  memcpy(output, &bits, sizeof(bits));
  if (*output == 0.0) *output = 0.0;
  return isfinite(*output);
}

static bool sha256(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(bytes, size, output, &output_size, EVP_sha256(), NULL) == 1 &&
         output_size == 32;
}

static Lardon3DCalibrationBootstrapResult db_result(Lardon3DProjectDbResult result) {
  if (result == LARDON3D_PROJECT_DB_CONSTRAINT || result == LARDON3D_PROJECT_DB_NOT_FOUND)
    return LARDON3D_CALIBRATION_BOOTSTRAP_SELECTION_CONFLICT;
  return LARDON3D_CALIBRATION_BOOTSTRAP_DB_ERROR;
}

Lardon3DCalibrationBootstrapResult lardon3d_calibration_bootstrap_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[32],
    Lardon3DCalibrationBootstrapOutput *output) {
  if (!database || execution_id == 0 || !artifact || !expected_artifact_sha256 || !output ||
      artifact_size < BOOTSTRAP_HEADER_SIZE ||
      artifact_size > LARDON3D_CALIBRATION_BOOTSTRAP_MAX_BYTES)
    return LARDON3D_CALIBRATION_BOOTSTRAP_INVALID_ARGUMENT;
  memset(output, 0, sizeof(*output));

  unsigned char artifact_sha256[32];
  if (!sha256(artifact, artifact_size, artifact_sha256))
    return LARDON3D_CALIBRATION_BOOTSTRAP_DB_ERROR;
  if (memcmp(artifact_sha256, expected_artifact_sha256, 32) != 0)
    return LARDON3D_CALIBRATION_BOOTSTRAP_PROVENANCE_MISMATCH;

  Lardon3DProjectDbSelectedExecution execution;
  Lardon3DProjectDbResult loaded =
      lardon3d_project_db_load_selected_execution(database, execution_id, &execution);
  if (loaded != LARDON3D_PROJECT_DB_OK) return db_result(loaded);
  if (execution.stage != LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
      execution.stage != LARDON3D_SELECTED_EXECUTION_READY)
    return LARDON3D_CALIBRATION_BOOTSTRAP_SELECTION_CONFLICT;

  BootstrapReader reader = {artifact, artifact_size};
  unsigned char magic[8];
  uint32_t version, model_kind, model_version, entry_count;
  unsigned char solver_executable_sha256[32];
  unsigned char solver_configuration_sha256[32];
  unsigned char initialization_evidence_sha256[32];
  unsigned char validation_evidence_sha256[32];
  if (!take(&reader, magic, sizeof(magic)) ||
      !read_u32(&reader, &version) || !read_u32(&reader, &model_kind) ||
      !read_u32(&reader, &model_version) || !read_u32(&reader, &entry_count) ||
      !take(&reader, solver_executable_sha256, 32) ||
      !take(&reader, solver_configuration_sha256, 32) ||
      !take(&reader, initialization_evidence_sha256, 32) ||
      !take(&reader, validation_evidence_sha256, 32) ||
      memcmp(magic, bootstrap_magic, sizeof(magic)) != 0 ||
      version != LARDON3D_CALIBRATION_BOOTSTRAP_ARTIFACT_VERSION ||
      model_kind != LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE ||
      model_version != LARDON3D_SPARSE_SFM_CALIBRATION_VERSION || entry_count == 0 ||
      entry_count != execution.item_count ||
      artifact_size != BOOTSTRAP_HEADER_SIZE + (size_t)entry_count * BOOTSTRAP_ENTRY_SIZE)
    return LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT;

  /* These four digests bind executable identity, exact solver configuration,
   * initialization evidence and validation diagnostics into artifact identity.
   * An all-zero digest is not evidence and is therefore rejected. */
  unsigned char executable_or = 0, configuration_or = 0, initialization_or = 0, validation_or = 0;
  for (size_t index = 0; index < 32; ++index) {
    executable_or |= solver_executable_sha256[index];
    configuration_or |= solver_configuration_sha256[index];
    initialization_or |= initialization_evidence_sha256[index];
    validation_or |= validation_evidence_sha256[index];
  }
  if (executable_or == 0 || configuration_or == 0 || initialization_or == 0 ||
      validation_or == 0)
    return LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT;

  Lardon3DSparseCalibration *calibrations = calloc(entry_count, sizeof(*calibrations));
  Lardon3DSparseCalibrationMember *members = calloc(entry_count, sizeof(*members));
  if (!calibrations || !members) {
    free(calibrations);
    free(members);
    return LARDON3D_CALIBRATION_BOOTSTRAP_OUT_OF_MEMORY;
  }

  Lardon3DCalibrationBootstrapResult result = LARDON3D_CALIBRATION_BOOTSTRAP_OK;
  for (uint32_t index = 0; index < entry_count && result == LARDON3D_CALIBRATION_BOOTSTRAP_OK;
       ++index) {
    uint64_t image_id;
    unsigned char representation_sha256[32];
    uint32_t width, height, support_images, support_observations, validation_flags;
    double reprojection_rmse, maximum_parameter_delta;
    Lardon3DSparseCalibration *calibration = &calibrations[index];
    if (!read_u64(&reader, &image_id) || !take(&reader, representation_sha256, 32) ||
        !read_u32(&reader, &width) || !read_u32(&reader, &height) ||
        !read_f64(&reader, &calibration->fx) || !read_f64(&reader, &calibration->fy) ||
        !read_f64(&reader, &calibration->cx) || !read_f64(&reader, &calibration->cy) ||
        !read_f64(&reader, &calibration->k1) || !read_f64(&reader, &calibration->k2) ||
        !read_f64(&reader, &calibration->p1) || !read_f64(&reader, &calibration->p2) ||
        !read_u32(&reader, &support_images) || !read_u32(&reader, &support_observations) ||
        !read_f64(&reader, &reprojection_rmse) ||
        !read_f64(&reader, &maximum_parameter_delta) ||
        !read_u32(&reader, &validation_flags) || image_id == 0 || width == 0 || height == 0 ||
        calibration->fx <= 0.0 || calibration->fy <= 0.0 || calibration->cx < 0.0 ||
        calibration->cy < 0.0 || calibration->cx >= (double)width ||
        calibration->cy >= (double)height || support_images == 0 || support_observations == 0 ||
        reprojection_rmse < 0.0 || maximum_parameter_delta < 0.0 ||
        validation_flags != BOOTSTRAP_VALIDATION_FLAGS) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT;
      break;
    }

    Lardon3DProjectDbSelectedExecutionItem selected;
    Lardon3DProjectDbImage image;
    Lardon3DProjectDbImageAsset asset;
    loaded = lardon3d_project_db_load_selected_execution_item(database, execution_id, index,
                                                               &selected);
    if (loaded != LARDON3D_PROJECT_DB_OK || !selected.has_image || selected.image_id != image_id) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_SELECTION_CONFLICT;
      break;
    }
    loaded = lardon3d_project_db_load_image(database, image_id, &image, &asset);
    if (loaded != LARDON3D_PROJECT_DB_OK || memcmp(asset.sha256, representation_sha256, 32) != 0) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_SELECTION_CONFLICT;
      break;
    }
    calibration->model_kind = model_kind;
    calibration->model_version = model_version;
    calibration->width = width;
    calibration->height = height;
    calibration->provenance_kind = LARDON3D_SPARSE_SFM_PROVENANCE_IMPORTED_TRUSTED;
    memcpy(calibration->provenance_fingerprint, artifact_sha256, 32);
    members[index].image_id = image_id;
  }
  if (reader.remaining != 0 && result == LARDON3D_CALIBRATION_BOOTSTRAP_OK)
    result = LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT;

  /* Validation precedes publication. The established calibration APIs own
   * short transactions; their immutable, content-addressed rows may survive a
   * later failure, while READY remains guarded by the final scope attachment. */
  for (uint32_t index = 0; index < entry_count && result == LARDON3D_CALIBRATION_BOOTSTRAP_OK;
       ++index) {
    Lardon3DSparseCalibration stored;
    loaded = lardon3d_sparse_calibration_create(database, &calibrations[index], &stored);
    if (loaded != LARDON3D_PROJECT_DB_OK) {
      result = db_result(loaded);
      break;
    }
    members[index].calibration_id = stored.calibration_id;
    memcpy(members[index].calibration_hash, stored.scientific_hash, 32);
  }
  Lardon3DSparseCalibrationScope scope;
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_OK) {
    loaded = lardon3d_sparse_calibration_scope_create(database, members, entry_count, &scope);
    if (loaded != LARDON3D_PROJECT_DB_OK) result = db_result(loaded);
  }
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_OK) {
    loaded = lardon3d_project_db_assign_selected_calibration_scope(database, execution_id,
                                                                   scope.scope_id);
    if (loaded != LARDON3D_PROJECT_DB_OK) result = db_result(loaded);
  }
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_OK) {
    memcpy(output->artifact_sha256, artifact_sha256, 32);
    output->scope = scope;
    output->calibration_count = entry_count;
  }
  free(calibrations);
  free(members);
  return result;
}
