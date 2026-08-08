#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/image_catalog.h>

Lardon3DProjectDbResult lardon3d_project_db_test_delete_feature_identity(Lardon3DProjectDb *,
                                                                         uint64_t, uint64_t);

typedef struct {
  Lardon3DAppState *state;
  uint64_t image_id;
  const Lardon3DFeatureExtractorParameters *parameters;
  const Lardon3DExtractedFeatures *features;
  Lardon3DFeatureStoreResult result;
  Lardon3DProjectDbFeatureSet feature_set;
} PublishThreadContext;

static void *publish_thread(void *userdata) {
  PublishThreadContext *context = userdata;
  context->result =
      lardon3d_feature_store_publish(context->state, context->image_id, 0, context->parameters,
                                     context->features, &context->feature_set);
  return NULL;
}

#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "Échec ligne %d: %s\n", __LINE__, #x);                                       \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

static bool join_path(char out[PATH_MAX], const char *a, const char *b) {
  int n = snprintf(out, PATH_MAX, "%s/%s", a, b);
  return n > 0 && (size_t)n < PATH_MAX;
}
static bool remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(st.st_mode)) {
    return unlink(path) == 0;
  }
  DIR *d = opendir(path);
  if (!d) {
    return false;
  }
  bool ok = true;
  for (struct dirent *e = readdir(d); e; e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    char child[PATH_MAX];
    if (!join_path(child, path, e->d_name) || !remove_tree(child)) {
      ok = false;
    }
  }
  if (closedir(d) != 0 || rmdir(path) != 0) {
    ok = false;
  }
  return ok;
}
static size_t count_files(const char *path) {
  DIR *d = opendir(path);
  if (!d) {
    return 0;
  }
  size_t count = 0;
  for (struct dirent *e = readdir(d); e; e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    char child[PATH_MAX];
    struct stat s;
    if (join_path(child, path, e->d_name) && lstat(child, &s) == 0) {
      if (S_ISDIR(s.st_mode)) {
        count += count_files(child);
      } else if (S_ISREG(s.st_mode)) {
        ++count;
      }
    }
  }
  closedir(d);
  return count;
}

static bool write_pgm(const char *path, bool uniform) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    return false;
  }
  if (fprintf(f, "P5\n128 128\n255\n") <= 0) {
    fclose(f);
    return false;
  }
  for (unsigned int y = 0; y < 128; ++y) {
    for (unsigned int x = 0; x < 128; ++x) {
      unsigned char v = uniform ? 128U : (unsigned char)((((x / 8U) ^ (y / 8U)) & 1U) ? 240U : 15U);
      if (fwrite(&v, 1, 1, f) != 1) {
        fclose(f);
        return false;
      }
    }
  }
  return fclose(f) == 0;
}

static uint64_t read_u64_le(const unsigned char *bytes) {
  uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value |= (uint64_t)bytes[index] << (8U * index);
  }
  return value;
}

static bool restore_file(const char *path, const unsigned char *bytes, size_t size) {
  int descriptor = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  size_t written = 0;
  while (written < size) {
    ssize_t amount = write(descriptor, bytes + written, size - written);
    if (amount <= 0) {
      close(descriptor);
      return false;
    }
    written += (size_t)amount;
  }
  return close(descriptor) == 0;
}

static bool expect_reader_result(const char *root, const Lardon3DProjectDbFeatureSet *feature_set,
                                 Lardon3DFeatureStoreResult expected) {
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  Lardon3DFeatureStoreResult result =
      lardon3d_feature_reader_open(root, feature_set, &reader, &metadata);
  lardon3d_feature_reader_close(reader);
  return result == expected;
}

static bool run_test(void) {
  char root[] = "/tmp/lardon3d-feature-store-XXXXXX";
  CHECK(mkdtemp(root));
  char dbpath[PATH_MAX], structured[PATH_MAX], uniform[PATH_MAX];
  CHECK(join_path(dbpath, root, "project.db") && join_path(structured, root, "structured.pgm") &&
        join_path(uniform, root, "uniform.pgm"));
  CHECK(write_pgm(structured, false) && write_pgm(uniform, true));
  Lardon3DProjectDb *db = NULL;
  char error[256];
  CHECK(lardon3d_project_db_open(dbpath, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = db;
  CHECK(snprintf(state.project_path, sizeof(state.project_path), "%s", root) > 0);
  Lardon3DProjectDbScanSet a, b;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &a) &&
        lardon3d_image_catalog_create_scanset(&state, "B", &b));
  Lardon3DProjectDbImage ia, ib, iu;
  Lardon3DProjectDbImageAsset aa, ab, au;
  CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id, structured, 0, &ia, &aa) ==
        LARDON3D_IMAGE_CATALOG_IMPORTED);
  CHECK(lardon3d_image_catalog_import_file(&state, b.scanset_id, structured, 0, &ib, &ab) ==
            LARDON3D_IMAGE_CATALOG_IMPORTED &&
        aa.asset_id == ab.asset_id);
  CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id, uniform, 0, &iu, &au) ==
        LARDON3D_IMAGE_CATALOG_IMPORTED);
  Lardon3DFeatureExtractorParameters p = {
      .max_features = 512, .pyramid_levels = 4, .fast_threshold = 10};
  CHECK(lardon3d_feature_extractor_parameters_valid(&p));
  CHECK(!lardon3d_feature_extractor_parameters_valid(
      &(Lardon3DFeatureExtractorParameters){0, 4, 10}));
  CHECK(!lardon3d_feature_extractor_parameters_valid(
      &(Lardon3DFeatureExtractorParameters){8193, 4, 10}));
  CHECK(!lardon3d_feature_extractor_parameters_valid(
      &(Lardon3DFeatureExtractorParameters){512, 0, 10}));
  CHECK(!lardon3d_feature_extractor_parameters_valid(
      &(Lardon3DFeatureExtractorParameters){512, 17, 10}));
  CHECK(!lardon3d_feature_extractor_parameters_valid(
      &(Lardon3DFeatureExtractorParameters){512, 4, 0}));
  CHECK(!lardon3d_feature_extractor_parameters_valid(
      &(Lardon3DFeatureExtractorParameters){512, 4, 256}));
  unsigned char fp[32], fp2[32];
  lardon3d_feature_extractor_parameter_fingerprint(&p, fp);
  lardon3d_feature_extractor_parameter_fingerprint(&p, fp2);
  CHECK(memcmp(fp, fp2, 32) == 0);
  Lardon3DFeatureExtractorParameters p2 = p;
  p2.max_features = 256;
  lardon3d_feature_extractor_parameter_fingerprint(&p2, fp2);
  CHECK(memcmp(fp, fp2, 32) != 0);
  Lardon3DExtractedFeatures f;
  CHECK(lardon3d_feature_extract_orb(structured, &p, &f) == LARDON3D_FEATURE_EXTRACT_OK &&
        f.feature_count > 0 && f.feature_count <= 512);
  Lardon3DProjectDbFeatureSet sa, same, sb;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &p, &f, &sa) ==
        LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &p, &f, &same) ==
            LARDON3D_FEATURE_STORE_ALREADY_PRESENT &&
        same.feature_set_id == sa.feature_set_id);
  CHECK(lardon3d_feature_store_publish(&state, ib.image_id, 0, &p, &f, &sb) ==
            LARDON3D_FEATURE_STORE_OK &&
        sb.feature_set_id != sa.feature_set_id && sb.feature_asset_id == sa.feature_asset_id);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  CHECK(lardon3d_feature_reader_open(root, &sa, &reader, &metadata) == LARDON3D_FEATURE_STORE_OK &&
        metadata.feature_count == f.feature_count);
  size_t chunk = metadata.feature_count < 8 ? metadata.feature_count : 8;
  Lardon3DFeatureKeypoint points[8];
  unsigned char descriptors[8 * 32];
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, points, chunk) == LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_descriptors(reader, 0, descriptors, chunk, sizeof(descriptors)) ==
        LARDON3D_FEATURE_STORE_OK);
  for (size_t i = 0; i < chunk; ++i) {
    CHECK(isfinite(points[i].x) && memcmp(descriptors + i * 32, f.descriptors + i * 32, 32) == 0);
  }
  CHECK(lardon3d_feature_reader_keypoints(reader, metadata.feature_count, points, 1) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  CHECK(lardon3d_feature_reader_descriptors(reader, UINT32_MAX, descriptors, 1,
                                            sizeof(descriptors)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, points, 257) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  CHECK(lardon3d_feature_reader_descriptors(reader, 0, descriptors, 257, sizeof(descriptors)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  float wrong_type_descriptor[128];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, wrong_type_descriptor, 1,
                                                sizeof(wrong_type_descriptor)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  lardon3d_feature_reader_close(reader);

  uint32_t sift_count = f.feature_count < 300 ? f.feature_count : 300;
  Lardon3DExtractedFeatures sift = {.image_width = f.image_width,
                                    .image_height = f.image_height,
                                    .feature_count = sift_count};
  sift.keypoints = calloc(sift_count, sizeof(*sift.keypoints));
  sift.descriptors = calloc((size_t)sift_count * 128, sizeof(float));
  sift.descriptor_bytes = (size_t)sift_count * 128 * sizeof(float);
  CHECK(sift_count > 0 && sift.keypoints && sift.descriptors);
  memcpy(sift.keypoints, f.keypoints, (size_t)sift_count * sizeof(*sift.keypoints));
  float *sift_values = (float *)sift.descriptors;
  for (size_t i = 0; i < (size_t)sift_count * 128; ++i) {
    sift_values[i] = (float)(i % 129) / 128.0F;
  }
  unsigned char sift_fp[32] = {0x53, 0x49, 0x46, 0x54};
  uint32_t capabilities = LARDON3D_FEATURE_HAS_SCALE |
                          LARDON3D_FEATURE_HAS_ORIENTATION |
                          LARDON3D_FEATURE_HAS_RESPONSE | LARDON3D_FEATURE_HAS_OCTAVE;
  Lardon3DProjectDbFeatureSet sift_set;
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "sift", 1, sift_fp, LARDON3D_FEATURE_DESCRIPTOR_F32, 128,
            capabilities, &sift, &sift_set) == LARDON3D_FEATURE_STORE_OK);
  Lardon3DProjectDbFeatureSet future_extractor_set;
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "sift", 2, sift_fp, LARDON3D_FEATURE_DESCRIPTOR_F32, 128,
            capabilities, &sift, &future_extractor_set) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  CHECK(lardon3d_feature_reader_open(root, &sift_set, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK &&
        metadata.format_version == 2 &&
        metadata.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32 &&
        metadata.descriptor_dimension == 128 && metadata.descriptor_scalar_size_bytes == 4 &&
        metadata.capabilities == capabilities);
  float sift_range[256 * 128];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, sift_range, 1,
                                                128 * sizeof(float)) ==
            LARDON3D_FEATURE_STORE_OK &&
        memcmp(sift_range, sift_values, 128 * sizeof(float)) == 0);
  CHECK(lardon3d_feature_reader_descriptors_u8(reader, 0, descriptors, 1,
                                               sizeof(descriptors)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  uint32_t sift_chunk = sift_count < 256 ? sift_count : 256;
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, sift_count - sift_chunk, sift_range,
                                                sift_chunk, sizeof(sift_range)) ==
        LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, sift_count / 2, sift_range, 1,
                                                128 * sizeof(float)) ==
        LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, sift_range, 257,
                                                sizeof(sift_range)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  lardon3d_feature_reader_close(reader);
  ((float *)sift.descriptors)[0] = NAN;
  unsigned char invalid_sift_fp[32] = {0x54};
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "sift", 1, invalid_sift_fp,
            LARDON3D_FEATURE_DESCRIPTOR_F32, 128, capabilities, &sift, &same) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  ((float *)sift.descriptors)[0] = sift_values[1];
  Lardon3DExtractedFeatures wrong_sized_sift = sift;
  wrong_sized_sift.descriptor_bytes -= sizeof(float);
  invalid_sift_fp[0]++;
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "sift", 1, invalid_sift_fp,
            LARDON3D_FEATURE_DESCRIPTOR_F32, 128, capabilities, &wrong_sized_sift, &same) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  wrong_sized_sift.descriptor_bytes = sift.descriptor_bytes + sizeof(float);
  invalid_sift_fp[0]++;
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "sift", 1, invalid_sift_fp,
            LARDON3D_FEATURE_DESCRIPTOR_F32, 128, capabilities, &wrong_sized_sift, &same) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  ((float *)sift.descriptors)[0] = INFINITY;
  invalid_sift_fp[0]++;
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "sift", 1, invalid_sift_fp,
            LARDON3D_FEATURE_DESCRIPTOR_F32, 128, capabilities, &sift, &same) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  ((float *)sift.descriptors)[0] = sift_values[1];

  const char *invalid_paths[] = {
      "/assets/features/00/hash",
      "../assets/features/00/hash",
      "assets/features/AA/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "assets/features/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "assets/features/0/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "assets/features/aa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  };
  for (size_t index = 0; index < sizeof(invalid_paths) / sizeof(invalid_paths[0]); ++index) {
    Lardon3DProjectDbFeatureSet invalid_path_set = sa;
    CHECK(snprintf(invalid_path_set.asset.path, sizeof(invalid_path_set.asset.path), "%s",
                   invalid_paths[index]) > 0);
    CHECK(lardon3d_feature_reader_open(root, &invalid_path_set, &reader, &metadata) ==
          LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  }

  Lardon3DExtractedFeatures invalid_features = f;
  Lardon3DFeatureKeypoint invalid_point = f.keypoints[0];
  invalid_point.x = NAN;
  invalid_features.keypoints = &invalid_point;
  invalid_features.feature_count = 1;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &p2, &invalid_features, &same) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);

  Lardon3DFeatureExtractorParameters concurrent_parameters = {508, 4, 10};
  PublishThreadContext concurrent[2] = {
      {.state = &state,
       .image_id = ia.image_id,
       .parameters = &concurrent_parameters,
       .features = &f},
      {.state = &state,
       .image_id = ia.image_id,
       .parameters = &concurrent_parameters,
       .features = &f},
  };
  pthread_t publish_threads[2];
  CHECK(pthread_create(&publish_threads[0], NULL, publish_thread, &concurrent[0]) == 0 &&
        pthread_create(&publish_threads[1], NULL, publish_thread, &concurrent[1]) == 0);
  CHECK(pthread_join(publish_threads[0], NULL) == 0 && pthread_join(publish_threads[1], NULL) == 0);
  CHECK((concurrent[0].result == LARDON3D_FEATURE_STORE_OK ||
         concurrent[0].result == LARDON3D_FEATURE_STORE_ALREADY_PRESENT) &&
        (concurrent[1].result == LARDON3D_FEATURE_STORE_OK ||
         concurrent[1].result == LARDON3D_FEATURE_STORE_ALREADY_PRESENT) &&
        concurrent[0].feature_set.feature_set_id == concurrent[1].feature_set.feature_set_id);
  Lardon3DExtractedFeatures uf;
  CHECK(lardon3d_feature_extract_orb(uniform, &p, &uf) == LARDON3D_FEATURE_EXTRACT_OK &&
        uf.feature_count == 0);
  Lardon3DProjectDbFeatureSet us;
  CHECK(lardon3d_feature_store_publish(&state, iu.image_id, 0, &p, &uf, &us) ==
        LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_open(root, &us, &reader, &metadata) == LARDON3D_FEATURE_STORE_OK &&
        metadata.feature_count == 0);
  lardon3d_feature_reader_close(reader);
  Lardon3DFeatureExtractorParameters large_parameters = {8192, 4, 10};
  Lardon3DExtractedFeatures large = {
      .image_width = 128, .image_height = 128, .feature_count = 8192};
  large.keypoints = calloc(large.feature_count, sizeof(*large.keypoints));
  large.descriptors = calloc(large.feature_count, 32);
  large.descriptor_bytes = (size_t)large.feature_count * 32;
  CHECK(large.keypoints && large.descriptors);
  for (uint32_t i = 0; i < large.feature_count; ++i) {
    large.keypoints[i] =
        (Lardon3DFeatureKeypoint){(float)(i % 128), (float)((i / 128) % 64), 1, 0, 1, 0};
    memset(large.descriptors + (size_t)i * 32, (int)(i & 255), 32);
  }
  Lardon3DProjectDbFeatureSet large_set;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &large_parameters, &large,
                                       &large_set) == LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_open(root, &large_set, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK &&
        metadata.feature_count == 8192);
  Lardon3DFeatureKeypoint last;
  unsigned char last_descriptor[32];
  CHECK(lardon3d_feature_reader_keypoints(reader, 8191, &last, 1) == LARDON3D_FEATURE_STORE_OK &&
        lardon3d_feature_reader_descriptors(reader, 8191, last_descriptor, 1,
                                            sizeof(last_descriptor)) == LARDON3D_FEATURE_STORE_OK &&
        last_descriptor[0] == 255);
  lardon3d_feature_reader_close(reader);
  Lardon3DExtractedFeatures large_float = {
      .image_width = 128, .image_height = 128, .feature_count = 8192};
  large_float.keypoints = calloc(large_float.feature_count, sizeof(*large_float.keypoints));
  large_float.descriptors = calloc((size_t)large_float.feature_count * 128, sizeof(float));
  large_float.descriptor_bytes = (size_t)large_float.feature_count * 128 * sizeof(float);
  CHECK(large_float.keypoints && large_float.descriptors);
  memcpy(large_float.keypoints, large.keypoints,
         (size_t)large_float.feature_count * sizeof(*large_float.keypoints));
  unsigned char large_float_fp[32] = {0x4c, 0x41, 0x52, 0x47, 0x45};
  Lardon3DProjectDbFeatureSet large_float_set;
  CHECK(lardon3d_feature_store_publish_v2(
            &state, ia.image_id, 0, "rootsift", 1, large_float_fp,
            LARDON3D_FEATURE_DESCRIPTOR_F32, 128, capabilities, &large_float,
            &large_float_set) == LARDON3D_FEATURE_STORE_OK &&
        lardon3d_feature_reader_open(root, &large_float_set, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK &&
        metadata.feature_count == 8192);
  float large_float_range[256 * 128];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, large_float_range, 1,
                                                sizeof(large_float_range)) ==
            LARDON3D_FEATURE_STORE_OK &&
        lardon3d_feature_reader_descriptors_f32(reader, 4096, large_float_range, 256,
                                                sizeof(large_float_range)) ==
            LARDON3D_FEATURE_STORE_OK &&
        lardon3d_feature_reader_descriptors_f32(reader, 8191, large_float_range, 1,
                                                sizeof(large_float_range)) ==
            LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);
  lardon3d_extracted_features_destroy(&large_float);
  lardon3d_extracted_features_destroy(&large);
  char features_dir[PATH_MAX];
  CHECK(join_path(features_dir, root, "assets/features"));
  size_t before = count_files(features_dir);
  Lardon3DFeatureExtractorParameters failed_parameters = {511, 4, 10};
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_FEATURE_REGISTER", "1", 1) == 0);
  Lardon3DProjectDbFeatureSet failed_set;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &failed_parameters, &f,
                                       &failed_set) == LARDON3D_FEATURE_STORE_DB_BUSY);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_FEATURE_REGISTER") == 0 &&
        count_files(features_dir) == before + 1);
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &failed_parameters, &f,
                                       &failed_set) == LARDON3D_FEATURE_STORE_OK);
  Lardon3DFeatureExtractorParameters nondurable_parameters = {510, 4, 10};
  CHECK(setenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC", "1", 1) == 0);
  Lardon3DProjectDbFeatureSet nondurable;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &nondurable_parameters, &f,
                                       &nondurable) ==
            LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE &&
        nondurable.asset.durability == LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE);
  CHECK(unsetenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC") == 0);
  Lardon3DProjectDbFeatureSet promoted;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &nondurable_parameters, &f,
                                       &promoted) == LARDON3D_FEATURE_STORE_ALREADY_PRESENT &&
        promoted.feature_set_id == nondurable.feature_set_id &&
        promoted.asset.durability == LARDON3D_DB_FEATURE_ASSET_DURABLE);
  uint64_t deleted_set = nondurable.feature_set_id, deleted_asset = nondurable.feature_asset_id;
  CHECK(lardon3d_project_db_test_delete_feature_identity(db, deleted_set, deleted_asset) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DFeatureExtractorParameters replacement_parameters = {509, 4, 10};
  Lardon3DProjectDbFeatureSet replacement;
  CHECK(lardon3d_feature_store_publish(&state, ia.image_id, 0, &replacement_parameters, &f,
                                       &replacement) == LARDON3D_FEATURE_STORE_OK &&
        replacement.feature_set_id > deleted_set && replacement.feature_asset_id > deleted_asset);
  Lardon3DProjectDbFeatureSet page[8];
  size_t count = 0;
  CHECK(lardon3d_project_db_list_feature_sets(db, 0, page, 1, &count) == LARDON3D_PROJECT_DB_OK &&
        count == 1);
  CHECK(lardon3d_project_db_list_feature_sets(db, page[0].feature_set_id, page, 8, &count) ==
            LARDON3D_PROJECT_DB_OK &&
        count == 8);
  for (uint32_t index = 0; index < 2000; ++index) {
    unsigned char metadata_fingerprint[32] = {0};
    metadata_fingerprint[0] = (unsigned char)index;
    metadata_fingerprint[1] = (unsigned char)(index >> 8);
    metadata_fingerprint[2] = 0xa5;
    Lardon3DProjectDbFeatureSet metadata_set;
    CHECK(lardon3d_project_db_register_feature_set(
              db, ia.image_id, "metadata.fixture", 1, metadata_fingerprint, aa.sha256,
              sa.feature_count, sa.descriptor_type, sa.descriptor_dimension, sa.asset.sha256,
              sa.asset.path, sa.asset.size_bytes, sa.asset.durability, 0, index,
              &metadata_set) == LARDON3D_PROJECT_DB_OK);
  }
  Lardon3DProjectDbFeatureSet *metadata_page = calloc(256, sizeof(*metadata_page));
  CHECK(metadata_page);
  const size_t page_sizes[] = {1, 8, 64, 256};
  for (size_t index = 0; index < sizeof(page_sizes) / sizeof(page_sizes[0]); ++index) {
    CHECK(lardon3d_project_db_list_feature_sets(db, 0, metadata_page, page_sizes[index], &count) ==
              LARDON3D_PROJECT_DB_OK &&
          count == page_sizes[index]);
  }
  CHECK(lardon3d_project_db_list_feature_sets(db, 0, metadata_page, 257, &count) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  free(metadata_page);
  char asset_path[PATH_MAX];
  CHECK(join_path(asset_path, root, sa.asset.path));
  int fd = open(asset_path, O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  struct stat asset_info;
  CHECK(fstat(fd, &asset_info) == 0 && asset_info.st_size >= 160);
  size_t original_size = (size_t)asset_info.st_size;
  unsigned char *original = malloc(original_size);
  CHECK(original && read(fd, original, original_size) == (ssize_t)original_size && close(fd) == 0);

  uint64_t descriptor_offset = read_u64_le(original + 48);
  const off_t truncated_sizes[] = {
      100,
      (off_t)descriptor_offset - 1,
      (off_t)descriptor_offset,
      (off_t)original_size - 1,
  };
  for (size_t index = 0; index < sizeof(truncated_sizes) / sizeof(truncated_sizes[0]); ++index) {
    CHECK(truncate(asset_path, truncated_sizes[index]) == 0 &&
          expect_reader_result(root, &sa, LARDON3D_FEATURE_STORE_CORRUPT) &&
          restore_file(asset_path, original, original_size));
  }

  const size_t corrupt_offsets[] = {12, 16, 20, 24, 28, 40, 48, 56, 148, 144, 128};
  for (size_t index = 0; index < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); ++index) {
    unsigned char mutated = (unsigned char)(original[corrupt_offsets[index]] ^ 0x5aU);
    fd = open(asset_path, O_WRONLY | O_CLOEXEC);
    CHECK(fd >= 0 && pwrite(fd, &mutated, 1, (off_t)corrupt_offsets[index]) == 1 &&
          close(fd) == 0 && expect_reader_result(root, &sa, LARDON3D_FEATURE_STORE_CORRUPT) &&
          restore_file(asset_path, original, original_size));
  }

  unsigned char future_version[4] = {3, 0, 0, 0};
  fd = open(asset_path, O_WRONLY | O_CLOEXEC);
  CHECK(fd >= 0 && pwrite(fd, future_version, sizeof(future_version), 8) == 4 && close(fd) == 0 &&
        expect_reader_result(root, &sa, LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION) &&
        restore_file(asset_path, original, original_size));

  char sift_asset_path[PATH_MAX];
  CHECK(join_path(sift_asset_path, root, sift_set.asset.path));
  fd = open(sift_asset_path, O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0 && fstat(fd, &asset_info) == 0 && asset_info.st_size >= 176);
  size_t sift_original_size = (size_t)asset_info.st_size;
  unsigned char *sift_original = malloc(sift_original_size);
  CHECK(sift_original && read(fd, sift_original, sift_original_size) ==
                             (ssize_t)sift_original_size &&
        close(fd) == 0);
  const size_t v2_corrupt_offsets[] = {12, 16, 20, 24, 28, 32, 36, 40, 44,
                                       48, 56, 64, 72, 104, 136, 152, 160};
  for (size_t index = 0; index < sizeof(v2_corrupt_offsets) / sizeof(v2_corrupt_offsets[0]);
       ++index) {
    size_t offset = v2_corrupt_offsets[index];
    unsigned char mutated = (unsigned char)(sift_original[offset] ^ 0x5aU);
    fd = open(sift_asset_path, O_WRONLY | O_CLOEXEC);
    CHECK(fd >= 0 && pwrite(fd, &mutated, 1, (off_t)offset) == 1 && close(fd) == 0 &&
          expect_reader_result(root, &sift_set, LARDON3D_FEATURE_STORE_CORRUPT) &&
          restore_file(sift_asset_path, sift_original, sift_original_size));
  }
  fd = open(sift_asset_path, O_WRONLY | O_CLOEXEC);
  CHECK(fd >= 0 && pwrite(fd, future_version, sizeof(future_version), 8) == 4 && close(fd) == 0 &&
        expect_reader_result(root, &sift_set, LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION) &&
        restore_file(sift_asset_path, sift_original, sift_original_size));
  uint64_t sift_descriptor_offset = read_u64_le(sift_original + 56);
  unsigned char nan_le[4] = {0, 0, 0xc0, 0x7f};
  fd = open(sift_asset_path, O_WRONLY | O_CLOEXEC);
  CHECK(fd >= 0 && pwrite(fd, nan_le, sizeof(nan_le), (off_t)sift_descriptor_offset) == 4 &&
        close(fd) == 0 &&
        expect_reader_result(root, &sift_set, LARDON3D_FEATURE_STORE_CORRUPT) &&
        expect_reader_result(root, &sa, LARDON3D_FEATURE_STORE_OK) &&
        restore_file(sift_asset_path, sift_original, sift_original_size));
  free(sift_original);

  fd = open(asset_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  CHECK(fd >= 0 && write(fd, "bad", 3) == 3 && close(fd) == 0 &&
        expect_reader_result(root, &sa, LARDON3D_FEATURE_STORE_CORRUPT));
  free(original);
  lardon3d_extracted_features_destroy(&f);
  lardon3d_extracted_features_destroy(&sift);
  lardon3d_extracted_features_destroy(&uf);
  lardon3d_project_db_close(db);
  CHECK(remove_tree(root));
  return true;
}

int main(void) { return run_test() ? 0 : 1; }
