#include <lardon3d/calibration_bootstrap_v2.h>

#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  BOOTSTRAP_V2_HEADER_SIZE = 28,
  BOOTSTRAP_V2_GROUP_SIZE = 232,
  BOOTSTRAP_V2_ENTRY_SIZE = 144,
  BOOTSTRAP_V2_VALIDATION_FLAGS = 15,
};

static const unsigned char bootstrap_v2_magic[8] = {'L', '3', 'D', 'C', 'A', 'L', 'B', '2'};

typedef struct {
  const unsigned char *bytes;
  size_t remaining;
} BootstrapV2Reader;

typedef struct {
  uint32_t selected_item_index;
  unsigned char representation_sha256[32];
  Lardon3DSparseCalibration calibration;
  Lardon3DSparseCalibrationMember member;
} BootstrapV2Entry;

static bool take(BootstrapV2Reader *reader, void *output, size_t count) {
  if (count > reader->remaining) return false;
  if (output) memcpy(output, reader->bytes, count);
  reader->bytes += count;
  reader->remaining -= count;
  return true;
}

static bool read_u32(BootstrapV2Reader *reader, uint32_t *output) {
  unsigned char bytes[4];
  if (!take(reader, bytes, sizeof(bytes))) return false;
  *output = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
            ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  return true;
}

static bool read_u64(BootstrapV2Reader *reader, uint64_t *output) {
  unsigned char bytes[8];
  if (!take(reader, bytes, sizeof(bytes))) return false;
  *output = 0;
  for (size_t index = 0; index < sizeof(bytes); ++index)
    *output |= (uint64_t)bytes[index] << (8u * index);
  return true;
}

static bool read_f64(BootstrapV2Reader *reader, double *output) {
  uint64_t bits = 0;
  if (!read_u64(reader, &bits)) return false;
  memcpy(output, &bits, sizeof(bits));
  if (!isfinite(*output)) return false;
  if (*output == 0.0) *output = 0.0;
  return true;
}

static bool sha256(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(bytes, size, output, &output_size, EVP_sha256(), NULL) == 1 &&
         output_size == 32;
}

static bool nonzero_digest(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t index = 0; index < 32; ++index) any |= value[index];
  return any != 0;
}

static Lardon3DCalibrationBootstrapV2Result db_result(Lardon3DProjectDbResult result) {
  if (result == LARDON3D_PROJECT_DB_CONSTRAINT || result == LARDON3D_PROJECT_DB_NOT_FOUND)
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_SELECTION_CONFLICT;
  return LARDON3D_CALIBRATION_BOOTSTRAP_V2_DB_ERROR;
}

static Lardon3DCalibrationBootstrapV2Result bootstrap_v2_publish(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[32],
    Lardon3DCalibrationBootstrapV2Member *published_members,
    size_t member_capacity, bool attach_scope,
    Lardon3DCalibrationBootstrapV2Output *output) {
  if (!database || execution_id == 0 || !artifact || !expected_artifact_sha256 || !output ||
      artifact_size < BOOTSTRAP_V2_HEADER_SIZE ||
      artifact_size > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_BYTES)
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_INVALID_ARGUMENT;
  memset(output, 0, sizeof(*output));

  unsigned char artifact_sha256[32];
  if (!sha256(artifact, artifact_size, artifact_sha256))
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_DB_ERROR;
  if (memcmp(artifact_sha256, expected_artifact_sha256, 32) != 0)
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_PROVENANCE_MISMATCH;

  BootstrapV2Reader reader = {artifact, artifact_size};
  unsigned char magic[8];
  uint32_t version, model_kind, model_version, group_count, entry_count;
  if (!take(&reader, magic, sizeof(magic)) || !read_u32(&reader, &version) ||
      !read_u32(&reader, &model_kind) || !read_u32(&reader, &model_version) ||
      !read_u32(&reader, &group_count) || !read_u32(&reader, &entry_count) ||
      memcmp(magic, bootstrap_v2_magic, sizeof(magic)) != 0 ||
      version != LARDON3D_CALIBRATION_BOOTSTRAP_V2_ARTIFACT_VERSION ||
      model_kind != LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE ||
      model_version != LARDON3D_SPARSE_SFM_CALIBRATION_VERSION || group_count == 0 ||
      group_count > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_GROUPS || entry_count == 0 ||
      entry_count > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_ENTRIES)
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
  if (!attach_scope && (!published_members || member_capacity < entry_count))
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_INVALID_ARGUMENT;
  size_t expected_size = BOOTSTRAP_V2_HEADER_SIZE + (size_t)group_count * BOOTSTRAP_V2_GROUP_SIZE;
  if (entry_count > (SIZE_MAX - expected_size) / BOOTSTRAP_V2_ENTRY_SIZE)
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
  expected_size += (size_t)entry_count * BOOTSTRAP_V2_ENTRY_SIZE;
  if (artifact_size != expected_size)
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;

  BootstrapV2Entry *entries = calloc(entry_count, sizeof(*entries));
  bool *covered = calloc(entry_count, sizeof(*covered));
  if (!entries || !covered) {
    free(entries);
    free(covered);
    return LARDON3D_CALIBRATION_BOOTSTRAP_V2_OUT_OF_MEMORY;
  }

  Lardon3DCalibrationBootstrapV2Result result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK;
  size_t parsed_entries = 0;
  unsigned char previous_group_identity[32] = {0};
  uint32_t previous_group_version = 0;
  for (uint32_t group_index = 0;
       group_index < group_count && result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK;
       ++group_index) {
    const unsigned char *group_start = reader.bytes;
    unsigned char group_identity[32], evidence[6][32];
    uint32_t group_version, member_count;
    if (!take(&reader, group_identity, 32) || !read_u32(&reader, &group_version)) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
      break;
    }
    for (size_t index = 0; index < 6; ++index)
      if (!take(&reader, evidence[index], 32)) {
        result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
        break;
      }
    if (result != LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK ||
        !read_u32(&reader, &member_count)) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
      break;
    }
    int group_order = group_index == 0 ? -1 :
        memcmp(previous_group_identity, group_identity, 32);
    if (!nonzero_digest(group_identity) || group_version == 0 || member_count == 0 ||
        member_count > entry_count - parsed_entries || group_order > 0 ||
        (group_order == 0 && previous_group_version >= group_version)) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
      break;
    }
    for (size_t index = 0; index < 6; ++index)
      if (!nonzero_digest(evidence[index])) {
        result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
        break;
      }
    memcpy(previous_group_identity, group_identity, 32);
    previous_group_version = group_version;
    size_t group_first_entry = parsed_entries;
    uint32_t previous_item_index = 0;
    for (uint32_t member_index = 0;
         member_index < member_count && result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK;
         ++member_index) {
      BootstrapV2Entry *entry = &entries[parsed_entries];
      uint32_t width, height, support_images, support_observations, validation_flags;
      double reprojection_rmse_px, maximum_parameter_delta;
      if (!read_u32(&reader, &entry->selected_item_index) ||
          !read_u64(&reader, &entry->member.image_id) ||
          !take(&reader, entry->representation_sha256, 32) || !read_u32(&reader, &width) ||
          !read_u32(&reader, &height) || !read_f64(&reader, &entry->calibration.fx) ||
          !read_f64(&reader, &entry->calibration.fy) ||
          !read_f64(&reader, &entry->calibration.cx) ||
          !read_f64(&reader, &entry->calibration.cy) ||
          !read_f64(&reader, &entry->calibration.k1) ||
          !read_f64(&reader, &entry->calibration.k2) ||
          !read_f64(&reader, &entry->calibration.p1) ||
          !read_f64(&reader, &entry->calibration.p2) ||
          !read_u32(&reader, &support_images) ||
          !read_u32(&reader, &support_observations) ||
          !read_f64(&reader, &reprojection_rmse_px) ||
          !read_f64(&reader, &maximum_parameter_delta) ||
          !read_u32(&reader, &validation_flags) ||
          entry->selected_item_index >= entry_count || covered[entry->selected_item_index] ||
          (member_index > 0 && previous_item_index >= entry->selected_item_index) ||
          entry->member.image_id == 0 || !nonzero_digest(entry->representation_sha256) ||
          width == 0 || height == 0 || entry->calibration.fx <= 0.0 ||
          entry->calibration.fy <= 0.0 || entry->calibration.cx < 0.0 ||
          entry->calibration.cy < 0.0 || entry->calibration.cx >= (double)width ||
          entry->calibration.cy >= (double)height || support_images == 0 ||
          support_observations == 0 || reprojection_rmse_px < 0.0 ||
          maximum_parameter_delta < 0.0 || validation_flags != BOOTSTRAP_V2_VALIDATION_FLAGS) {
        result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
        break;
      }
      for (size_t prior = 0; prior < parsed_entries; ++prior)
        if (entries[prior].member.image_id == entry->member.image_id) {
          result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
          break;
        }
      covered[entry->selected_item_index] = true;
      previous_item_index = entry->selected_item_index;
      entry->calibration.model_kind = model_kind;
      entry->calibration.model_version = model_version;
      entry->calibration.width = width;
      entry->calibration.height = height;
      entry->calibration.provenance_kind =
          LARDON3D_SPARSE_SFM_PROVENANCE_IMPORTED_TRUSTED;
      ++parsed_entries;
    }
    if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK) {
      unsigned char group_fingerprint[32];
      size_t group_size = (size_t)(reader.bytes - group_start);
      if (!sha256(group_start, group_size, group_fingerprint)) {
        result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_DB_ERROR;
        break;
      }
      /* Group-local hashing preserves independently acquired provenance: an
       * unrelated group's addition cannot redefine these calibrations. */
      for (size_t index = group_first_entry; index < parsed_entries; ++index)
        memcpy(entries[index].calibration.provenance_fingerprint, group_fingerprint, 32);
    }
  }
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK &&
      (parsed_entries != entry_count || reader.remaining != 0))
    result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;
  for (uint32_t index = 0;
       index < entry_count && result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK; ++index)
    if (!covered[index]) result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT;

  Lardon3DProjectDbSelectedExecution execution;
  Lardon3DProjectDbResult db_status = LARDON3D_PROJECT_DB_OK;
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK) {
    db_status = lardon3d_project_db_load_selected_execution(database, execution_id, &execution);
    if (db_status != LARDON3D_PROJECT_DB_OK)
      result = db_result(db_status);
    else if (execution.item_count != entry_count ||
             (execution.stage != LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
              execution.stage != LARDON3D_SELECTED_EXECUTION_READY))
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_SELECTION_CONFLICT;
  }
  for (size_t index = 0;
       index < parsed_entries && result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK; ++index) {
    BootstrapV2Entry *entry = &entries[index];
    Lardon3DProjectDbSelectedExecutionItem selected;
    Lardon3DProjectDbImage image;
    Lardon3DProjectDbImageAsset asset;
    db_status = lardon3d_project_db_load_selected_execution_item(
        database, execution_id, entry->selected_item_index, &selected);
    if (db_status != LARDON3D_PROJECT_DB_OK || !selected.has_image ||
        selected.image_id != entry->member.image_id) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_SELECTION_CONFLICT;
      break;
    }
    db_status = lardon3d_project_db_load_image(database, entry->member.image_id, &image, &asset);
    if (db_status != LARDON3D_PROJECT_DB_OK ||
        memcmp(asset.sha256, entry->representation_sha256, 32) != 0) {
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_SELECTION_CONFLICT;
      break;
    }
  }

  /* All bytes and immutable selected bindings are verified before this first
   * write. Publication uses the existing short, content-addressed DB APIs; a
   * crash may retain reusable calibrations but READY remains guarded by the
   * single complete-scope attachment below. */
  for (size_t index = 0;
       index < parsed_entries && result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK; ++index) {
    Lardon3DSparseCalibration stored;
    db_status = lardon3d_sparse_calibration_create(database, &entries[index].calibration,
                                                    &stored);
    if (db_status != LARDON3D_PROJECT_DB_OK) {
      result = db_result(db_status);
      break;
    }
    entries[index].member.calibration_id = stored.calibration_id;
    memcpy(entries[index].member.calibration_hash, stored.scientific_hash, 32);
  }

  Lardon3DSparseCalibrationScope scope;
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK) {
    Lardon3DSparseCalibrationMember *members = calloc(entry_count, sizeof(*members));
    if (!members)
      result = LARDON3D_CALIBRATION_BOOTSTRAP_V2_OUT_OF_MEMORY;
    else {
      for (size_t index = 0; index < entry_count; ++index) members[index] = entries[index].member;
      db_status = lardon3d_sparse_calibration_scope_create(database, members, entry_count, &scope);
      free(members);
      if (db_status != LARDON3D_PROJECT_DB_OK) result = db_result(db_status);
    }
  }
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK && attach_scope) {
    db_status = lardon3d_project_db_assign_selected_calibration_scope(database, execution_id,
                                                                      scope.scope_id);
    if (db_status != LARDON3D_PROJECT_DB_OK) result = db_result(db_status);
  }
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK) {
    memcpy(output->artifact_sha256, artifact_sha256, 32);
    output->scope = scope;
    output->calibration_count = entry_count;
    output->group_count = group_count;
    if (!attach_scope) {
      /* Item order, not artifact group order, is the durable composition key.
       * Workflow must bind it back to selected_execution_items.capture_id and
       * must never reverse-map Capture identity from image_id. */
      for (size_t index = 0; index < entry_count; ++index) {
        BootstrapV2Entry *entry = &entries[index];
        Lardon3DCalibrationBootstrapV2Member *member =
            &published_members[entry->selected_item_index];
        member->selected_item_index = entry->selected_item_index;
        member->image_id = entry->member.image_id;
        member->calibration_id = entry->member.calibration_id;
      }
    }
  }
  free(entries);
  free(covered);
  return result;
}

Lardon3DCalibrationBootstrapV2Result
lardon3d_calibration_bootstrap_v2_publish_unattached(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[32],
    Lardon3DCalibrationBootstrapV2Member *members, size_t member_capacity,
    Lardon3DCalibrationBootstrapV2Output *output) {
  return bootstrap_v2_publish(database, execution_id, artifact, artifact_size,
                              expected_artifact_sha256, members,
                              member_capacity, false, output);
}

Lardon3DCalibrationBootstrapV2Result lardon3d_calibration_bootstrap_v2_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[32],
    Lardon3DCalibrationBootstrapV2Output *output) {
  return bootstrap_v2_publish(database, execution_id, artifact, artifact_size,
                              expected_artifact_sha256, NULL, 0, true, output);
}
