#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include <lardon3d/candidate_pair_gen.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/project_db.h>
#include <lardon3d/visual_index.h>

#include "../src/candidate_pair_gen_internal.h"

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);                             \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

static bool join_path(char output[PATH_MAX], const char *left, const char *right) {
  int written = snprintf(output, PATH_MAX, "%s/%s", left, right);
  return written > 0 && (size_t)written < PATH_MAX;
}

static bool remove_tree(const char *path) {
  struct stat status;
  if (lstat(path, &status) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(status.st_mode)) {
    return unlink(path) == 0;
  }
  DIR *directory = opendir(path);
  if (!directory) {
    return false;
  }
  bool ok = true;
  for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[PATH_MAX];
    if (!join_path(child, path, entry->d_name) || !remove_tree(child)) {
      ok = false;
    }
  }
  return closedir(directory) == 0 && rmdir(path) == 0 && ok;
}

static bool write_source(const char *path, unsigned char value) {
  int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  unsigned char bytes[64];
  memset(bytes, value, sizeof(bytes));
  bool ok = write(descriptor, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
  return close(descriptor) == 0 && ok;
}

static bool publish_features(Lardon3DAppState *state, uint64_t image_id, unsigned int family,
                             Lardon3DProjectDbFeatureSet *set) {
  enum { COUNT = 16 };
  Lardon3DFeatureKeypoint keypoints[COUNT];
  unsigned char descriptors[COUNT * 32];
  for (uint32_t i = 0; i < COUNT; ++i) {
    keypoints[i] = (Lardon3DFeatureKeypoint){(float)i, (float)i, 8.0F, 0.0F,
                                             (float)(COUNT - i), 0};
    for (uint32_t byte = 0; byte < 32; ++byte) {
      descriptors[i * 32 + byte] =
          family == 0 || (family == 1 && i < 10)
              ? (unsigned char)(i * 17U + byte * 29U)
              : (unsigned char)(family * 73U + i * 31U + byte * 7U);
    }
  }
  Lardon3DExtractedFeatures features = {
      .image_width = 64,
      .image_height = 64,
      .feature_count = COUNT,
      .keypoints = keypoints,
      .descriptors = descriptors,
      .descriptor_bytes = sizeof(descriptors),
  };
  Lardon3DFeatureExtractorParameters parameters = {64, 4, 20};
  Lardon3DFeatureStoreResult result =
      lardon3d_feature_store_publish(state, image_id, 0, &parameters, &features, set);
  return result == LARDON3D_FEATURE_STORE_OK ||
         result == LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
}

static bool pair_exists(const Lardon3DProjectDbCandidatePair *pairs, size_t count,
                        uint64_t image_a, uint64_t image_b) {
  uint64_t canonical_a = image_a < image_b ? image_a : image_b;
  uint64_t canonical_b = image_a < image_b ? image_b : image_a;
  for (size_t i = 0; i < count; ++i) {
    if (pairs[i].image_id_a == canonical_a && pairs[i].image_id_b == canonical_b) {
      return true;
    }
  }
  return false;
}

static bool retain_sparse_memberships(const char *database_path, uint64_t index_id,
                                      uint64_t first_id, uint64_t last_id) {
  sqlite3 *database = NULL;
  if (sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    sqlite3_close(database);
    return false;
  }
  char sql[256];
  int n = snprintf(sql, sizeof(sql),
                   "DELETE FROM visual_index_memberships WHERE visual_index_id=%lu "
                   "AND feature_set_id NOT IN(%lu,%lu)",
                   (unsigned long)index_id, (unsigned long)first_id,
                   (unsigned long)last_id);
  bool ok = n > 0 && (size_t)n < sizeof(sql) &&
            sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK;
  sqlite3_close(database);
  return ok;
}

static bool run_test(void) {
  char root[] = "/tmp/lardon3d-candidate-pair-gen-XXXXXX";
  CHECK(mkdtemp(root));
  char internal[PATH_MAX];
  char database_path[PATH_MAX];
  char source_paths[6][PATH_MAX];
  CHECK(join_path(internal, root, ".lardon3d") && mkdir(internal, 0700) == 0 &&
        join_path(database_path, root, "project.db"));
  for (size_t i = 0; i < 6; ++i) {
    CHECK(snprintf(source_paths[i], sizeof(source_paths[i]), "%s/source-%zu.bin", root, i) > 0 &&
          write_source(source_paths[i], (unsigned char)(i + 1)));
  }
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = database;
  CHECK(snprintf(state.project_path, sizeof(state.project_path), "%s", root) > 0);
  Lardon3DProjectDbScanSet scan_a;
  Lardon3DProjectDbScanSet scan_b;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scan_a) &&
        lardon3d_image_catalog_create_scanset(&state, "B", &scan_b));
  Lardon3DProjectDbImage images[6];
  Lardon3DProjectDbImageAsset asset;
  const uint64_t scansets[6] = {scan_a.scanset_id, scan_a.scanset_id, scan_a.scanset_id,
                                scan_b.scanset_id, scan_b.scanset_id, scan_b.scanset_id};
  const unsigned int families[6] = {0, 0, 0, 1, 2, 0};
  for (size_t i = 0; i < 6; ++i) {
    CHECK(lardon3d_image_catalog_import_file(&state, scansets[i], source_paths[i], 0, &images[i],
                                             &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
  }
  Lardon3DProjectDbFeatureSet feature_sets[6];
  for (size_t i = 0; i < 6; ++i) {
    CHECK(publish_features(&state, images[i].image_id, families[i], &feature_sets[i]));
  }
  Lardon3DVisualIndexConfiguration configuration = {1, 16, 4096};
  uint64_t index_id = 0;
  CHECK(lardon3d_visual_index_create(database, &feature_sets[0], &configuration, &index_id) ==
            LARDON3D_VISUAL_INDEX_OK &&
        index_id > 0);
  uint64_t last = 0;
  size_t indexed = 0;
  CHECK(lardon3d_visual_index_update_once(root, database, index_id, 0, 0, 16, &last, &indexed) ==
            LARDON3D_VISUAL_INDEX_OK &&
        indexed == 6);

  Lardon3DVisualIndexQueryOptions options = {
      .top_k = 2,
      .minimum_evidence_count = 1,
      .scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET,
  };
  Lardon3DCandidatePairGenStats stats;
  Lardon3DProjectDbCandidatePair pairs[LARDON3D_PROJECT_DB_CANDIDATE_PAIR_PAGE_MAX];
  size_t pair_count = 0;

  /* Top-K borné : seuls les 2 meilleurs candidats sont persistés. */
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[0].feature_set_id, &options, &stats) ==
            LARDON3D_VISUAL_INDEX_OK &&
        stats.queried_count == 2 && stats.generated_count == 2 && stats.skipped_count == 0);
  CHECK(lardon3d_project_db_list_candidate_pairs(database, 0, pairs, 256, &pair_count) ==
            LARDON3D_PROJECT_DB_OK &&
        pair_count == 2);
  CHECK(pair_exists(pairs, pair_count, images[0].image_id, images[1].image_id) &&
        pair_exists(pairs, pair_count, images[0].image_id, images[2].image_id) &&
        !pair_exists(pairs, pair_count, images[0].image_id, images[5].image_id));

  /* Moins de K candidats : 4 candidats retournés, 2 nouveaux, 2 déjà présents. */
  options.top_k = 10;
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[0].feature_set_id, &options, &stats) ==
            LARDON3D_VISUAL_INDEX_OK &&
        stats.queried_count == 4 && stats.generated_count == 2 && stats.skipped_count == 2);
  CHECK(lardon3d_project_db_list_candidate_pairs(database, 0, pairs, 256, &pair_count) ==
            LARDON3D_PROJECT_DB_OK &&
        pair_count == 4);
  CHECK(pair_exists(pairs, pair_count, images[0].image_id, images[5].image_id) &&
        pair_exists(pairs, pair_count, images[0].image_id, images[3].image_id));

  /* Canonicalisation A,B / B,A : génération depuis l'autre extrémité. */
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[1].feature_set_id, &options, &stats) ==
            LARDON3D_VISUAL_INDEX_OK &&
        stats.queried_count == 4 && stats.generated_count == 3 && stats.skipped_count == 1);
  Lardon3DProjectDbCandidatePair found;
  CHECK(lardon3d_project_db_find_candidate_pair(database, images[1].image_id, images[0].image_id,
                                                &found) == LARDON3D_PROJECT_DB_OK &&
        found.image_id_a == images[0].image_id && found.image_id_b == images[1].image_id);

  /* Génération répétée idempotente : aucune nouvelle paire. */
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[0].feature_set_id, &options, &stats) ==
            LARDON3D_VISUAL_INDEX_OK &&
        stats.queried_count == 4 && stats.generated_count == 0 && stats.skipped_count == 4);

  /* Absence de doublons et exclusion self-pair. */
  CHECK(lardon3d_project_db_list_candidate_pairs(database, 0, pairs, 256, &pair_count) ==
            LARDON3D_PROJECT_DB_OK &&
        pair_count == 7);
  for (size_t i = 0; i < pair_count; ++i) {
    CHECK(pairs[i].image_id_a < pairs[i].image_id_b);
    for (size_t j = i + 1; j < pair_count; ++j) {
      CHECK(pairs[i].image_id_a != pairs[j].image_id_a ||
            pairs[i].image_id_b != pairs[j].image_id_b);
    }
  }

  /* Plusieurs images partageant des candidats. */
  CHECK(pair_exists(pairs, pair_count, images[1].image_id, images[2].image_id) &&
        pair_exists(pairs, pair_count, images[1].image_id, images[5].image_id) &&
        pair_exists(pairs, pair_count, images[1].image_id, images[3].image_id));

  /* Image sans candidat : famille 2 sans correspondance. */
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[4].feature_set_id, &options, &stats) ==
            LARDON3D_VISUAL_INDEX_OK &&
        stats.queried_count == 0 && stats.generated_count == 0 && stats.skipped_count == 0);

  /* Index vide : aucun segment, aucun candidat. */
  Lardon3DVisualIndexConfiguration empty_configuration = {1, 15, 4096};
  uint64_t empty_index = 0;
  CHECK(lardon3d_visual_index_create(database, &feature_sets[0], &empty_configuration,
                                     &empty_index) == LARDON3D_VISUAL_INDEX_OK &&
        empty_index != index_id);
  Lardon3DVisualIndexResult empty_result = lardon3d_candidate_pair_generate(
      root, database, empty_index, feature_sets[0].feature_set_id, &options, &stats);
  CHECK(empty_result == LARDON3D_VISUAL_INDEX_OK);
  CHECK(stats.queried_count == 0 && stats.generated_count == 0 && stats.skipped_count == 0);

  /* Ordre et scores déterministes du Visual Index. */
  options.top_k = 4;
  Lardon3DVisualIndexCandidate candidates[LARDON3D_VISUAL_INDEX_TOP_K_MAX];
  size_t count_a = 0;
  size_t count_b = 0;
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_sets[0].feature_set_id,
                                    &options, candidates, 4, &count_a) ==
            LARDON3D_VISUAL_INDEX_OK &&
        count_a == 4);
  Lardon3DVisualIndexCandidate first[4];
  memcpy(first, candidates, count_a * sizeof(*first));
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_sets[0].feature_set_id,
                                    &options, candidates, 4, &count_b) ==
            LARDON3D_VISUAL_INDEX_OK &&
        count_b == count_a);
  for (size_t i = 0; i < count_a; ++i) {
    CHECK(candidates[i].feature_set_id == first[i].feature_set_id &&
          candidates[i].image_id == first[i].image_id &&
          candidates[i].score == first[i].score &&
          candidates[i].evidence_count == first[i].evidence_count);
  }

  /* Fingerprint déterministe. */
  unsigned char fp1[32], fp2[32];
  lardon3d_candidate_pair_generation_fingerprint(index_id, feature_sets[0].feature_set_id,
                                                 &options, fp1);
  lardon3d_candidate_pair_generation_fingerprint(index_id, feature_sets[0].feature_set_id,
                                                 &options, fp2);
  CHECK(memcmp(fp1, fp2, 32) == 0);
  static const unsigned char canonical_fingerprint[32] = {
      0x60, 0x61, 0x4b, 0x22, 0xa6, 0x2f, 0xe5, 0x10,
      0x8a, 0x63, 0x07, 0xb1, 0xa1, 0x69, 0x11, 0x29,
      0xd2, 0xee, 0xc4, 0xa4, 0x0e, 0x4f, 0x6c, 0x50,
      0x83, 0x78, 0xc0, 0xfa, 0x23, 0x6d, 0x7b, 0x09,
  };
  Lardon3DVisualIndexQueryOptions canonical_options = {
      .top_k = 4,
      .minimum_evidence_count = 3,
      .scanset_filter = LARDON3D_VISUAL_INDEX_OTHER_SCANSETS,
      .exclude_same_asset = true,
  };
  lardon3d_candidate_pair_generation_fingerprint(
      1, 2, &canonical_options, fp2);
  /* This vector equals the acquired x86 v1 output and now also proves that
   * native integer/enum/bool representation cannot enter scientific identity. */
  CHECK(memcmp(fp2, canonical_fingerprint, sizeof(fp2)) == 0);
  options.top_k = 8;
  lardon3d_candidate_pair_generation_fingerprint(index_id, feature_sets[0].feature_set_id,
                                                 &options, fp2);
  CHECK(memcmp(fp1, fp2, 32) != 0);
  options.top_k = 4;

  /* Erreurs d'arguments. */
  CHECK(lardon3d_candidate_pair_generate(NULL, database, index_id,
                                         feature_sets[0].feature_set_id, &options, &stats) ==
        LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  CHECK(lardon3d_candidate_pair_generate(root, NULL, index_id, feature_sets[0].feature_set_id,
                                         &options, &stats) == LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  CHECK(lardon3d_candidate_pair_generate(root, database, 0, feature_sets[0].feature_set_id,
                                         &options, &stats) == LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id, 0, &options, &stats) ==
        LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id, feature_sets[0].feature_set_id,
                                         NULL, &stats) == LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id, feature_sets[0].feature_set_id,
                                         &options, NULL) == LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  options.top_k = 0;
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[0].feature_set_id, &options, &stats) ==
        LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  options.top_k = LARDON3D_VISUAL_INDEX_TOP_K_MAX + 1;
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id,
                                         feature_sets[0].feature_set_id, &options, &stats) ==
        LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  options.top_k = 4;
  CHECK(lardon3d_candidate_pair_generate(root, database, index_id, 999999, &options, &stats) ==
        LARDON3D_VISUAL_INDEX_NOT_FOUND);

  /* A staged duplicate retains the established find-before-create semantics:
   * the first proposal creates and the second is accounted as skipped. */
  uint64_t duplicate_a = images[0].image_id < images[4].image_id
                             ? images[0].image_id : images[4].image_id;
  uint64_t duplicate_b = images[0].image_id < images[4].image_id
                             ? images[4].image_id : images[0].image_id;
  Lardon3DCandidatePairComputation duplicate = {
      .source_feature_set_id = feature_sets[0].feature_set_id,
      .queried_count = 2,
      .proposal_count = 2,
      .proposals = {{duplicate_a, duplicate_b}, {duplicate_a, duplicate_b}},
  };
  CHECK(lardon3d_candidate_pair_publish(database, &duplicate, &stats) ==
            LARDON3D_VISUAL_INDEX_OK &&
        stats.queried_count == 2 && stats.generated_count == 1 &&
        stats.skipped_count == 1);

  /* Génération batch : tous les FeatureSets. */
  Lardon3DCandidatePairGenStats batch_stats;
  uint64_t last_feature_set_id = 0;
  options.top_k = 4;
  CHECK(lardon3d_candidate_pair_generate_batch(root, database, index_id, 0, &options,
                                                &batch_stats, &last_feature_set_id) ==
            LARDON3D_VISUAL_INDEX_OK &&
        batch_stats.queried_count > 0 && batch_stats.generated_count > 0);
  CHECK(lardon3d_project_db_list_candidate_pairs(database, 0, pairs, 256, &pair_count) ==
            LARDON3D_PROJECT_DB_OK &&
        pair_count >= 7);
  size_t batch_pair_count = pair_count;

  /* Persistance puis réouverture de la base. */
  lardon3d_project_db_close(database);
  database = NULL;
  state.project_db = NULL;
  CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
  state.project_db = database;
  CHECK(lardon3d_project_db_list_candidate_pairs(database, 0, pairs, 256, &pair_count) ==
            LARDON3D_PROJECT_DB_OK &&
        pair_count == batch_pair_count);
  CHECK(lardon3d_project_db_find_candidate_pair(database, images[1].image_id, images[0].image_id,
                                                 &found) == LARDON3D_PROJECT_DB_OK &&
        found.image_id_a == images[0].image_id && found.image_id_b == images[1].image_id);
  lardon3d_project_db_close(database);

  /* Membership progress is a row rank, not arithmetic on sparse IDs. This
   * fixture intentionally retains only the first and last indexed source. */
  CHECK(retain_sparse_memberships(database_path, index_id,
                                  feature_sets[0].feature_set_id,
                                  feature_sets[5].feature_set_id) &&
        lardon3d_project_db_open(database_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK);
  uint64_t membership_ids[4] = {0};
  size_t membership_count = 0;
  uint64_t completed = 0, total = 0;
  CHECK(lardon3d_project_db_list_visual_index_memberships(
            database, index_id, 0, membership_ids, 4, &membership_count) ==
            LARDON3D_PROJECT_DB_OK &&
        membership_count == 2 &&
        membership_ids[0] == feature_sets[0].feature_set_id &&
        membership_ids[1] == feature_sets[5].feature_set_id);
  CHECK(lardon3d_project_db_visual_index_membership_progress(
            database, index_id, feature_sets[0].feature_set_id, &completed, &total) ==
            LARDON3D_PROJECT_DB_OK &&
        completed == 1 && total == 2);
  CHECK(lardon3d_project_db_visual_index_membership_progress(
            database, index_id, feature_sets[5].feature_set_id, &completed, &total) ==
            LARDON3D_PROJECT_DB_OK &&
        completed == 2 && total == 2);
  lardon3d_project_db_close(database);
  CHECK(remove_tree(root));
  return true;
}

int main(void) { return run_test() ? 0 : 1; }
