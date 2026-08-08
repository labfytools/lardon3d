#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/visual_index.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);                             \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

typedef struct {
  const char *root;
  Lardon3DProjectDb *database;
  uint64_t index_id;
  uint64_t query_id;
  bool ok;
} QueryThread;

typedef struct {
  pthread_barrier_t barrier;
  const char *root;
  Lardon3DProjectDb *database;
  uint64_t index_id;
  Lardon3DVisualIndexResult result[2];
} UpdateRace;

typedef struct {
  UpdateRace *race;
  size_t slot;
} UpdateRaceThread;

static size_t candidate_rank(const Lardon3DVisualIndexCandidate *candidates, size_t count,
                             uint64_t feature_set_id) {
  for (size_t i = 0; i < count; ++i) {
    if (candidates[i].feature_set_id == feature_set_id) {
      return i;
    }
  }
  return SIZE_MAX;
}

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

static unsigned char scene_pixel(unsigned int x, unsigned int y, unsigned int seed) {
  uint32_t value = x * 2654435761U ^ y * 2246822519U ^ seed * 3266489917U;
  value ^= value >> 13;
  value *= 1274126177U;
  return (unsigned char)(value >> 24);
}

static bool write_scene(const char *path, unsigned int shift, unsigned int seed) {
  FILE *file = fopen(path, "wb");
  if (!file || fprintf(file, "P5\n256 256\n255\n") <= 0) {
    if (file) {
      fclose(file);
    }
    return false;
  }
  for (unsigned int y = 0; y < 256; ++y) {
    for (unsigned int x = 0; x < 256; ++x) {
      unsigned char value = x >= shift ? scene_pixel(x - shift, y, seed) : 0;
      if (fwrite(&value, 1, 1, file) != 1) {
        fclose(file);
        return false;
      }
    }
  }
  return fclose(file) == 0;
}

static bool write_transformed_scene(const char *path, unsigned int seed, bool crop,
                                    double angle_degrees) {
  unsigned int width = crop ? 192U : 256U;
  FILE *file = fopen(path, "wb");
  if (!file || fprintf(file, "P5\n%u %u\n255\n", width, width) <= 0) {
    if (file) {
      fclose(file);
    }
    return false;
  }
  double angle = angle_degrees * 3.14159265358979323846 / 180.0;
  double cosine = cos(angle);
  double sine = sin(angle);
  for (unsigned int y = 0; y < width; ++y) {
    for (unsigned int x = 0; x < width; ++x) {
      double output_x = crop ? (double)x + 32.0 : (double)x;
      double output_y = crop ? (double)y + 32.0 : (double)y;
      double dx = output_x - 127.5;
      double dy = output_y - 127.5;
      int source_x = (int)(127.5 + cosine * dx + sine * dy + 0.5);
      int source_y = (int)(127.5 - sine * dx + cosine * dy + 0.5);
      unsigned char value = source_x >= 0 && source_x < 256 && source_y >= 0 && source_y < 256
                                ? scene_pixel((unsigned int)source_x, (unsigned int)source_y, seed)
                                : 0;
      if (fwrite(&value, 1, 1, file) != 1) {
        fclose(file);
        return false;
      }
    }
  }
  return fclose(file) == 0;
}

static bool publish_real_features(Lardon3DAppState *state, uint64_t image_id,
                                  const char *source, Lardon3DProjectDbFeatureSet *set) {
  Lardon3DFeatureExtractorParameters parameters = {512, 4, 20};
  Lardon3DExtractedFeatures features;
  if (lardon3d_feature_extract_orb(source, &parameters, &features) !=
      LARDON3D_FEATURE_EXTRACT_OK) {
    return false;
  }
  Lardon3DFeatureStoreResult result =
      lardon3d_feature_store_publish(state, image_id, 0, &parameters, &features, set);
  lardon3d_extracted_features_destroy(&features);
  return result == LARDON3D_FEATURE_STORE_OK ||
         result == LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
}

static bool publish_product_features(Lardon3DAppState *state, uint64_t image_id,
                                     const char *source, Lardon3DProjectDbFeatureSet *set) {
  Lardon3DFeatureExtractorParameters parameters = {640, 4, 20};
  Lardon3DExtractedFeatures features;
  if (lardon3d_feature_extract_orb(source, &parameters, &features) !=
      LARDON3D_FEATURE_EXTRACT_OK) {
    return false;
  }
  Lardon3DFeatureStoreResult result =
      lardon3d_feature_store_publish(state, image_id, 0, &parameters, &features, set);
  lardon3d_extracted_features_destroy(&features);
  return result == LARDON3D_FEATURE_STORE_OK ||
         result == LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
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

static bool publish_empty_features(Lardon3DAppState *state, uint64_t image_id,
                                   Lardon3DProjectDbFeatureSet *set) {
  Lardon3DExtractedFeatures features = {.image_width = 64, .image_height = 64};
  Lardon3DFeatureExtractorParameters parameters = {64, 4, 20};
  Lardon3DFeatureStoreResult result =
      lardon3d_feature_store_publish(state, image_id, 0, &parameters, &features, set);
  return result == LARDON3D_FEATURE_STORE_OK ||
         result == LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
}

static bool publish_burst_features(Lardon3DAppState *state, uint64_t image_id, unsigned int kind,
                                   Lardon3DProjectDbFeatureSet *set) {
  enum { COUNT = 16 };
  Lardon3DFeatureKeypoint keypoints[COUNT];
  unsigned char descriptors[COUNT * 32];
  memset(descriptors, 0, sizeof(descriptors));
  for (uint32_t i = 0; i < COUNT; ++i) {
    keypoints[i] = (Lardon3DFeatureKeypoint){(float)i, (float)i, 8.0F, 0.0F,
                                             (float)(COUNT - i), 0};
    if ((kind == 0 && i >= 8) || (kind == 2 && i < 8)) {
      uint32_t detail = kind == 0 ? i - 8 : i;
      for (uint32_t byte = 0; byte < 32; ++byte) {
        descriptors[i * 32 + byte] = (unsigned char)(detail * 37U + byte * 19U + 1U);
      }
    } else if (kind == 2 && i >= 8) {
      memset(descriptors + i * 32, (int)(i + 31U), 32);
    }
  }
  Lardon3DExtractedFeatures features = {
      64, 64, COUNT, keypoints, descriptors, {0}, sizeof(descriptors)};
  Lardon3DFeatureExtractorParameters parameters = {63, 4, 20};
  Lardon3DFeatureStoreResult result =
      lardon3d_feature_store_publish(state, image_id, 0, &parameters, &features, set);
  return result == LARDON3D_FEATURE_STORE_OK ||
         result == LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
}

static void update_race_barrier(void *userdata) {
  UpdateRace *race = userdata;
  (void)pthread_barrier_wait(&race->barrier);
}

static void *update_race_thread(void *userdata) {
  UpdateRaceThread *thread = userdata;
  uint64_t last = 0;
  size_t indexed = 0;
  thread->race->result[thread->slot] = lardon3d_visual_index_test_update_once(
      thread->race->root, thread->race->database, thread->race->index_id, 0, 0, 16, &last,
      &indexed, update_race_barrier, thread->race);
  return NULL;
}

static bool sha256_bytes(const unsigned char *bytes, size_t size, unsigned char hash[32]) {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  unsigned int hash_size = 0;
  bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(context, bytes, size) == 1 &&
            EVP_DigestFinal_ex(context, hash, &hash_size) == 1 && hash_size == 32;
  EVP_MD_CTX_free(context);
  return ok;
}

static bool replace_segment_metadata(const char *database_path, uint64_t segment_id,
                                     const unsigned char hash[32], const char *path,
                                     uint64_t size) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  bool ok = sqlite3_open(database_path, &connection) == SQLITE_OK &&
            sqlite3_prepare_v2(connection,
                               "UPDATE visual_index_segments SET sha256=?1,path=?2,size_bytes=?3 "
                               "WHERE visual_index_segment_id=?4", -1, &statement, NULL) ==
                SQLITE_OK;
  if (ok) {
    sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)size);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)segment_id);
    ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(connection) == 1;
  }
  sqlite3_finalize(statement);
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool publish_mutated_segment(const char *root, const char *database_path,
                                    const Lardon3DProjectDbVisualIndexSegment *segment,
                                    const unsigned char *bytes, size_t size, char path[PATH_MAX],
                                    unsigned char hash[32]) {
  char hex[65];
  if (!sha256_bytes(bytes, size, hash)) {
    return false;
  }
  for (size_t i = 0; i < 32; ++i) {
    snprintf(hex + i * 2, 3, "%02x", hash[i]);
  }
  char directory[PATH_MAX];
  if (snprintf(path, PATH_MAX, "assets/visual-index/%.2s/%s", hex, hex) <= 0 ||
      snprintf(directory, sizeof(directory), "%s/assets/visual-index/%.2s", root, hex) <= 0) {
    return false;
  }
  if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
    return false;
  }
  char absolute[PATH_MAX];
  if (!join_path(absolute, root, path)) {
    return false;
  }
  int descriptor = open(absolute, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  bool ok = write(descriptor, bytes, size) == (ssize_t)size && close(descriptor) == 0;
  return ok && replace_segment_metadata(database_path, segment->visual_index_segment_id, hash,
                                        path, size);
}

static void *query_thread(void *userdata) {
  QueryThread *context = userdata;
  Lardon3DVisualIndexQueryOptions options = {
      .top_k = 4,
      .minimum_evidence_count = 1,
      .scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET,
  };
  Lardon3DVisualIndexCandidate candidates[LARDON3D_VISUAL_INDEX_TOP_K_MAX];
  size_t count = 0;
  context->ok = lardon3d_visual_index_query(context->root, context->database, context->index_id,
                                             context->query_id, &options, candidates, 4, &count) ==
                    LARDON3D_VISUAL_INDEX_OK &&
                count > 0;
  return NULL;
}

static bool insert_scale_fixture(const char *database_path, uint64_t image_asset_id,
                                 uint64_t feature_asset_id,
                                 const Lardon3DProjectDbFeatureSet *prototype, uint64_t first,
                                 uint64_t count) {
  sqlite3 *connection = NULL;
  if (sqlite3_open(database_path, &connection) != SQLITE_OK ||
      sqlite3_exec(connection, "PRAGMA foreign_keys=ON;BEGIN IMMEDIATE", NULL, NULL, NULL) !=
          SQLITE_OK) {
    sqlite3_close(connection);
    return false;
  }
  sqlite3_stmt *scan = NULL;
  sqlite3_stmt *image = NULL;
  sqlite3_stmt *feature = NULL;
  bool ok = sqlite3_prepare_v2(connection,
                               "INSERT INTO scansets(scanset_id,name,created_at,updated_at) "
                               "VALUES(?1,'scale',0,0)", -1, &scan, NULL) == SQLITE_OK &&
            sqlite3_prepare_v2(connection,
                               "INSERT INTO images(image_id,scanset_id,asset_id,original_name,"
                               "source_path,imported_at) VALUES(?1,?2,?3,'scale','scale',0)", -1,
                               &image, NULL) == SQLITE_OK &&
            sqlite3_prepare_v2(connection,
                               "INSERT INTO feature_sets(feature_set_id,image_id,feature_asset_id,"
                               "extractor_kind,extractor_version,parameter_fingerprint,"
                               "source_image_sha256,feature_count,descriptor_type,"
                               "descriptor_dimension,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,"
                               "?8,?9,?10,0)", -1, &feature, NULL) == SQLITE_OK;
  for (uint64_t i = 0; i < count && ok; ++i) {
    sqlite3_int64 id = (sqlite3_int64)(first + i);
    sqlite3_bind_int64(scan, 1, id);
    ok = sqlite3_step(scan) == SQLITE_DONE;
    sqlite3_reset(scan);
    sqlite3_clear_bindings(scan);
    sqlite3_bind_int64(image, 1, id);
    sqlite3_bind_int64(image, 2, id);
    sqlite3_bind_int64(image, 3, (sqlite3_int64)image_asset_id);
    ok = ok && sqlite3_step(image) == SQLITE_DONE;
    sqlite3_reset(image);
    sqlite3_clear_bindings(image);
    sqlite3_bind_int64(feature, 1, id);
    sqlite3_bind_int64(feature, 2, id);
    sqlite3_bind_int64(feature, 3, (sqlite3_int64)feature_asset_id);
    sqlite3_bind_text(feature, 4, prototype->extractor_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(feature, 5, prototype->extractor_version);
    sqlite3_bind_blob(feature, 6, prototype->parameter_fingerprint, 32, SQLITE_TRANSIENT);
    sqlite3_bind_blob(feature, 7, prototype->source_image_sha256, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int64(feature, 8, prototype->feature_count);
    sqlite3_bind_int64(feature, 9, prototype->descriptor_type);
    sqlite3_bind_int64(feature, 10, prototype->descriptor_dimension);
    ok = ok && sqlite3_step(feature) == SQLITE_DONE;
    sqlite3_reset(feature);
    sqlite3_clear_bindings(feature);
  }
  sqlite3_finalize(scan);
  sqlite3_finalize(image);
  sqlite3_finalize(feature);
  ok = ok && sqlite3_exec(connection, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
  if (!ok) {
    sqlite3_exec(connection, "ROLLBACK", NULL, NULL, NULL);
  }
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool run_test(void) {
  char root[] = "/tmp/lardon3d-visual-index-XXXXXX";
  CHECK(mkdtemp(root));
  char internal[PATH_MAX];
  char database_path[PATH_MAX];
  char source_a[PATH_MAX];
  char source_b[PATH_MAX];
  char source_c[PATH_MAX];
  char scene_a[PATH_MAX];
  char scene_b[PATH_MAX];
  char scene_c[PATH_MAX];
  char product_paths[6][PATH_MAX];
  CHECK(join_path(internal, root, ".lardon3d") && mkdir(internal, 0700) == 0 &&
        join_path(database_path, root, "project.db") && join_path(source_a, root, "a.bin") &&
        join_path(source_b, root, "b.bin") && join_path(source_c, root, "c.bin") &&
        join_path(scene_a, root, "scene-a.pgm") && join_path(scene_b, root, "scene-b.pgm") &&
        join_path(scene_c, root, "scene-c.pgm") &&
        join_path(product_paths[0], root, "product-a1.pgm") &&
        join_path(product_paths[1], root, "product-a2.pgm") &&
        join_path(product_paths[2], root, "product-a3.pgm") &&
        join_path(product_paths[3], root, "product-b1-crop.pgm") &&
        join_path(product_paths[4], root, "product-b2-rotation.pgm") &&
        join_path(product_paths[5], root, "product-b3.pgm") && write_source(source_a, 1) &&
        write_source(source_b, 2) && write_source(source_c, 3) && write_scene(scene_a, 0, 11) &&
        write_scene(scene_b, 5, 11) && write_scene(scene_c, 0, 97) &&
        write_scene(product_paths[0], 0, 21) && write_scene(product_paths[1], 0, 43) &&
        write_scene(product_paths[2], 0, 89) &&
        write_transformed_scene(product_paths[3], 21, true, 0.0) &&
        write_transformed_scene(product_paths[4], 43, false, 8.0) &&
        write_scene(product_paths[5], 0, 127));
  Lardon3DProjectDb *database = NULL;
  char error[256];
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
  Lardon3DProjectDbImage image_a;
  Lardon3DProjectDbImage image_b;
  Lardon3DProjectDbImage image_c;
  Lardon3DProjectDbImage image_a_copy;
  Lardon3DProjectDbImage empty_image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scan_a.scanset_id, source_a, 0, &image_a,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED &&
        lardon3d_image_catalog_import_file(&state, scan_b.scanset_id, source_b, 0, &image_b,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED &&
        lardon3d_image_catalog_import_file(&state, scan_b.scanset_id, source_c, 0, &image_c,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED &&
        lardon3d_image_catalog_import_file(&state, scan_b.scanset_id, source_a, 0, &image_a_copy,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED &&
        image_a_copy.asset_id == image_a.asset_id &&
        lardon3d_image_catalog_import_file(&state, scan_a.scanset_id, source_c, 0, &empty_image,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
  Lardon3DProjectDbFeatureSet feature_a;
  Lardon3DProjectDbFeatureSet feature_b;
  Lardon3DProjectDbFeatureSet feature_c;
  Lardon3DProjectDbFeatureSet feature_a_copy;
  Lardon3DProjectDbFeatureSet empty_feature;
  CHECK(publish_features(&state, image_a.image_id, 0, &feature_a) &&
        publish_features(&state, image_b.image_id, 1, &feature_b) &&
        publish_features(&state, image_c.image_id, 2, &feature_c) &&
        publish_features(&state, image_a_copy.image_id, 0, &feature_a_copy) &&
        publish_empty_features(&state, empty_image.image_id, &empty_feature));
  Lardon3DProjectDbFeatureSet burst_query;
  Lardon3DProjectDbFeatureSet burst_pattern;
  Lardon3DProjectDbFeatureSet burst_related;
  CHECK(publish_burst_features(&state, image_a.image_id, 0, &burst_query) &&
        publish_burst_features(&state, image_b.image_id, 1, &burst_pattern) &&
        publish_burst_features(&state, image_c.image_id, 2, &burst_related));
  Lardon3DVisualIndexConfiguration burst_configuration = {1, 16, 8};
  uint64_t burst_index = 0;
  uint64_t last = 0;
  size_t indexed = 0;
  CHECK(lardon3d_visual_index_create(database, &burst_query, &burst_configuration, &burst_index) ==
            LARDON3D_VISUAL_INDEX_OK &&
        lardon3d_visual_index_update_once(root, database, burst_index, 0, 0, 16, &last,
                                          &indexed) == LARDON3D_VISUAL_INDEX_OK &&
        indexed == 3);
  Lardon3DVisualIndexCandidate candidates[LARDON3D_VISUAL_INDEX_TOP_K_MAX];
  size_t count = 0;
  Lardon3DVisualIndexQueryOptions options = {
      .top_k = 4,
      .minimum_evidence_count = 1,
      .scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET,
  };
  CHECK(lardon3d_visual_index_query(root, database, burst_index, burst_query.feature_set_id,
                                    &options, candidates, 4, &count) ==
            LARDON3D_VISUAL_INDEX_OK &&
        candidate_rank(candidates, count, burst_related.feature_set_id) != SIZE_MAX &&
        candidate_rank(candidates, count, burst_pattern.feature_set_id) == SIZE_MAX);
  Lardon3DVisualIndexConfiguration configuration = {1, 16, 4096};
  uint64_t index_id = 0;
  CHECK(lardon3d_visual_index_create(database, &feature_a, &configuration, &index_id) ==
            LARDON3D_VISUAL_INDEX_OK &&
        index_id > 0);
  CHECK(lardon3d_visual_index_update_once(root, database, index_id, 0, 0, 16, &last, &indexed) ==
            LARDON3D_VISUAL_INDEX_OK &&
        indexed == 5 && last == empty_feature.feature_set_id);
  CHECK(lardon3d_visual_index_update_once(root, database, index_id, 0, last, 16, &last, &indexed) ==
            LARDON3D_VISUAL_INDEX_NO_CHANGE &&
        indexed == 0);
  Lardon3DProjectDbFeatureSet no_pending[16];
  size_t no_pending_count = 0;
  CHECK(lardon3d_project_db_list_visual_index_pending(database, index_id, 0, no_pending, 16,
                                                       &no_pending_count) ==
            LARDON3D_PROJECT_DB_OK &&
        no_pending_count == 0);
  options = (Lardon3DVisualIndexQueryOptions){
      .top_k = 4,
      .minimum_evidence_count = 2,
      .scanset_filter = LARDON3D_VISUAL_INDEX_OTHER_SCANSETS,
  };
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_OK &&
        count >= 1);
  bool found_related = false;
  for (size_t i = 0; i < count; ++i) {
    found_related = found_related ||
                    (candidates[i].feature_set_id == feature_b.feature_set_id &&
                     candidates[i].image_id == image_b.image_id &&
                     candidates[i].evidence_count >= 10 && candidates[i].scanset_id ==
                                                                scan_b.scanset_id);
  }
  CHECK(found_related);
  bool found_same_asset = false;
  for (size_t i = 0; i < count; ++i) {
    if (candidates[i].feature_set_id == feature_a_copy.feature_set_id) {
      found_same_asset = candidates[i].same_image_asset && candidates[i].evidence_count == 16 &&
                         candidates[i].score == 1.0;
    }
  }
  CHECK(found_same_asset);
  options.exclude_same_asset = true;
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_OK);
  for (size_t i = 0; i < count; ++i) {
    CHECK(candidates[i].feature_set_id != feature_a_copy.feature_set_id);
  }
  options.exclude_same_asset = false;
  options.scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET;
  CHECK(lardon3d_visual_index_query(root, database, index_id, empty_feature.feature_set_id,
                                    &options, candidates, 4, &count) ==
            LARDON3D_VISUAL_INDEX_OK &&
        count == 0);
  options.scanset_filter = LARDON3D_VISUAL_INDEX_SAME_SCANSET;
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_OK &&
        count == 0);

  Lardon3DProjectDbImage real_a;
  Lardon3DProjectDbImage real_b;
  Lardon3DProjectDbImage real_c;
  CHECK(lardon3d_image_catalog_import_file(&state, scan_a.scanset_id, scene_a, 0, &real_a,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED &&
        lardon3d_image_catalog_import_file(&state, scan_b.scanset_id, scene_b, 0, &real_b,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED &&
        lardon3d_image_catalog_import_file(&state, scan_b.scanset_id, scene_c, 0, &real_c,
                                           &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
  Lardon3DProjectDbFeatureSet real_feature_a;
  Lardon3DProjectDbFeatureSet real_feature_b;
  Lardon3DProjectDbFeatureSet real_feature_c;
  CHECK(publish_real_features(&state, real_a.image_id, scene_a, &real_feature_a) &&
        publish_real_features(&state, real_b.image_id, scene_b, &real_feature_b) &&
        publish_real_features(&state, real_c.image_id, scene_c, &real_feature_c));
  uint64_t real_index = 0;
  Lardon3DVisualIndexConfiguration real_configuration = {1, 512, 256};
  CHECK(lardon3d_visual_index_create(database, &real_feature_a, &real_configuration, &real_index) ==
            LARDON3D_VISUAL_INDEX_OK &&
        setenv("LARDON3D_TEST_VISUAL_INDEX_FAIL_DIR_FSYNC", "1", 1) == 0 &&
        lardon3d_visual_index_update_once(root, database, real_index, 0, 0, 16, &last, &indexed) ==
            LARDON3D_VISUAL_INDEX_PUBLISHED_NOT_DURABLE &&
        indexed == 3 && unsetenv("LARDON3D_TEST_VISUAL_INDEX_FAIL_DIR_FSYNC") == 0);
  options = (Lardon3DVisualIndexQueryOptions){8, 2, LARDON3D_VISUAL_INDEX_OTHER_SCANSETS, false};
  CHECK(lardon3d_visual_index_query(root, database, real_index, real_feature_a.feature_set_id,
                                    &options, candidates, 8, &count) ==
            LARDON3D_VISUAL_INDEX_OK &&
        count > 0 && candidates[0].feature_set_id == real_feature_b.feature_set_id);
  Lardon3DProjectDbImage product_images[6];
  Lardon3DProjectDbFeatureSet product_features[6];
  for (size_t i = 0; i < 6; ++i) {
    uint64_t scanset = i < 3 ? scan_a.scanset_id : scan_b.scanset_id;
    CHECK(lardon3d_image_catalog_import_file(&state, scanset, product_paths[i], 0,
                                             &product_images[i], &asset) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED &&
          publish_product_features(&state, product_images[i].image_id, product_paths[i],
                                   &product_features[i]));
  }
  Lardon3DVisualIndexConfiguration product_configuration = {1, 640, 256};
  uint64_t product_index = 0;
  CHECK(lardon3d_visual_index_create(database, &product_features[0], &product_configuration,
                                     &product_index) == LARDON3D_VISUAL_INDEX_OK &&
        lardon3d_visual_index_update_once(root, database, product_index, 0, 0, 16, &last,
                                          &indexed) == LARDON3D_VISUAL_INDEX_OK &&
        indexed == 6);
  options = (Lardon3DVisualIndexQueryOptions){6, 1, LARDON3D_VISUAL_INDEX_ANY_SCANSET, false};
  CHECK(lardon3d_visual_index_query(root, database, product_index,
                                    product_features[0].feature_set_id, &options, candidates, 6,
                                    &count) == LARDON3D_VISUAL_INDEX_OK);
  size_t crop_rank = candidate_rank(candidates, count, product_features[3].feature_set_id);
  size_t unrelated_rank = candidate_rank(candidates, count, product_features[5].feature_set_id);
  CHECK(crop_rank != SIZE_MAX && (unrelated_rank == SIZE_MAX || crop_rank < unrelated_rank));
  size_t low_evidence_count = count;
  options.minimum_evidence_count = 8;
  CHECK(lardon3d_visual_index_query(root, database, product_index,
                                    product_features[0].feature_set_id, &options, candidates, 6,
                                    &count) == LARDON3D_VISUAL_INDEX_OK &&
        count <= low_evidence_count);
  options = (Lardon3DVisualIndexQueryOptions){6, 1,
                                              LARDON3D_VISUAL_INDEX_OTHER_SCANSETS, false};
  CHECK(lardon3d_visual_index_query(root, database, product_index,
                                    product_features[0].feature_set_id, &options, candidates, 6,
                                    &count) == LARDON3D_VISUAL_INDEX_OK);
  for (size_t i = 0; i < count; ++i) {
    CHECK(candidates[i].scanset_id == scan_b.scanset_id);
  }
  options.scanset_filter = LARDON3D_VISUAL_INDEX_SAME_SCANSET;
  CHECK(lardon3d_visual_index_query(root, database, product_index,
                                    product_features[0].feature_set_id, &options, candidates, 6,
                                    &count) == LARDON3D_VISUAL_INDEX_OK);
  for (size_t i = 0; i < count; ++i) {
    CHECK(candidates[i].scanset_id == scan_a.scanset_id);
  }
  options.scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET;
  CHECK(lardon3d_visual_index_query(root, database, product_index,
                                    product_features[1].feature_set_id, &options, candidates, 6,
                                    &count) == LARDON3D_VISUAL_INDEX_OK);
  size_t rotation_rank = candidate_rank(candidates, count, product_features[4].feature_set_id);
  unrelated_rank = candidate_rank(candidates, count, product_features[5].feature_set_id);
  CHECK(rotation_rank != SIZE_MAX &&
        (unrelated_rank == SIZE_MAX || rotation_rank < unrelated_rank));
  Lardon3DVisualIndexConfiguration retry_configuration = {1, 15, 64};
  uint64_t retry_index = 0;
  CHECK(lardon3d_visual_index_create(database, &feature_a, &retry_configuration, &retry_index) ==
            LARDON3D_VISUAL_INDEX_OK &&
        setenv("LARDON3D_TEST_PROJECT_DB_FAIL_VISUAL_SEGMENT", "1", 1) == 0 &&
        lardon3d_visual_index_update_once(root, database, retry_index, 0, 0, 16, &last, &indexed) ==
            LARDON3D_VISUAL_INDEX_DB_BUSY &&
        unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_VISUAL_SEGMENT") == 0 &&
        lardon3d_visual_index_update_once(root, database, retry_index, 0, 0, 16, &last, &indexed) ==
            LARDON3D_VISUAL_INDEX_OK &&
        indexed == 5);
  Lardon3DVisualIndexConfiguration race_configuration = {1, 14, 64};
  uint64_t race_index = 0;
  CHECK(lardon3d_visual_index_create(database, &feature_a, &race_configuration, &race_index) ==
        LARDON3D_VISUAL_INDEX_OK);
  UpdateRace race = {.root = root, .database = database, .index_id = race_index};
  UpdateRaceThread race_threads[2] = {{&race, 0}, {&race, 1}};
  pthread_t update_threads[2];
  CHECK(pthread_barrier_init(&race.barrier, NULL, 2) == 0 &&
        pthread_create(&update_threads[0], NULL, update_race_thread, &race_threads[0]) == 0 &&
        pthread_create(&update_threads[1], NULL, update_race_thread, &race_threads[1]) == 0 &&
        pthread_join(update_threads[0], NULL) == 0 && pthread_join(update_threads[1], NULL) == 0 &&
        pthread_barrier_destroy(&race.barrier) == 0);
  CHECK((race.result[0] == LARDON3D_VISUAL_INDEX_OK) !=
        (race.result[1] == LARDON3D_VISUAL_INDEX_OK));
  CHECK(race.result[0] == LARDON3D_VISUAL_INDEX_OK ||
        race.result[0] == LARDON3D_VISUAL_INDEX_DB_ERROR ||
        race.result[0] == LARDON3D_VISUAL_INDEX_DB_BUSY);
  CHECK(race.result[1] == LARDON3D_VISUAL_INDEX_OK ||
        race.result[1] == LARDON3D_VISUAL_INDEX_DB_ERROR ||
        race.result[1] == LARDON3D_VISUAL_INDEX_DB_BUSY);
  CHECK(lardon3d_visual_index_update_once(root, database, race_index, 0, 0, 16, &last,
                                          &indexed) == LARDON3D_VISUAL_INDEX_NO_CHANGE);
  options.top_k = LARDON3D_VISUAL_INDEX_TOP_K_MAX + 1;
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) ==
        LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT);
  QueryThread contexts[4];
  pthread_t threads[4];
  for (size_t i = 0; i < 4; ++i) {
    contexts[i] = (QueryThread){root, database, index_id, feature_a.feature_set_id, false};
    CHECK(pthread_create(&threads[i], NULL, query_thread, &contexts[i]) == 0);
  }
  for (size_t i = 0; i < 4; ++i) {
    CHECK(pthread_join(threads[i], NULL) == 0 && contexts[i].ok);
  }
  Lardon3DProjectDbVisualIndexSegment segments[2];
  size_t segment_count = 0;
  CHECK(lardon3d_project_db_list_visual_index_segments(database, index_id, 0, segments, 2,
                                                        &segment_count) == LARDON3D_PROJECT_DB_OK &&
        segment_count == 1);
  char segment_path[PATH_MAX];
  CHECK(join_path(segment_path, root, segments[0].path));
  unsigned char *segment_bytes = malloc((size_t)segments[0].size_bytes);
  CHECK(segment_bytes);
  int descriptor = open(segment_path, O_RDWR | O_CLOEXEC);
  unsigned char original;
  CHECK(descriptor >= 0 &&
        pread(descriptor, segment_bytes, (size_t)segments[0].size_bytes, 0) ==
            (ssize_t)segments[0].size_bytes &&
        pread(descriptor, &original, 1, 12) == 1);
  unsigned char original_hash[32];
  char original_path[PATH_MAX];
  memcpy(original_hash, segments[0].sha256, sizeof(original_hash));
  CHECK(snprintf(original_path, sizeof(original_path), "%s", segments[0].path) > 0);
  options = (Lardon3DVisualIndexQueryOptions){4, 1, LARDON3D_VISUAL_INDEX_ANY_SCANSET, false};
  unsigned char *coherent = malloc((size_t)segments[0].size_bytes);
  char coherent_path[PATH_MAX];
  unsigned char coherent_hash[32];
  CHECK(coherent);
  memcpy(coherent, segment_bytes, (size_t)segments[0].size_bytes);
  coherent[8] = 2;
  coherent[9] = coherent[10] = coherent[11] = 0;
  CHECK(publish_mutated_segment(root, database_path, &segments[0], coherent,
                                (size_t)segments[0].size_bytes, coherent_path, coherent_hash) &&
        lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) ==
            LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION &&
        replace_segment_metadata(database_path, segments[0].visual_index_segment_id,
                                 original_hash, original_path, segments[0].size_bytes));
  memcpy(coherent, segment_bytes, (size_t)segments[0].size_bytes);
  memset(coherent + 24, 0xff, 8);
  CHECK(publish_mutated_segment(root, database_path, &segments[0], coherent,
                                (size_t)segments[0].size_bytes, coherent_path, coherent_hash) &&
        lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_CORRUPT &&
        replace_segment_metadata(database_path, segments[0].visual_index_segment_id,
                                 original_hash, original_path, segments[0].size_bytes));
  memcpy(coherent, segment_bytes, (size_t)segments[0].size_bytes);
  uint64_t overflow_count = (UINT64_MAX - 128U) / 24U + 1U;
  for (size_t i = 0; i < 8; ++i) {
    coherent[24 + i] = (unsigned char)(overflow_count >> (i * 8));
  }
  CHECK(publish_mutated_segment(root, database_path, &segments[0], coherent,
                                (size_t)segments[0].size_bytes, coherent_path, coherent_hash) &&
        lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_CORRUPT &&
        replace_segment_metadata(database_path, segments[0].visual_index_segment_id,
                                 original_hash, original_path, segments[0].size_bytes));
  free(coherent);
  unsigned char mutated = (unsigned char)(original ^ 0x40U);
  CHECK(pwrite(descriptor, &mutated, 1, 12) == 1 && close(descriptor) == 0);
  options = (Lardon3DVisualIndexQueryOptions){4, 1, LARDON3D_VISUAL_INDEX_ANY_SCANSET, false};
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_CORRUPT);
  descriptor = open(segment_path, O_RDWR | O_CLOEXEC);
  CHECK(descriptor >= 0 && pwrite(descriptor, &original, 1, 12) == 1 && close(descriptor) == 0);
  char hidden_path[PATH_MAX];
  CHECK(snprintf(hidden_path, sizeof(hidden_path), "%s.hidden", segment_path) > 0 &&
        rename(segment_path, hidden_path) == 0);
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_NOT_FOUND &&
        rename(hidden_path, segment_path) == 0);
  descriptor = open(segment_path, O_RDWR | O_CLOEXEC);
  CHECK(descriptor >= 0 && ftruncate(descriptor, (off_t)segments[0].size_bytes - 1) == 0 &&
        close(descriptor) == 0);
  CHECK(lardon3d_visual_index_query(root, database, index_id, feature_a.feature_set_id, &options,
                                    candidates, 4, &count) == LARDON3D_VISUAL_INDEX_CORRUPT);
  descriptor = open(segment_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  CHECK(descriptor >= 0 &&
        write(descriptor, segment_bytes, (size_t)segments[0].size_bytes) ==
            (ssize_t)segments[0].size_bytes &&
        close(descriptor) == 0);
  free(segment_bytes);
  lardon3d_project_db_close(database);
  database = NULL;
  state.project_db = NULL;
  CHECK(insert_scale_fixture(database_path, image_a.asset_id, feature_a.feature_asset_id,
                             &feature_a, 10000, 50000));
  CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
  state.project_db = database;
  Lardon3DVisualIndexConfiguration capacity_configuration = {1, 15, 4096};
  uint64_t capacity_index = 0;
  CHECK(lardon3d_visual_index_create(database, &feature_a, &capacity_configuration,
                                     &capacity_index) == LARDON3D_VISUAL_INDEX_OK);
  size_t total_indexed = 0;
  last = 0;
  for (size_t batch = 0; batch < LARDON3D_VISUAL_INDEX_SEGMENT_MAX; ++batch) {
    Lardon3DVisualIndexResult updated =
        lardon3d_visual_index_update_once(root, database, capacity_index, 0, last, 16, &last,
                                          &indexed);
    CHECK(updated == LARDON3D_VISUAL_INDEX_OK);
    total_indexed += indexed;
  }
  CHECK(total_indexed == 4096 &&
        lardon3d_visual_index_update_once(root, database, capacity_index, 0, last, 16, &last,
                                          &indexed) == LARDON3D_VISUAL_INDEX_LIMIT);
  Lardon3DProjectDbFeatureSet pending[16];
  size_t pending_count = 0;
  CHECK(lardon3d_project_db_list_visual_index_pending(database, capacity_index, last, pending, 16,
                                                       &pending_count) ==
            LARDON3D_PROJECT_DB_OK &&
        pending_count == 16);
  options = (Lardon3DVisualIndexQueryOptions){10, 1, LARDON3D_VISUAL_INDEX_ANY_SCANSET, false};
  CHECK(lardon3d_visual_index_query(root, database, capacity_index, feature_a.feature_set_id,
                                    &options,
                                    candidates, 10, &count) == LARDON3D_VISUAL_INDEX_OK &&
        count == 10);
  lardon3d_project_db_close(database);
  CHECK(remove_tree(root));
  return true;
}

int main(void) { return run_test() ? 0 : 1; }
