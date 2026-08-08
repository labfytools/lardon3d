#include <float.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/precision_features.h>

enum { SUPPORT_BUCKETS = 16384 };

Lardon3DFeatureConsolidationParameters lardon3d_feature_consolidation_v1(void) {
  return (Lardon3DFeatureConsolidationParameters){.association_radius_pixels = 4.0};
}

bool lardon3d_feature_consolidation_parameters_valid(
    const Lardon3DFeatureConsolidationParameters *p) {
  return p && isfinite(p->association_radius_pixels) && p->association_radius_pixels >= 0.5 &&
         p->association_radius_pixels <= 64.0;
}

void lardon3d_feature_consolidation_fingerprint(
    const Lardon3DFeatureConsolidationParameters *p, unsigned char fingerprint[32]) {
  if (!fingerprint) return;
  memset(fingerprint, 0, 32);
  if (!lardon3d_feature_consolidation_parameters_valid(p)) return;
  unsigned char encoded[20] = {'L', '3', 'D', 'S', 'U', 'P', 'P', '1', 1};
  uint64_t bits = 0;
  memcpy(&bits, &p->association_radius_pixels, sizeof(bits));
  for (unsigned i = 0; i < 8; ++i) encoded[12 + i] = (unsigned char)(bits >> (8U * i));
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  unsigned int length = 0;
  if (!context || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
      EVP_DigestUpdate(context, encoded, sizeof(encoded)) != 1 ||
      EVP_DigestFinal_ex(context, fingerprint, &length) != 1 || length != 32) {
    memset(fingerprint, 0, 32);
  }
  EVP_MD_CTX_free(context);
}

bool lardon3d_feature_support_higher_quality(
    const Lardon3DProjectDbFeatureSupportGroup *candidate,
    const Lardon3DProjectDbFeatureSupportGroup *reference) {
  if (!candidate || !reference) return false;
  if (candidate->support_count != reference->support_count)
    return candidate->support_count > reference->support_count;
  return candidate->feature_support_group_id < reference->feature_support_group_id;
}

static bool read_points(const char *project_path, const Lardon3DProjectDbFeatureSet *set,
                        Lardon3DFeatureKeypoint **points) {
  *points = NULL;
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  if (lardon3d_feature_reader_open(project_path, set, &reader, &metadata) !=
      LARDON3D_FEATURE_STORE_OK) {
    return false;
  }
  Lardon3DFeatureKeypoint *all = set->feature_count
                                    ? calloc(set->feature_count, sizeof(*all))
                                    : NULL;
  bool ok = set->feature_count == 0 || all != NULL;
  for (uint32_t start = 0; ok && start < set->feature_count;) {
    size_t count = set->feature_count - start;
    if (count > LARDON3D_FEATURE_READER_RANGE_MAX) count = LARDON3D_FEATURE_READER_RANGE_MAX;
    ok = lardon3d_feature_reader_keypoints(reader, start, all + start, count) ==
         LARDON3D_FEATURE_STORE_OK;
    start += (uint32_t)count;
  }
  lardon3d_feature_reader_close(reader);
  if (!ok) free(all);
  else *points = all;
  return ok;
}

static uint32_t bucket_for(int64_t x, int64_t y) {
  uint64_t a = (uint64_t)x * UINT64_C(0x9e3779b185ebca87);
  uint64_t b = (uint64_t)y * UINT64_C(0xc2b2ae3d27d4eb4f);
  return (uint32_t)((a ^ b) & (SUPPORT_BUCKETS - 1));
}

static bool associate_points(const Lardon3DFeatureKeypoint *orb, uint32_t orb_count,
                             const Lardon3DFeatureKeypoint *sift, uint32_t sift_count,
                             double radius, Lardon3DProjectDbFeatureSupportGroup *groups) {
  int32_t heads[SUPPORT_BUCKETS];
  for (size_t i = 0; i < SUPPORT_BUCKETS; ++i) heads[i] = -1;
  int32_t *next = orb_count ? malloc((size_t)orb_count * sizeof(*next)) : NULL;
  bool *used = orb_count ? calloc(orb_count, sizeof(*used)) : NULL;
  if (orb_count && (!next || !used)) {
    free(next);
    free(used);
    return false;
  }
  for (uint32_t i = 0; i < orb_count; ++i) {
    int64_t cx = (int64_t)floor(orb[i].x / radius);
    int64_t cy = (int64_t)floor(orb[i].y / radius);
    uint32_t bucket = bucket_for(cx, cy);
    next[i] = heads[bucket];
    heads[bucket] = (int32_t)i;
    groups[i] = (Lardon3DProjectDbFeatureSupportGroup){
        .x = orb[i].x, .y = orb[i].y, .support_count = 1, .first_feature_index = i};
  }
  uint32_t unmatched = orb_count;
  double radius_squared = radius * radius;
  for (uint32_t s = 0; s < sift_count; ++s) {
    int64_t cx = (int64_t)floor(sift[s].x / radius);
    int64_t cy = (int64_t)floor(sift[s].y / radius);
    int32_t best = -1;
    double best_distance = radius_squared;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
      for (int32_t candidate = heads[bucket_for(cx + dx, cy + dy)]; candidate >= 0;
           candidate = next[candidate]) {
        if (used[candidate]) continue;
        double px = orb[candidate].x - sift[s].x;
        double py = orb[candidate].y - sift[s].y;
        double distance = px * px + py * py;
        if (distance <= best_distance &&
            (distance < best_distance || best < 0 || candidate < best)) {
          best = candidate;
          best_distance = distance;
        }
      }
    }
    if (best >= 0) {
      used[best] = true;
      groups[best].x = sift[s].x;
      groups[best].y = sift[s].y;
      groups[best].support_count = 2;
      groups[best].distance_pixels = sqrt(best_distance);
      groups[best].has_second_feature = true;
      groups[best].second_feature_index = s;
    } else {
      groups[unmatched++] = (Lardon3DProjectDbFeatureSupportGroup){
          .x = sift[s].x, .y = sift[s].y, .support_count = 1,
          .first_member_from_second_set = true, .first_feature_index = s};
    }
  }
  free(used);
  free(next);
  return true;
}

Lardon3DProjectDbResult lardon3d_consolidate_orb_sift(
    Lardon3DAppState *state, uint64_t orb_id, uint64_t sift_id,
    const Lardon3DFeatureConsolidationParameters *p,
    Lardon3DProjectDbFeatureSupportSet *published) {
  if (!state || !state->project_loaded || !state->project_db || state->project_path[0] == '\0' ||
      !published || !lardon3d_feature_consolidation_parameters_valid(p)) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbFeatureSet orb, sift;
  if (lardon3d_project_db_load_feature_set(state->project_db, orb_id, &orb) !=
          LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_load_feature_set(state->project_db, sift_id, &sift) !=
          LARDON3D_PROJECT_DB_OK ||
      orb.image_id != sift.image_id || strcmp(orb.extractor_kind, "orb") != 0 ||
      (strcmp(sift.extractor_kind, "sift") != 0 && strcmp(sift.extractor_kind, "rootsift") != 0)) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  unsigned char fingerprint[32];
  lardon3d_feature_consolidation_fingerprint(p, fingerprint);
  Lardon3DProjectDbResult found = lardon3d_project_db_load_feature_support(
      state->project_db, orb.image_id, orb_id, sift_id, fingerprint, published);
  if (found == LARDON3D_PROJECT_DB_OK)
    return published->radius_pixels == p->association_radius_pixels
               ? LARDON3D_PROJECT_DB_OK
               : LARDON3D_PROJECT_DB_CORRUPT;
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND) return found;
  Lardon3DFeatureKeypoint *orb_points = NULL, *sift_points = NULL;
  if (!read_points(state->project_path, &orb, &orb_points) ||
      !read_points(state->project_path, &sift, &sift_points)) {
    free(orb_points);
    free(sift_points);
    return LARDON3D_PROJECT_DB_CORRUPT;
  }
  size_t count = (size_t)orb.feature_count + sift.feature_count;
  Lardon3DProjectDbFeatureSupportGroup *groups = count ? calloc(count, sizeof(*groups)) : NULL;
  if (count && !groups) {
    free(orb_points);
    free(sift_points);
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }
  if (!associate_points(orb_points, orb.feature_count, sift_points, sift.feature_count,
                        p->association_radius_pixels, groups)) {
    free(groups);
    free(orb_points);
    free(sift_points);
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }
  uint32_t matched = 0;
  for (uint32_t i = 0; i < orb.feature_count; ++i) matched += groups[i].support_count == 2;
  size_t group_count = count - matched;
  Lardon3DProjectDbFeatureSupportSet config = {
      .image_id = orb.image_id, .first_feature_set_id = orb_id, .second_feature_set_id = sift_id,
      .radius_pixels = p->association_radius_pixels, .created_at = (int64_t)time(NULL)};
  memcpy(config.parameter_fingerprint, fingerprint, 32);
  Lardon3DProjectDbResult result = lardon3d_project_db_publish_feature_support(
      state->project_db, &config, groups, group_count, published);
  free(groups);
  free(orb_points);
  free(sift_points);
  return result;
}
