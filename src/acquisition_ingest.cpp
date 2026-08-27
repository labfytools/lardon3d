#include <lardon3d/acquisition_ingest.h>

#include <lardon3d/acquisition_pairing.h>
extern "C" {
#include <lardon3d/image_catalog.h>
}
#include <lardon3d/raw_development.h>

#include <climits>
#include <cstdio>
#include <cstring>

namespace {

#ifdef LARDON3D_ACQUISITION_INGEST_TESTING
const Lardon3DAcquisitionMetadata *testing_metadata = nullptr;
size_t testing_metadata_count = 0u;
#endif

struct PublishedSource {
  Lardon3DProjectDbImageAsset asset;
  Lardon3DAcquisitionMetadata metadata;
  char managed_path[PATH_MAX];
};

bool valid_options(const Lardon3DAcquisitionIngestOptions &options) {
  return (options.grouping == LARDON3D_ACQUISITION_GROUP_AUTOMATIC ||
          options.grouping == LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT) &&
         (options.representation == LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE ||
          options.representation == LARDON3D_ACQUISITION_SELECT_DEVELOP_RAW) &&
         options.select_representation <= 1u && options.imported_at >= 0 &&
         options.max_source_bytes != 0u;
}

const char *base_name(const char *path) {
  const char *slash = std::strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

Lardon3DAcquisitionIngestResult db_result(Lardon3DProjectDbResult result) {
  if (result == LARDON3D_PROJECT_DB_CONSTRAINT) return LARDON3D_ACQUISITION_INGEST_CONSTRAINT;
  return result == LARDON3D_PROJECT_DB_NOT_FOUND ? LARDON3D_ACQUISITION_INGEST_SCANSET_NOT_FOUND
                                                 : LARDON3D_ACQUISITION_INGEST_DB_ERROR;
}

bool add_to_group(Lardon3DAcquisitionIngestGroup &group, size_t source_index) {
  if (group.source_count >= LARDON3D_ACQUISITION_INGEST_MAX_SOURCES) return false;
  group.source_indices[group.source_count++] = source_index;
  return true;
}

}  // namespace

#ifdef LARDON3D_ACQUISITION_INGEST_TESTING
extern "C" void lardon3d_acquisition_ingest_test_set_metadata(
    const Lardon3DAcquisitionMetadata *metadata, size_t count) {
  testing_metadata = metadata;
  testing_metadata_count = count;
}
#endif

extern "C" Lardon3DAcquisitionIngestResult lardon3d_acquisition_ingest(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionIngestSource *sources, size_t source_count,
    const Lardon3DAcquisitionIngestOptions *options, Lardon3DAcquisitionIngestOutput *output) {
  if (output != nullptr) std::memset(output, 0, sizeof(*output));
  if (state == nullptr || !state->project_loaded || state->project_db == nullptr || scanset_id == 0u ||
      sources == nullptr || source_count == 0u ||
      source_count > LARDON3D_ACQUISITION_INGEST_MAX_SOURCES || options == nullptr ||
      !valid_options(*options) || output == nullptr) return LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT;
  Lardon3DProjectDbScanSet scanset{};
  Lardon3DProjectDbResult database_result =
      lardon3d_project_db_load_scanset(state->project_db, scanset_id, &scanset);
  if (database_result != LARDON3D_PROJECT_DB_OK) return db_result(database_result);
  if (options->grouping == LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT) {
    for (size_t i = 0; i < source_count; ++i)
      if (sources[i].explicit_group == 0u) return LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT;
  }

  PublishedSource published[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES]{};
  for (size_t i = 0; i < source_count; ++i) {
    if (sources[i].source_path == nullptr || sources[i].source_path[0] == '\0')
      return LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT;
    if (lardon3d_image_catalog_publish_asset_file(state, sources[i].source_path,
            options->imported_at, options->max_source_bytes, &published[i].asset) !=
        LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED) return LARDON3D_ACQUISITION_INGEST_PUBLISH_ERROR;
    for (size_t previous = 0; previous < i; ++previous)
      if (published[previous].asset.asset_id == published[i].asset.asset_id)
        return LARDON3D_ACQUISITION_INGEST_DUPLICATE_ASSET;
    int length = std::snprintf(published[i].managed_path, sizeof(published[i].managed_path),
                               "%s/%s", state->project_path, published[i].asset.path);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(published[i].managed_path))
      return LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT;
    if (lardon3d_acquisition_extract_metadata(published[i].managed_path,
                                              &published[i].metadata) != LARDON3D_ACQUISITION_OK)
      return LARDON3D_ACQUISITION_INGEST_METADATA_ERROR;
#ifdef LARDON3D_ACQUISITION_INGEST_TESTING
    if (testing_metadata != nullptr) {
      if (testing_metadata_count != source_count) return LARDON3D_ACQUISITION_INGEST_INTERNAL_ERROR;
      published[i].metadata = testing_metadata[i];
    }
#endif
  }

  bool assigned[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES]{};
  if (options->grouping == LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT) {
    for (size_t i = 0; i < source_count; ++i) {
      if (assigned[i]) continue;
      Lardon3DAcquisitionIngestGroup &group = output->groups[output->group_count++];
      group.basis = LARDON3D_ACQUISITION_GROUP_EXPLICIT;
      for (size_t j = i; j < source_count; ++j) {
        if (sources[j].explicit_group == sources[i].explicit_group) {
          (void)add_to_group(group, j);
          assigned[j] = true;
        }
      }
    }
  } else {
    size_t strong_count[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES]{};
    size_t strong_partner[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES]{};
    for (size_t i = 0; i < source_count; ++i) for (size_t j = i + 1; j < source_count; ++j) {
      Lardon3DAcquisitionPairResult pair{};
      if (lardon3d_acquisition_compare(&published[i].metadata, &published[j].metadata, 0, 0,
                                       &pair) != LARDON3D_ACQUISITION_OK)
        return LARDON3D_ACQUISITION_INGEST_INTERNAL_ERROR;
      if (pair.decision == LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG) {
        ++strong_count[i]; ++strong_count[j]; strong_partner[i] = j; strong_partner[j] = i;
      }
    }
    for (size_t i = 0; i < source_count; ++i) {
      if (assigned[i]) continue;
      Lardon3DAcquisitionIngestGroup &group = output->groups[output->group_count++];
      (void)add_to_group(group, i); assigned[i] = true;
      size_t partner = strong_partner[i];
      if (strong_count[i] == 1u && strong_count[partner] == 1u && strong_partner[partner] == i) {
        (void)add_to_group(group, partner); assigned[partner] = true;
        group.basis = LARDON3D_ACQUISITION_GROUP_STRONG;
      } else group.basis = LARDON3D_ACQUISITION_GROUP_SINGLETON;
    }
  }
  if (options->resume_capture_id != 0u && output->group_count != 1u)
    return LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT;

  for (size_t group_index = 0; group_index < output->group_count; ++group_index) {
    Lardon3DAcquisitionIngestGroup &group = output->groups[group_index];
    Lardon3DProjectDbCapture capture{};
    if (options->resume_capture_id != 0u) {
      database_result = lardon3d_project_db_load_capture(state->project_db,
                                                         options->resume_capture_id, &capture);
      if (database_result != LARDON3D_PROJECT_DB_OK) return db_result(database_result);
      if (capture.scanset_id != scanset_id) return LARDON3D_ACQUISITION_INGEST_CONSTRAINT;
    } else {
      database_result = lardon3d_project_db_create_capture(state->project_db, scanset_id,
                                                            options->imported_at, &capture);
      if (database_result != LARDON3D_PROJECT_DB_OK) return db_result(database_result);
    }
    group.capture_id = capture.capture_id;
    size_t jpeg = SIZE_MAX, raw = SIZE_MAX;
    for (size_t member = 0; member < group.source_count; ++member) {
      size_t index = group.source_indices[member];
      database_result = lardon3d_project_db_attach_capture_source_asset(
          state->project_db, capture.capture_id, published[index].asset.asset_id);
      if (database_result != LARDON3D_PROJECT_DB_OK) return db_result(database_result);
      if (published[index].metadata.source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG && jpeg == SIZE_MAX)
        jpeg = index;
      if (published[index].metadata.source_kind == LARDON3D_ACQUISITION_SOURCE_RAW && raw == SIZE_MAX)
        raw = index;
    }
    bool use_jpeg = jpeg != SIZE_MAX &&
        (options->representation == LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE || raw == SIZE_MAX);
    if (use_jpeg) {
      Lardon3DProjectDbImage image{};
      Lardon3DProjectDbImageRegisterStatus status{};
      database_result = lardon3d_project_db_publish_source_capture_image(
          state->project_db, capture.capture_id, published[jpeg].asset.asset_id,
          base_name(sources[jpeg].source_path), sources[jpeg].source_path,
          options->producer_task_id, options->imported_at, options->select_representation != 0u,
          &status, &image);
      if (database_result != LARDON3D_PROJECT_DB_OK) return db_result(database_result);
    } else if (raw != SIZE_MAX) {
      Lardon3DRawDevelopmentOutput developed{};
      Lardon3DRawDevelopmentResult result = lardon3d_raw_develop_to_capture(
          state, capture.capture_id, published[raw].managed_path, options->producer_task_id,
          options->imported_at, &developed);
      if (result != LARDON3D_RAW_DEVELOPMENT_OK)
        return LARDON3D_ACQUISITION_INGEST_DEVELOPMENT_ERROR;
      if (options->select_representation) {
        database_result = lardon3d_project_db_set_selected_capture_image(
            state->project_db, capture.capture_id, developed.image.image_id);
        if (database_result != LARDON3D_PROJECT_DB_OK) return db_result(database_result);
      }
    } else return LARDON3D_ACQUISITION_INGEST_CONSTRAINT;
  }
  return LARDON3D_ACQUISITION_INGEST_OK;
}
