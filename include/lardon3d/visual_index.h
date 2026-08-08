#ifndef LARDON3D_VISUAL_INDEX_H
#define LARDON3D_VISUAL_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/project_db.h>

#define LARDON3D_VISUAL_INDEX_KIND "orb-lsh"

enum {
  LARDON3D_VISUAL_INDEX_VERSION = 1,
  LARDON3D_VISUAL_INDEX_TABLE_COUNT = 6,
  LARDON3D_VISUAL_INDEX_KEY_BITS = 24,
  LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX = 16,
  LARDON3D_VISUAL_INDEX_SEGMENT_MAX = 256,
  LARDON3D_VISUAL_INDEX_TOP_K_MAX = 256,
  LARDON3D_VISUAL_INDEX_CANDIDATE_MAX = 4096,
  LARDON3D_VISUAL_INDEX_POSTING_READ_MAX = 256,
};

typedef enum {
  LARDON3D_VISUAL_INDEX_OK = 0,
  LARDON3D_VISUAL_INDEX_NO_CHANGE,
  LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT,
  LARDON3D_VISUAL_INDEX_NOT_FOUND,
  LARDON3D_VISUAL_INDEX_INCOMPATIBLE,
  LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION,
  LARDON3D_VISUAL_INDEX_CORRUPT,
  LARDON3D_VISUAL_INDEX_LIMIT,
  LARDON3D_VISUAL_INDEX_IO_ERROR,
  LARDON3D_VISUAL_INDEX_DB_BUSY,
  LARDON3D_VISUAL_INDEX_DB_ERROR,
  LARDON3D_VISUAL_INDEX_PUBLISHED_NOT_DURABLE
} Lardon3DVisualIndexResult;

typedef struct {
  uint32_t version;
  uint32_t max_features_per_set;
  uint32_t max_bucket_postings;
} Lardon3DVisualIndexConfiguration;

typedef enum {
  LARDON3D_VISUAL_INDEX_ANY_SCANSET = 0,
  LARDON3D_VISUAL_INDEX_SAME_SCANSET,
  LARDON3D_VISUAL_INDEX_OTHER_SCANSETS
} Lardon3DVisualIndexScanSetFilter;

typedef struct {
  uint32_t top_k;
  uint32_t minimum_evidence_count;
  Lardon3DVisualIndexScanSetFilter scanset_filter;
  bool exclude_same_asset;
} Lardon3DVisualIndexQueryOptions;

typedef struct {
  uint64_t feature_set_id;
  uint64_t image_id;
  uint64_t scanset_id;
  double score;
  uint32_t evidence_count;
  bool same_image_asset;
} Lardon3DVisualIndexCandidate;

bool lardon3d_visual_index_configuration_valid(
    const Lardon3DVisualIndexConfiguration *configuration);
void lardon3d_visual_index_configuration_fingerprint(
    const Lardon3DVisualIndexConfiguration *configuration, unsigned char fingerprint[32]);
Lardon3DVisualIndexResult lardon3d_visual_index_create(
    Lardon3DProjectDb *database, const Lardon3DProjectDbFeatureSet *prototype,
    const Lardon3DVisualIndexConfiguration *configuration, uint64_t *visual_index_id);
Lardon3DVisualIndexResult lardon3d_visual_index_update_once(
    const char *project_path, Lardon3DProjectDb *database, uint64_t visual_index_id,
    uint64_t producer_task_id, uint64_t after_feature_set_id, size_t maximum_feature_sets,
    uint64_t *last_feature_set_id, size_t *indexed_count);
#ifdef LARDON3D_VISUAL_INDEX_TESTING
typedef void (*Lardon3DVisualIndexAfterSelectHook)(void *userdata);
Lardon3DVisualIndexResult lardon3d_visual_index_test_update_once(
    const char *project_path, Lardon3DProjectDb *database, uint64_t visual_index_id,
    uint64_t producer_task_id, uint64_t after_feature_set_id, size_t maximum_feature_sets,
    uint64_t *last_feature_set_id, size_t *indexed_count,
    Lardon3DVisualIndexAfterSelectHook after_select, void *hook_userdata);
#endif
Lardon3DVisualIndexResult lardon3d_visual_index_query(
    const char *project_path, Lardon3DProjectDb *database, uint64_t visual_index_id,
    uint64_t query_feature_set_id, const Lardon3DVisualIndexQueryOptions *options,
    Lardon3DVisualIndexCandidate *results, size_t capacity, size_t *result_count);

#endif
