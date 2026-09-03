#include <lardon3d/calibration_tooling_v2.h>

#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <string.h>

enum {
  TOOLING_V2_HEADER_SIZE = 28,
  TOOLING_V2_GROUP_SIZE = 232,
  TOOLING_V2_ENTRY_SIZE = 144,
};

static bool nonzero_digest(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t index = 0; index < 32; ++index) any |= value[index];
  return any != 0;
}

static void put_u32(unsigned char *output, uint32_t value) {
  for (size_t index = 0; index < 4; ++index)
    output[index] = (unsigned char)(value >> (8u * index));
}

static void put_u64(unsigned char *output, uint64_t value) {
  for (size_t index = 0; index < 8; ++index)
    output[index] = (unsigned char)(value >> (8u * index));
}

static void put_f64(unsigned char *output, double value) {
  uint64_t bits = 0;
  /* The format has one representation of zero so equal scientific values do
   * not acquire different artifact identities through a negative sign bit. */
  if (value == 0.0) value = 0.0;
  memcpy(&bits, &value, sizeof(bits));
  put_u64(output, bits);
}

static bool sha256(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(bytes, size, output, &output_size, EVP_sha256(), NULL) == 1 &&
         output_size == 32;
}

static bool entry_valid(const Lardon3DCalibrationToolingV2Entry *entry) {
  const double parameters[] = {entry->fx, entry->fy, entry->cx, entry->cy,
                               entry->k1, entry->k2, entry->p1, entry->p2};
  if (entry->image_id == 0 || !nonzero_digest(entry->representation_sha256) ||
      entry->width == 0 || entry->height == 0 || entry->support_images == 0 ||
      entry->support_observations == 0 ||
      entry->validation_flags != LARDON3D_CALIBRATION_TOOLING_V2_VALIDATION_FLAGS)
    return false;
  for (size_t index = 0; index < sizeof(parameters) / sizeof(parameters[0]); ++index)
    if (!isfinite(parameters[index])) return false;
  return entry->fx > 0.0 && entry->fy > 0.0 && entry->cx >= 0.0 && entry->cy >= 0.0 &&
         entry->cx < (double)entry->width && entry->cy < (double)entry->height &&
         isfinite(entry->reprojection_rmse_px) && entry->reprojection_rmse_px >= 0.0 &&
         isfinite(entry->maximum_parameter_delta) && entry->maximum_parameter_delta >= 0.0;
}

Lardon3DCalibrationToolingV2Result lardon3d_calibration_tooling_v2_validate(
    const Lardon3DCalibrationToolingV2Evidence *evidence) {
  if (!evidence || !evidence->groups || evidence->group_count == 0 ||
      evidence->group_count > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_GROUPS ||
      evidence->entry_count == 0 ||
      evidence->entry_count > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_ENTRIES)
    return LARDON3D_CALIBRATION_TOOLING_V2_INVALID_ARGUMENT;

  bool covered[LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_ENTRIES] = {false};
  size_t total_entries = 0;
  for (size_t group_index = 0; group_index < evidence->group_count; ++group_index) {
    const Lardon3DCalibrationToolingV2Group *group = &evidence->groups[group_index];
    if (!group->entries || group->entry_count == 0 || group->group_version == 0 ||
        !nonzero_digest(group->group_identity_sha256) ||
        !nonzero_digest(group->optical_state_sha256) || !nonzero_digest(group->target_sha256) ||
        !nonzero_digest(group->solver_executable_sha256) ||
        !nonzero_digest(group->solver_configuration_sha256) ||
        !nonzero_digest(group->initialization_evidence_sha256) ||
        !nonzero_digest(group->validation_evidence_sha256))
      return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
    if (group_index > 0) {
      const Lardon3DCalibrationToolingV2Group *previous = &evidence->groups[group_index - 1];
      int order = memcmp(previous->group_identity_sha256, group->group_identity_sha256, 32);
      if (order > 0 || (order == 0 && previous->group_version >= group->group_version))
        return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
    }
    if (group->entry_count > evidence->entry_count - total_entries)
      return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
    total_entries += group->entry_count;
    for (size_t entry_index = 0; entry_index < group->entry_count; ++entry_index) {
      const Lardon3DCalibrationToolingV2Entry *entry = &group->entries[entry_index];
      if (!entry_valid(entry) || entry->selected_item_index >= evidence->entry_count ||
          covered[entry->selected_item_index] ||
          (entry_index > 0 &&
           group->entries[entry_index - 1].selected_item_index >= entry->selected_item_index))
        return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
      covered[entry->selected_item_index] = true;
      /* Scientific images cannot belong to two groups under different item
       * indexes; item coverage alone would not detect that identity conflict. */
      for (size_t prior_group = 0; prior_group <= group_index; ++prior_group) {
        const Lardon3DCalibrationToolingV2Group *candidate_group =
            &evidence->groups[prior_group];
        size_t limit = prior_group == group_index ? entry_index : candidate_group->entry_count;
        for (size_t prior_entry = 0; prior_entry < limit; ++prior_entry)
          if (candidate_group->entries[prior_entry].image_id == entry->image_id)
            return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
      }
    }
  }
  if (total_entries != evidence->entry_count)
    return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
  for (size_t index = 0; index < evidence->entry_count; ++index)
    if (!covered[index]) return LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED;
  return LARDON3D_CALIBRATION_TOOLING_V2_OK;
}

Lardon3DCalibrationToolingV2Result lardon3d_calibration_tooling_v2_produce(
    const Lardon3DCalibrationToolingV2Evidence *evidence, unsigned char *artifact,
    size_t artifact_capacity, size_t *written, unsigned char artifact_sha256[32]) {
  if (written) *written = 0;
  if (!artifact || !artifact_sha256) return LARDON3D_CALIBRATION_TOOLING_V2_INVALID_ARGUMENT;
  Lardon3DCalibrationToolingV2Result result =
      lardon3d_calibration_tooling_v2_validate(evidence);
  if (result != LARDON3D_CALIBRATION_TOOLING_V2_OK) return result;
  if (evidence->group_count > (SIZE_MAX - TOOLING_V2_HEADER_SIZE) / TOOLING_V2_GROUP_SIZE)
    return LARDON3D_CALIBRATION_TOOLING_V2_CAPACITY;
  size_t size = TOOLING_V2_HEADER_SIZE + evidence->group_count * TOOLING_V2_GROUP_SIZE;
  if (evidence->entry_count > (SIZE_MAX - size) / TOOLING_V2_ENTRY_SIZE)
    return LARDON3D_CALIBRATION_TOOLING_V2_CAPACITY;
  size += evidence->entry_count * TOOLING_V2_ENTRY_SIZE;
  if (size > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_BYTES || artifact_capacity < size)
    return LARDON3D_CALIBRATION_TOOLING_V2_CAPACITY;

  size_t at = 0;
  memcpy(artifact + at, "L3DCALB2", 8);
  at += 8;
#define PUT32(value) do { put_u32(artifact + at, (uint32_t)(value)); at += 4; } while (0)
  PUT32(LARDON3D_CALIBRATION_TOOLING_V2_VERSION);
  PUT32(LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE);
  PUT32(LARDON3D_SPARSE_SFM_CALIBRATION_VERSION);
  PUT32(evidence->group_count);
  PUT32(evidence->entry_count);
  for (size_t group_index = 0; group_index < evidence->group_count; ++group_index) {
    const Lardon3DCalibrationToolingV2Group *group = &evidence->groups[group_index];
    const unsigned char *digests[] = {
        group->group_identity_sha256, group->optical_state_sha256, group->target_sha256,
        group->solver_executable_sha256, group->solver_configuration_sha256,
        group->initialization_evidence_sha256, group->validation_evidence_sha256};
    memcpy(artifact + at, digests[0], 32); at += 32;
    PUT32(group->group_version);
    for (size_t digest_index = 1; digest_index < 7; ++digest_index) {
      memcpy(artifact + at, digests[digest_index], 32);
      at += 32;
    }
    PUT32(group->entry_count);
    for (size_t entry_index = 0; entry_index < group->entry_count; ++entry_index) {
      const Lardon3DCalibrationToolingV2Entry *entry = &group->entries[entry_index];
      PUT32(entry->selected_item_index);
      put_u64(artifact + at, entry->image_id); at += 8;
      memcpy(artifact + at, entry->representation_sha256, 32); at += 32;
      PUT32(entry->width); PUT32(entry->height);
      const double parameters[] = {entry->fx, entry->fy, entry->cx, entry->cy,
                                   entry->k1, entry->k2, entry->p1, entry->p2};
      for (size_t index = 0; index < 8; ++index) {
        put_f64(artifact + at, parameters[index]); at += 8;
      }
      PUT32(entry->support_images); PUT32(entry->support_observations);
      put_f64(artifact + at, entry->reprojection_rmse_px); at += 8;
      put_f64(artifact + at, entry->maximum_parameter_delta); at += 8;
      PUT32(entry->validation_flags);
    }
  }
#undef PUT32
  if (at != size || !sha256(artifact, size, artifact_sha256))
    return LARDON3D_CALIBRATION_TOOLING_V2_ENCODING_ERROR;
  if (written) *written = size;
  return LARDON3D_CALIBRATION_TOOLING_V2_OK;
}

Lardon3DCalibrationToolingV2Result lardon3d_calibration_tooling_v2_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const Lardon3DCalibrationToolingV2Evidence *evidence, unsigned char *artifact,
    size_t artifact_capacity, size_t *written,
    Lardon3DCalibrationBootstrapV2Output *output) {
  unsigned char artifact_sha256[32];
  size_t local_written = 0;
  size_t *encoded_size = written ? written : &local_written;
  Lardon3DCalibrationToolingV2Result result = lardon3d_calibration_tooling_v2_produce(
      evidence, artifact, artifact_capacity, encoded_size, artifact_sha256);
  if (result != LARDON3D_CALIBRATION_TOOLING_V2_OK) return result;
  if (!database || execution_id == 0 || !output)
    return LARDON3D_CALIBRATION_TOOLING_V2_INVALID_ARGUMENT;
  return lardon3d_calibration_bootstrap_v2_import(
             database, execution_id, artifact, *encoded_size, artifact_sha256, output) ==
                 LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK
             ? LARDON3D_CALIBRATION_TOOLING_V2_OK
             : LARDON3D_CALIBRATION_TOOLING_V2_IMPORT_ERROR;
}
