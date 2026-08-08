#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/candidate_pair_gen.h>
#include <lardon3d/project_db.h>
#include <lardon3d/visual_index.h>

#include <openssl/sha.h>

static Lardon3DVisualIndexResult db_result(Lardon3DProjectDbResult result) {
  switch (result) {
  case LARDON3D_PROJECT_DB_OK:
    return LARDON3D_VISUAL_INDEX_OK;
  case LARDON3D_PROJECT_DB_NOT_FOUND:
    return LARDON3D_VISUAL_INDEX_NOT_FOUND;
  case LARDON3D_PROJECT_DB_BUSY:
    return LARDON3D_VISUAL_INDEX_DB_BUSY;
  case LARDON3D_PROJECT_DB_INVALID_ARGUMENT:
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  case LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA:
    return LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION;
  case LARDON3D_PROJECT_DB_CORRUPT:
    return LARDON3D_VISUAL_INDEX_CORRUPT;
  default:
    return LARDON3D_VISUAL_INDEX_DB_ERROR;
  }
}

Lardon3DVisualIndexResult lardon3d_candidate_pair_generate(
    const char *project_path, Lardon3DProjectDb *database, uint64_t visual_index_id,
    uint64_t source_feature_set_id, const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *stats) {
  if (stats) {
    stats->generated_count = 0;
    stats->skipped_count = 0;
    stats->queried_count = 0;
  }
  if (!project_path || !database || visual_index_id == 0 || source_feature_set_id == 0 ||
      !query_options || !stats || query_options->top_k == 0 ||
      query_options->top_k > LARDON3D_VISUAL_INDEX_TOP_K_MAX ||
      query_options->minimum_evidence_count > 1024 ||
      query_options->scanset_filter > LARDON3D_VISUAL_INDEX_OTHER_SCANSETS) {
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbFeatureSet source_set;
  Lardon3DProjectDbResult loaded =
      lardon3d_project_db_load_feature_set(database, source_feature_set_id, &source_set);
  if (loaded != LARDON3D_PROJECT_DB_OK) {
    return db_result(loaded);
  }
  Lardon3DVisualIndexCandidate *candidates = calloc(query_options->top_k, sizeof(*candidates));
  if (!candidates) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  size_t result_count = 0;
  Lardon3DVisualIndexResult result = lardon3d_visual_index_query(
      project_path, database, visual_index_id, source_feature_set_id, query_options, candidates,
      query_options->top_k, &result_count);
  if (result != LARDON3D_VISUAL_INDEX_OK) {
    free(candidates);
    return result;
  }
  stats->queried_count = (uint32_t)result_count;
  time_t now = time(NULL);
  if (now < 0) {
    free(candidates);
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  for (size_t i = 0; i < result_count; ++i) {
    uint64_t candidate_image_id = candidates[i].image_id;
    if (candidate_image_id == source_set.image_id) {
      continue;
    }
    uint64_t image_a = candidate_image_id < source_set.image_id ? candidate_image_id
                                                                : source_set.image_id;
    uint64_t image_b = candidate_image_id < source_set.image_id ? source_set.image_id
                                                                : candidate_image_id;
    Lardon3DProjectDbCandidatePair pair;
    Lardon3DProjectDbResult found =
        lardon3d_project_db_find_candidate_pair(database, image_a, image_b, &pair);
    if (found == LARDON3D_PROJECT_DB_OK) {
      ++stats->skipped_count;
    } else if (found == LARDON3D_PROJECT_DB_NOT_FOUND) {
      Lardon3DProjectDbResult created =
          lardon3d_project_db_create_candidate_pair(database, image_a, image_b, (int64_t)now,
                                                    &pair);
      if (created != LARDON3D_PROJECT_DB_OK) {
        free(candidates);
        return db_result(created);
      }
      ++stats->generated_count;
    } else {
      free(candidates);
      return db_result(found);
    }
  }
  free(candidates);
  return LARDON3D_VISUAL_INDEX_OK;
}

Lardon3DVisualIndexResult lardon3d_candidate_pair_generate_batch(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t after_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *total_stats,
    uint64_t *last_feature_set_id) {
  if (total_stats) {
    total_stats->generated_count = 0;
    total_stats->skipped_count = 0;
    total_stats->queried_count = 0;
  }
  if (last_feature_set_id) {
    *last_feature_set_id = after_feature_set_id;
  }
  if (!project_path || !database || visual_index_id == 0 || !query_options ||
      !total_stats || !last_feature_set_id) {
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  }
  enum { PAGE_SIZE = 64 };
  Lardon3DProjectDbFeatureSet page[PAGE_SIZE];
  size_t count = 0;
  uint64_t cursor = after_feature_set_id;
  Lardon3DProjectDbResult listed =
      lardon3d_project_db_list_feature_sets(database, cursor, page, PAGE_SIZE, &count);
  if (listed != LARDON3D_PROJECT_DB_OK) {
    return db_result(listed);
  }
  while (count > 0) {
    for (size_t i = 0; i < count; ++i) {
      Lardon3DCandidatePairGenStats stats;
      Lardon3DVisualIndexResult result = lardon3d_candidate_pair_generate(
          project_path, database, visual_index_id, page[i].feature_set_id,
          query_options, &stats);
      if (result != LARDON3D_VISUAL_INDEX_OK) {
        return result;
      }
      total_stats->generated_count += stats.generated_count;
      total_stats->skipped_count += stats.skipped_count;
      total_stats->queried_count += stats.queried_count;
      cursor = page[i].feature_set_id;
    }
    listed = lardon3d_project_db_list_feature_sets(database, cursor, page, PAGE_SIZE, &count);
    if (listed != LARDON3D_PROJECT_DB_OK) {
      return db_result(listed);
    }
  }
  *last_feature_set_id = cursor;
  return LARDON3D_VISUAL_INDEX_OK;
}

void lardon3d_candidate_pair_generation_fingerprint(
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    unsigned char fingerprint[32]) {
  SHA256_CTX sha;
  SHA256_Init(&sha);
  SHA256_Update(&sha, &visual_index_id, sizeof(visual_index_id));
  SHA256_Update(&sha, &source_feature_set_id, sizeof(source_feature_set_id));
  if (query_options) {
    SHA256_Update(&sha, &query_options->top_k, sizeof(query_options->top_k));
    SHA256_Update(&sha, &query_options->minimum_evidence_count,
                  sizeof(query_options->minimum_evidence_count));
    SHA256_Update(&sha, &query_options->scanset_filter, sizeof(query_options->scanset_filter));
    bool exclude = query_options->exclude_same_asset;
    SHA256_Update(&sha, &exclude, sizeof(exclude));
  }
  SHA256_Final(fingerprint, &sha);
}