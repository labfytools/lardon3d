#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/project_db.h>
#include <lardon3d/sparse_sfm_model.h>

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      fprintf(stderr, "Sparse SfM check failed at line %d: %s\n", __LINE__,    \
              #value);                                                         \
      return false;                                                            \
    }                                                                          \
  } while (0)

static int raw_exec(const char *path, const char *sql) {
  sqlite3 *connection = NULL;
  char *message = NULL;
  int code = sqlite3_open_v2(path, &connection,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (code == SQLITE_OK)
    code = sqlite3_exec(connection, "PRAGMA foreign_keys=ON", NULL, NULL,
                        &message);
  if (code == SQLITE_OK)
    code = sqlite3_exec(connection, sql, NULL, NULL, &message);
  sqlite3_free(message);
  if (connection)
    sqlite3_close(connection);
  return code;
}

static bool raw_constraint(const char *path, const char *sql) {
  return raw_exec(path, sql) == SQLITE_CONSTRAINT;
}

static bool raw_scalar_equals(const char *path, const char *sql,
                              sqlite3_int64 expected) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  bool matches = false;
  if (sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) ==
      SQLITE_OK &&
      sqlite3_prepare_v2(connection, sql, -1, &statement, NULL) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW)
    matches = sqlite3_column_int64(statement, 0) == expected;
  if (statement)
    sqlite3_finalize(statement);
  if (connection)
    sqlite3_close(connection);
  return matches;
}

static bool register_image(Lardon3DProjectDb *db, uint64_t scanset_id,
                           unsigned char seed, uint64_t *image_id) {
  unsigned char hash[32] = {0};
  char asset_path[128];
  char hex[65];
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageRegisterStatus status;
  hash[0] = seed;
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < 32; ++index) {
    hex[index * 2] = digits[hash[index] >> 4];
    hex[index * 2 + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(asset_path, sizeof(asset_path), "assets/images/%c%c/%s",
                 hex[0], hex[1], hex);
  Lardon3DProjectDbResult result = lardon3d_project_db_register_image(
      db, scanset_id, hash, asset_path, 1, "test.jpg", "/test.jpg", 0, 1,
      &status, &image);
  if (result != LARDON3D_PROJECT_DB_OK) {
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    (void)lardon3d_project_db_last_error(db, error);
    fprintf(stderr, "register image: %s (%d)\n", error, (int)result);
  }
  return result == LARDON3D_PROJECT_DB_OK && (*image_id = image.image_id) != 0;
}

static bool run_test(void) {
  char path[] = "/tmp/lardon3d-sparse-model-XXXXXX";
  int descriptor = mkstemp(path);
  CHECK(descriptor >= 0);
  close(descriptor);
  unlink(path);

  Lardon3DProjectDb *db = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(db) == 17);

  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_project_db_create_scanset(db, "Sparse model", &scanset) ==
        LARDON3D_PROJECT_DB_OK);
  uint64_t image_a = 0;
  uint64_t image_b = 0;
  CHECK(register_image(db, scanset.scanset_id, 1, &image_a));
  CHECK(register_image(db, scanset.scanset_id, 2, &image_b));

  Lardon3DSparseCalibration calibration = {0};
  calibration.model_kind = LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE;
  calibration.model_version = LARDON3D_SPARSE_SFM_CALIBRATION_VERSION;
  calibration.width = 4000;
  calibration.height = 3000;
  calibration.fx = 2000.0;
  calibration.fy = 2001.0;
  calibration.cx = 2000.0;
  calibration.cy = 1500.0;
  calibration.provenance_kind = LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT;
  Lardon3DSparseCalibration stored;
  CHECK(lardon3d_sparse_calibration_create(db, &calibration, &stored) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibration alternate_calibration = calibration;
  alternate_calibration.provenance_fingerprint[0] = 77;
  Lardon3DSparseCalibration alternate_stored;
  CHECK(lardon3d_sparse_calibration_create(
            db, &alternate_calibration, &alternate_stored) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibration duplicate;
  CHECK(lardon3d_sparse_calibration_create(db, &calibration, &duplicate) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(stored.calibration_id == duplicate.calibration_id);

  Lardon3DSparseCalibrationMember members[2] = {
      {.image_id = image_b, .calibration_id = stored.calibration_id},
      {.image_id = image_a, .calibration_id = stored.calibration_id},
  };
  memcpy(members[0].calibration_hash, stored.scientific_hash, 32);
  memcpy(members[1].calibration_hash, stored.scientific_hash, 32);
  Lardon3DSparseCalibrationMember mismatched_members[2];
  memcpy(mismatched_members, members, sizeof(mismatched_members));
  mismatched_members[0].calibration_hash[0] ^= 1U;
  Lardon3DSparseCalibrationScope scope;
  CHECK(lardon3d_sparse_calibration_scope_create(
            db, mismatched_members, 2, &scope) != LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_sparse_calibration_scope_create(db, members, 2, &scope) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationMember page_members[2] = {members[0], members[1]};
  page_members[1].calibration_id = alternate_stored.calibration_id;
  memcpy(page_members[1].calibration_hash, alternate_stored.scientific_hash,
         32);
  Lardon3DSparseCalibrationScope page_scope;
  CHECK(lardon3d_sparse_calibration_scope_create(
            db, page_members, 2, &page_scope) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationMember reordered[2] = {members[1], members[0]};
  Lardon3DSparseCalibrationScope same_scope;
  CHECK(lardon3d_sparse_calibration_scope_create(
            db, reordered, 2, &same_scope) == LARDON3D_PROJECT_DB_OK);
  CHECK(scope.scope_id == same_scope.scope_id);

  Lardon3DSparseCalibrationMember page_items[2];
  size_t page_count = 0;
  uint64_t cursor = 0;
  CHECK(lardon3d_sparse_calibration_scope_list_members(
            db, scope.scope_id, 0, page_items, 1, &page_count, &cursor) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(page_count == 1 && cursor != 0);
  CHECK(lardon3d_sparse_calibration_scope_list_members(
            db, scope.scope_id, cursor, page_items, 1, &page_count, &cursor) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(page_count == 1);

  unsigned char parameter_fingerprint[32] = {3};
  unsigned char feature_hash_a[32] = {4};
  unsigned char feature_hash_b[32] = {5};
  unsigned char source_hash_a[32] = {1};
  unsigned char source_hash_b[32] = {2};
  Lardon3DProjectDbFeatureSet feature_a;
  Lardon3DProjectDbFeatureSet feature_b;
  char feature_path_a[128];
  char feature_path_b[128];
  (void)snprintf(feature_path_a, sizeof(feature_path_a),
                 "assets/features/04/04%062x", 0U);
  (void)snprintf(feature_path_b, sizeof(feature_path_b),
                 "assets/features/05/05%062x", 0U);
  Lardon3DProjectDbResult feature_result =
      lardon3d_project_db_register_feature_set(
          db, image_a, "orb", 1, parameter_fingerprint, source_hash_a, 128, 1,
          32, feature_hash_a, feature_path_a, 128,
          LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 1, &feature_a);
  if (feature_result != LARDON3D_PROJECT_DB_OK) {
    char feature_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    (void)lardon3d_project_db_last_error(db, feature_error);
    fprintf(stderr, "feature registration failed: %d %s\n", feature_result,
            feature_error);
  }
  CHECK(feature_result == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_register_feature_set(
            db, image_b, "orb", 1, parameter_fingerprint, source_hash_b, 10, 1,
            32, feature_hash_b, feature_path_b, 128,
            LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 1,
            &feature_b) == LARDON3D_PROJECT_DB_OK);

  Lardon3DProjectDbTrackObservation track_observations[4] = {
      {.feature_set_id = feature_a.feature_set_id,
       .feature_index = 1,
       .position_in_track = 0},
      {.feature_set_id = feature_b.feature_set_id,
       .feature_index = 1,
       .position_in_track = 1},
      {.feature_set_id = feature_a.feature_set_id,
       .feature_index = 2,
       .position_in_track = 0},
      {.feature_set_id = feature_b.feature_set_id,
       .feature_index = 2,
       .position_in_track = 1},
  };
  Lardon3DProjectDbTrack tracks[2] = {
      {.observation_count = 2, .observations = &track_observations[0]},
      {.observation_count = 2, .observations = &track_observations[2]},
  };
  Lardon3DProjectDbTrackSet track_configuration = {0};
  (void)snprintf(track_configuration.builder_kind,
                 sizeof(track_configuration.builder_kind), "test_builder");
  track_configuration.builder_version = 1;
  track_configuration.parameter_fingerprint[0] = 6;
  track_configuration.verifier_kind = 1;
  track_configuration.verifier_version = 1;
  track_configuration.verifier_fingerprint[0] = 7;
  track_configuration.input_scope_hash[0] = 8;
  track_configuration.gvr_count = 1;
  track_configuration.track_count = 2;
  Lardon3DProjectDbTrackSet published_tracks;
  CHECK(lardon3d_project_db_create_track_set(db, &track_configuration, tracks,
                                             2, &published_tracks) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbTrack loaded_tracks[2];
  memset(loaded_tracks, 0, sizeof(loaded_tracks));
  size_t loaded_count = 0;
  CHECK(lardon3d_project_db_list_tracks(db, published_tracks.track_set_id, 0,
                                        loaded_tracks, 2, &loaded_count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_count == 2);

  Lardon3DSparseComponent component = {.component_key = image_a,
                                       .registered_image_count = 2,
                                       .landmark_count = 2};
  Lardon3DSparseRegisteredImage poses[2] = {
      {.image_id = image_a,
       .component_key = image_a,
       .rotation_cw = {1, 0, 0, 0, 1, 0, 0, 0, 1},
       .translation_cw = {0, 0, 0}},
      {.image_id = image_b,
       .component_key = image_a,
       .rotation_cw = {1, 0, 0, 0, 1, 0, 0, 0, 1},
       .translation_cw = {1, 0, 0}},
  };
  Lardon3DSparseLandmark landmarks[2] = {
      {.track_id = loaded_tracks[0].track_id,
       .component_key = image_a,
       .x = 0,
       .y = 0,
       .z = 4,
       .reprojection_rmse_px = 0.1,
       .reprojection_median_px = 0.1,
       .observation_count = 2},
      {.track_id = loaded_tracks[1].track_id,
       .component_key = image_a,
       .x = 1,
       .y = 0,
       .z = 4,
       .reprojection_rmse_px = 0.1,
       .reprojection_median_px = 0.1,
       .observation_count = 2},
  };
  Lardon3DSparseLandmarkObservation observations[4] = {
      {.track_id = loaded_tracks[0].track_id,
       .feature_set_id = feature_a.feature_set_id,
       .feature_index = 1,
       .position_in_track = 0},
      {.track_id = loaded_tracks[0].track_id,
       .feature_set_id = feature_b.feature_set_id,
       .feature_index = 1,
       .position_in_track = 1},
      {.track_id = loaded_tracks[1].track_id,
       .feature_set_id = feature_a.feature_set_id,
       .feature_index = 2,
       .position_in_track = 0},
      {.track_id = loaded_tracks[1].track_id,
       .feature_set_id = feature_b.feature_set_id,
       .feature_index = 2,
       .position_in_track = 1},
  };
  Lardon3DSparsePublication publication = {
      .track_set_id = published_tracks.track_set_id,
      .calibration_scope_id = scope.scope_id,
      .sfm_kind = LARDON3D_SPARSE_SFM_KIND_INCREMENTAL,
      .sfm_version = LARDON3D_SPARSE_SFM_VERSION,
      .components = &component,
      .component_count = 1,
      .registered_images = poses,
      .registered_image_count = 2,
      .landmarks = landmarks,
      .landmark_count = 2,
      .observations = observations,
      .observation_count = 4,
      .reprojection_rmse_px = 0.1,
      .reprojection_median_px = 0.1,
  };
  publication.parameter_fingerprint[0] = 9;
  Lardon3DSparseReconstruction reconstruction;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &publication, &reconstruction) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseLandmarkPage landmark_page = {0};
  Lardon3DSparseLandmark landmark_items[1];
  landmark_page.items = landmark_items;
  Lardon3DProjectDbResult page_result = lardon3d_sparse_landmark_list(
      db, reconstruction.reconstruction_id, 0, 1, &landmark_page);
  if (page_result != LARDON3D_PROJECT_DB_OK) {
    char page_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    (void)lardon3d_project_db_last_error(db, page_error);
    fprintf(stderr, "landmark page: %d %s\n", page_result, page_error);
  }
  CHECK(page_result == LARDON3D_PROJECT_DB_OK);
  CHECK(landmark_page.count == 1);
  CHECK(lardon3d_sparse_landmark_list(
            db, reconstruction.reconstruction_id, landmark_page.next_track_id,
            1, &landmark_page) == LARDON3D_PROJECT_DB_OK);
  CHECK(landmark_page.count == 1);
  CHECK(lardon3d_sparse_landmark_list(
            db, reconstruction.reconstruction_id, landmark_page.next_track_id,
            1, &landmark_page) == LARDON3D_PROJECT_DB_OK);
  CHECK(landmark_page.count == 0);
  CHECK(lardon3d_sparse_landmark_list(db, reconstruction.reconstruction_id, 0,
                                      LARDON3D_SPARSE_SFM_PAGE_MAX + 1,
                                      &landmark_page) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  Lardon3DSparseRegisteredImagePage camera_page = {0};
  Lardon3DSparseRegisteredImage camera_items[1];
  camera_page.items = camera_items;
  Lardon3DProjectDbResult camera_result = lardon3d_sparse_registered_image_list(
      db, reconstruction.reconstruction_id, 0, 1, &camera_page);
  if (camera_result != LARDON3D_PROJECT_DB_OK) {
    char camera_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    (void)lardon3d_project_db_last_error(db, camera_error);
    fprintf(stderr, "camera page: %d %s\n", camera_result, camera_error);
  }
  CHECK(camera_result == LARDON3D_PROJECT_DB_OK);
  CHECK(camera_page.count == 1 && camera_page.next_image_id == image_a);
  CHECK(lardon3d_sparse_registered_image_list(
            db, reconstruction.reconstruction_id, camera_page.next_image_id, 1,
            &camera_page) == LARDON3D_PROJECT_DB_OK);
  CHECK(camera_page.count == 1 && camera_page.next_image_id == image_b);

  Lardon3DSparseObservationPage observation_page = {0};
  Lardon3DSparseLandmarkObservation observation_items[1];
  observation_page.items = observation_items;
  CHECK(lardon3d_sparse_observation_list(db, reconstruction.reconstruction_id,
                                         0, 0, 1, &observation_page) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(observation_page.count == 1);
  CHECK(lardon3d_sparse_observation_list(
            db, reconstruction.reconstruction_id,
            observation_page.next_landmark_id,
            observation_page.next_position_in_track, 1,
            &observation_page) == LARDON3D_PROJECT_DB_OK);
  CHECK(observation_page.count == 1);

  Lardon3DSparseComponentPage component_page = {0};
  Lardon3DSparseComponent component_items[1];
  component_page.items = component_items;
  CHECK(lardon3d_sparse_component_list(db, reconstruction.reconstruction_id, 0,
                                       1, &component_page) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(component_page.count == 1 &&
        component_page.next_component_key == image_a);

  Lardon3DSparseCalibration invalid = calibration;
  invalid.width = 0;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid = calibration;
  invalid.height = 0;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid = calibration;
  invalid.fx = 0.0;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid = calibration;
  invalid.fy = -1.0;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid = calibration;
  invalid.model_kind = 99;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid = calibration;
  invalid.fx = NAN;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid = calibration;
  invalid.fy = INFINITY;
  CHECK(lardon3d_sparse_calibration_create(db, &invalid, &duplicate) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  const char *lengths[] = {"0", "1", "31", "33", "64"};
  for (size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
    char sql[512];
    (void)snprintf(
        sql, sizeof(sql),
        "INSERT INTO sparse_calibrations "
        "(scientific_hash,model_kind,model_version,width,height,fx,fy,cx,cy,"
        "k1,k2,p1,p2,provenance_kind,provenance_fingerprint) VALUES "
        "(randomblob(32),1,1,4000,3000,2000,2000,2000,1500,0,0,0,0,1,"
        "zeroblob(%s))",
        lengths[index]);
    CHECK(raw_constraint(path, sql));
    (void)snprintf(
        sql, sizeof(sql),
        "INSERT INTO sparse_calibration_scopes "
        "(scientific_hash,member_count) VALUES(randomblob(%s),1)",
        lengths[index]);
    CHECK(raw_constraint(path, sql));
  }
  lardon3d_project_db_close(db);
  db = NULL;
  for (size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
    char sql[512];
    (void)snprintf(
        sql, sizeof(sql),
        "INSERT INTO sparse_reconstructions "
        "(track_set_id,calibration_scope_id,sfm_kind,sfm_version,"
        "parameter_fingerprint,component_count,registered_image_count,"
        "landmark_count,reprojection_rmse_px,reprojection_median_px,created_at)"
        " VALUES(%llu,%llu,1,1,zeroblob(%s),1,2,1,0.1,0.1,0)",
        (unsigned long long)published_tracks.track_set_id,
        (unsigned long long)scope.scope_id, lengths[index]);
    CHECK(raw_constraint(path, sql));
  }
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(path,
                 "UPDATE sparse_calibration_scopes SET member_count=1") ==
        SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationScope corrupt_scope;
  CHECK(lardon3d_sparse_calibration_scope_load(db, scope.scope_id,
                                               &corrupt_scope) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(path,
                 "UPDATE sparse_calibration_scopes SET member_count=2") ==
        SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseReconstruction reopened;
  CHECK(
      lardon3d_sparse_reconstruction_load(db, reconstruction.reconstruction_id,
                                          &reopened) == LARDON3D_PROJECT_DB_OK);
  CHECK(reopened.landmark_count == reconstruction.landmark_count);
  for (size_t index = 0; index < loaded_count; ++index)
    lardon3d_project_db_free_track(&loaded_tracks[index]);
  lardon3d_project_db_close(db);
  db = NULL;
  char corrupt_calibration_sql[128];
  (void)snprintf(corrupt_calibration_sql, sizeof(corrupt_calibration_sql),
                 "PRAGMA ignore_check_constraints=ON;UPDATE "
                 "sparse_calibrations SET width=0 WHERE calibration_id=%llu",
                 (unsigned long long)alternate_stored.calibration_id);
  CHECK(raw_exec(path, corrupt_calibration_sql) == SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationMember scope_page_items[2];
  size_t scope_page_count = 0;
  uint64_t scope_page_cursor = 0;
  CHECK(lardon3d_sparse_calibration_scope_list_members(
            db, page_scope.scope_id, 0, scope_page_items, 2, &scope_page_count,
            &scope_page_cursor) == LARDON3D_PROJECT_DB_CORRUPT);
  CHECK(scope_page_count == 0 && scope_page_cursor == 0);
  lardon3d_project_db_close(db);
  unlink(path);
  return true;
}

static bool register_fixture_feature_set(Lardon3DProjectDb *db, uint64_t image_id,
                                         unsigned char seed,
                                         Lardon3DProjectDbFeatureSet *output) {
  unsigned char parameter_hash[32] = {11};
  unsigned char source_hash[32] = {0};
  unsigned char asset_hash[32] = {0};
  char path[128];
  char hex[65];
  static const char digits[] = "0123456789abcdef";
  source_hash[0] = seed;
  asset_hash[0] = seed;
  for (size_t index = 0; index < 32; ++index) {
    hex[index * 2] = digits[asset_hash[index] >> 4];
    hex[index * 2 + 1] = digits[asset_hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, sizeof(path), "assets/features/%c%c/%s", hex[0], hex[1],
                 hex);
  return lardon3d_project_db_register_feature_set(
             db, image_id, "synthetic", 1, parameter_hash, source_hash, 3, 1,
             32, asset_hash, path, 64, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0,
             1, output) == LARDON3D_PROJECT_DB_OK;
}

static bool page_fixture(Lardon3DProjectDb *db, uint64_t reconstruction_id,
                         const uint64_t image_ids[4], const uint64_t track_ids[4],
                         const uint64_t feature_set_ids[4]) {
  Lardon3DSparseComponentPage components = {0};
  Lardon3DSparseComponent component_item;
  components.items = &component_item;
  uint64_t after_component = 0;
  size_t component_index = 0;
  for (;;) {
    CHECK(lardon3d_sparse_component_list(db, reconstruction_id,
                                         after_component, 1, &components) ==
          LARDON3D_PROJECT_DB_OK);
    if (components.count == 0)
      break;
    CHECK(component_index < 2);
    CHECK(component_item.component_key == image_ids[component_index * 2]);
    CHECK(component_item.registered_image_count == 2);
    CHECK(component_item.landmark_count == 2);
    after_component = components.next_component_key;
    ++component_index;
  }
  CHECK(component_index == 2);

  Lardon3DSparseRegisteredImagePage cameras = {0};
  Lardon3DSparseRegisteredImage camera_item;
  cameras.items = &camera_item;
  uint64_t after_image = 0;
  size_t image_index = 0;
  for (;;) {
    CHECK(lardon3d_sparse_registered_image_list(
              db, reconstruction_id, after_image, 1, &cameras) ==
          LARDON3D_PROJECT_DB_OK);
    if (cameras.count == 0)
      break;
    CHECK(image_index < 4);
    CHECK(camera_item.image_id == image_ids[image_index]);
    CHECK(camera_item.component_key == image_ids[image_index < 2 ? 0 : 2]);
    after_image = cameras.next_image_id;
    ++image_index;
  }
  CHECK(image_index == 4);

  Lardon3DSparseLandmarkPage landmarks = {0};
  Lardon3DSparseLandmark landmark_item;
  landmarks.items = &landmark_item;
  uint64_t after_track = 0;
  size_t landmark_index = 0;
  for (;;) {
    CHECK(lardon3d_sparse_landmark_list(db, reconstruction_id, after_track, 1,
                                        &landmarks) == LARDON3D_PROJECT_DB_OK);
    if (landmarks.count == 0)
      break;
    CHECK(landmark_index < 4);
    CHECK(landmark_item.track_id == track_ids[landmark_index]);
    CHECK(landmark_item.component_key == image_ids[landmark_index < 2 ? 0 : 2]);
    CHECK(landmark_item.observation_count == 2);
    after_track = landmarks.next_track_id;
    ++landmark_index;
  }
  CHECK(landmark_index == 4);

  Lardon3DSparseObservationPage observations = {0};
  Lardon3DSparseLandmarkObservation observation_item;
  observations.items = &observation_item;
  uint64_t after_landmark = 0;
  uint32_t after_position = 0;
  size_t observation_index = 0;
  for (;;) {
    CHECK(lardon3d_sparse_observation_list(
              db, reconstruction_id, after_landmark, after_position, 1,
              &observations) == LARDON3D_PROJECT_DB_OK);
    if (observations.count == 0)
      break;
    CHECK(observation_index < 8);
    CHECK(observation_item.track_id == track_ids[observation_index / 2]);
    CHECK(observation_item.feature_set_id ==
          feature_set_ids[(observation_index / 2) < 2 ?
                              observation_index % 2 :
                              2 + observation_index % 2]);
    CHECK(observation_item.position_in_track == observation_index % 2);
    after_landmark = observations.next_landmark_id;
    after_position = observations.next_position_in_track;
    ++observation_index;
  }
  CHECK(observation_index == 8);
  return true;
}

static bool run_multi_component_test(void) {
  char path[] = "/tmp/lardon3d-sparse-components-XXXXXX";
  int descriptor = mkstemp(path);
  CHECK(descriptor >= 0);
  close(descriptor);
  unlink(path);

  Lardon3DProjectDb *db = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_project_db_create_scanset(db, "two components", &scanset) ==
        LARDON3D_PROJECT_DB_OK);
  uint64_t image_ids[4] = {0};
  Lardon3DProjectDbFeatureSet features[4];
  for (size_t index = 0; index < 4; ++index) {
    CHECK(register_image(db, scanset.scanset_id, (unsigned char)(20 + index),
                         &image_ids[index]));
    CHECK(register_fixture_feature_set(db, image_ids[index],
                                       (unsigned char)(20 + index),
                                       &features[index]));
  }

  Lardon3DSparseCalibration calibration = {0};
  calibration.model_kind = LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE;
  calibration.model_version = LARDON3D_SPARSE_SFM_CALIBRATION_VERSION;
  calibration.width = 4000;
  calibration.height = 3000;
  calibration.fx = 2000.0;
  calibration.fy = 2000.0;
  calibration.cx = 2000.0;
  calibration.cy = 1500.0;
  calibration.provenance_kind = LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT;
  Lardon3DSparseCalibration stored;
  CHECK(lardon3d_sparse_calibration_create(db, &calibration, &stored) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationMember members[4];
  for (size_t index = 0; index < 4; ++index) {
    members[index].image_id = image_ids[index];
    members[index].calibration_id = stored.calibration_id;
    memcpy(members[index].calibration_hash, stored.scientific_hash, 32);
  }
  Lardon3DSparseCalibrationScope scope;
  CHECK(lardon3d_sparse_calibration_scope_create(db, members, 4, &scope) ==
        LARDON3D_PROJECT_DB_OK);

  Lardon3DProjectDbTrackObservation track_observations[8];
  Lardon3DProjectDbTrack tracks[4];
  for (size_t track = 0; track < 4; ++track) {
    size_t first = track * 2;
    size_t first_image = track < 2 ? 0 : 2;
    track_observations[first] = (Lardon3DProjectDbTrackObservation){
        .feature_set_id = features[first_image].feature_set_id,
        .feature_index = (uint32_t)(track % 2 + 1), .position_in_track = 0};
    track_observations[first + 1] = (Lardon3DProjectDbTrackObservation){
        .feature_set_id = features[first_image + 1].feature_set_id,
        .feature_index = (uint32_t)(track % 2 + 1), .position_in_track = 1};
    tracks[track] = (Lardon3DProjectDbTrack){
        .observation_count = 2, .observations = &track_observations[first]};
  }
  Lardon3DProjectDbTrackSet configuration = {0};
  (void)snprintf(configuration.builder_kind, sizeof(configuration.builder_kind),
                 "synthetic_components");
  configuration.builder_version = 1;
  configuration.parameter_fingerprint[0] = 31;
  configuration.verifier_kind = 1;
  configuration.verifier_version = 1;
  configuration.verifier_fingerprint[0] = 32;
  configuration.input_scope_hash[0] = 33;
  configuration.gvr_count = 1;
  configuration.track_count = 4;
  Lardon3DProjectDbTrackSet track_set;
  Lardon3DProjectDbResult track_result = lardon3d_project_db_create_track_set(
      db, &configuration, tracks, 4, &track_set);
  if (track_result != LARDON3D_PROJECT_DB_OK) {
    char track_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    (void)lardon3d_project_db_last_error(db, track_error);
    fprintf(stderr, "multi-component track set: %d %s\n", track_result,
            track_error);
  }
  CHECK(track_result == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbTrack loaded_tracks[4] = {0};
  size_t loaded_count = 0;
  CHECK(lardon3d_project_db_list_tracks(db, track_set.track_set_id, 0,
                                        loaded_tracks, 4, &loaded_count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_count == 4);
  uint64_t track_ids[4];
  for (size_t index = 0; index < 4; ++index)
    track_ids[index] = loaded_tracks[index].track_id;
  for (size_t index = 0; index < 4; ++index)
    lardon3d_project_db_free_track(&loaded_tracks[index]);

  Lardon3DSparseComponent components[2] = {
      {.component_key = image_ids[0], .registered_image_count = 2,
       .landmark_count = 2},
      {.component_key = image_ids[2], .registered_image_count = 2,
       .landmark_count = 2}};
  Lardon3DSparseRegisteredImage poses[4];
  for (size_t index = 0; index < 4; ++index) {
    poses[index] = (Lardon3DSparseRegisteredImage){
        .image_id = image_ids[index],
        .component_key = image_ids[index < 2 ? 0 : 2],
        .rotation_cw = {1, 0, 0, 0, 1, 0, 0, 0, 1},
        .translation_cw = {(double)index, 0, 0}};
  }
  Lardon3DSparseLandmark landmarks[4];
  for (size_t index = 0; index < 4; ++index)
    landmarks[index] = (Lardon3DSparseLandmark){
        .track_id = track_ids[index],
        .component_key = image_ids[index < 2 ? 0 : 2],
        .x = (double)index,
        .y = 0,
        .z = 4,
        .reprojection_rmse_px = 0.1,
        .reprojection_median_px = 0.1,
        .observation_count = 2};
  Lardon3DSparseLandmarkObservation observations[8];
  for (size_t index = 0; index < 8; ++index)
    observations[index] = (Lardon3DSparseLandmarkObservation){
        .track_id = track_ids[index / 2],
        .feature_set_id = features[index / 2 < 2 ? index / 2 : 2 + index / 2 - 2]
                              .feature_set_id,
        .feature_index = (uint32_t)(index / 2 % 2 + 1),
        .position_in_track = (uint32_t)(index % 2)};
  for (size_t index = 0; index < 8; ++index)
    observations[index].feature_set_id =
        features[(index / 2 < 2 ? 0 : 2) + index % 2].feature_set_id;

  Lardon3DSparsePublication publication = {
      .track_set_id = track_set.track_set_id,
      .calibration_scope_id = scope.scope_id,
      .sfm_kind = LARDON3D_SPARSE_SFM_KIND_INCREMENTAL,
      .sfm_version = LARDON3D_SPARSE_SFM_VERSION,
      .components = components,
      .component_count = 2,
      .registered_images = poses,
      .registered_image_count = 4,
      .landmarks = landmarks,
      .landmark_count = 4,
      .observations = observations,
      .observation_count = 8,
      .reprojection_rmse_px = 0.1,
      .reprojection_median_px = 0.1};
  publication.parameter_fingerprint[0] = 34;
  Lardon3DSparseReconstruction reconstruction;
  CHECK(lardon3d_sparse_reconstruction_publish(db, &publication,
                                               &reconstruction) ==
        LARDON3D_PROJECT_DB_OK);
  publication.parameter_fingerprint[0] = 47;
  Lardon3DSparseReconstruction deleted_reconstruction;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &publication, &deleted_reconstruction) ==
        LARDON3D_PROJECT_DB_OK);
  publication.parameter_fingerprint[0] = 34;
  uint64_t deleted_id = deleted_reconstruction.reconstruction_id;
  lardon3d_project_db_close(db);
  db = NULL;
  char lifecycle_sql[256];
  (void)snprintf(lifecycle_sql, sizeof(lifecycle_sql),
                 "DELETE FROM sparse_reconstructions WHERE reconstruction_id=%llu",
                 (unsigned long long)deleted_id);
  CHECK(raw_exec(path, lifecycle_sql) == SQLITE_OK);
  (void)snprintf(lifecycle_sql, sizeof(lifecycle_sql),
                 "SELECT COUNT(*) FROM sparse_reconstructions "
                 "WHERE reconstruction_id=%llu",
                 (unsigned long long)deleted_id);
  CHECK(raw_scalar_equals(path, lifecycle_sql, 0));
  (void)snprintf(lifecycle_sql, sizeof(lifecycle_sql),
                 "SELECT COUNT(*) FROM sparse_reconstruction_components "
                 "WHERE reconstruction_id=%llu",
                 (unsigned long long)deleted_id);
  CHECK(raw_scalar_equals(path, lifecycle_sql, 0));
  (void)snprintf(lifecycle_sql, sizeof(lifecycle_sql),
                 "SELECT COUNT(*) FROM sparse_registered_images "
                 "WHERE reconstruction_id=%llu",
                 (unsigned long long)deleted_id);
  CHECK(raw_scalar_equals(path, lifecycle_sql, 0));
  (void)snprintf(lifecycle_sql, sizeof(lifecycle_sql),
                 "SELECT COUNT(*) FROM sparse_landmarks "
                 "WHERE reconstruction_id=%llu",
                 (unsigned long long)deleted_id);
  CHECK(raw_scalar_equals(path, lifecycle_sql, 0));
  (void)snprintf(lifecycle_sql, sizeof(lifecycle_sql),
                 "SELECT COUNT(*) FROM sparse_landmark_observations AS o "
                 "JOIN sparse_landmarks AS l ON l.landmark_id=o.landmark_id "
                 "WHERE l.reconstruction_id=%llu",
                 (unsigned long long)deleted_id);
  CHECK(raw_scalar_equals(path, lifecycle_sql, 0));
  CHECK(raw_scalar_equals(
            path,
            "SELECT COUNT(*) FROM sparse_reconstruction_components AS c "
            "WHERE NOT EXISTS (SELECT 1 FROM sparse_reconstructions AS r "
            "WHERE r.reconstruction_id=c.reconstruction_id)",
            0));
  CHECK(raw_scalar_equals(
            path,
            "SELECT COUNT(*) FROM sparse_registered_images AS p WHERE "
            "NOT EXISTS (SELECT 1 FROM sparse_reconstructions AS r WHERE "
            "r.reconstruction_id=p.reconstruction_id) OR NOT EXISTS "
            "(SELECT 1 FROM sparse_reconstruction_components AS c WHERE "
            "c.reconstruction_id=p.reconstruction_id AND c.component_id="
            "p.component_id)",
            0));
  CHECK(raw_scalar_equals(
            path,
            "SELECT COUNT(*) FROM sparse_landmarks AS l WHERE NOT EXISTS "
            "(SELECT 1 FROM sparse_reconstructions AS r WHERE "
            "r.reconstruction_id=l.reconstruction_id) OR NOT EXISTS "
            "(SELECT 1 FROM sparse_reconstruction_components AS c WHERE "
            "c.reconstruction_id=l.reconstruction_id AND c.component_id="
            "l.component_id)",
            0));
  CHECK(raw_scalar_equals(
            path,
            "SELECT COUNT(*) FROM sparse_landmark_observations AS o WHERE "
            "NOT EXISTS (SELECT 1 FROM sparse_landmarks AS l WHERE "
            "l.landmark_id=o.landmark_id)",
            0));
  CHECK(raw_constraint(path, "DELETE FROM tracks WHERE track_id=1"));
  CHECK(raw_constraint(path, "DELETE FROM track_sets WHERE track_set_id=1"));
  CHECK(raw_constraint(path, "DELETE FROM images WHERE image_id=1"));
  CHECK(raw_constraint(path, "DELETE FROM feature_sets WHERE feature_set_id=1"));
  CHECK(raw_constraint(path, "DELETE FROM sparse_calibrations WHERE calibration_id=1"));
  CHECK(raw_constraint(path, "DELETE FROM sparse_calibration_scopes WHERE scope_id=1"));
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibration surviving_calibration;
  CHECK(lardon3d_sparse_calibration_load(db, stored.calibration_id,
                                         &surviving_calibration) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationScope surviving_scope;
  CHECK(lardon3d_sparse_calibration_scope_load(db, scope.scope_id,
                                               &surviving_scope) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbTrackSet surviving_track_set;
  CHECK(lardon3d_project_db_load_track_set(db, track_set.track_set_id,
                                           &surviving_track_set) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_sparse_reconstruction_load(db, reconstruction.reconstruction_id,
                                            &reconstruction) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseRegisteredImage wrong_component_poses[4];
  memcpy(wrong_component_poses, poses, sizeof(wrong_component_poses));
  wrong_component_poses[0].component_key = image_ids[2];
  Lardon3DSparsePublication invalid_publication = publication;
  invalid_publication.registered_images = wrong_component_poses;
  invalid_publication.parameter_fingerprint[0] = 39;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  Lardon3DSparseLandmark wrong_component_landmarks[4];
  memcpy(wrong_component_landmarks, landmarks, sizeof(wrong_component_landmarks));
  wrong_component_landmarks[0].component_key = image_ids[2];
  invalid_publication = publication;
  invalid_publication.landmarks = wrong_component_landmarks;
  invalid_publication.parameter_fingerprint[0] = 40;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  memcpy(wrong_component_poses, poses, sizeof(wrong_component_poses));
  wrong_component_poses[1].image_id = image_ids[0];
  invalid_publication = publication;
  invalid_publication.registered_images = wrong_component_poses;
  invalid_publication.parameter_fingerprint[0] = 41;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid_publication = publication;
  invalid_publication.track_set_id = track_set.track_set_id + 1000;
  invalid_publication.parameter_fingerprint[0] = 42;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  Lardon3DSparseLandmark duplicate_landmarks[4];
  memcpy(duplicate_landmarks, landmarks, sizeof(duplicate_landmarks));
  duplicate_landmarks[1].track_id = duplicate_landmarks[0].track_id;
  invalid_publication = publication;
  invalid_publication.landmarks = duplicate_landmarks;
  invalid_publication.parameter_fingerprint[0] = 43;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  Lardon3DSparseLandmarkObservation invalid_observations[8];
  memcpy(invalid_observations, observations, sizeof(invalid_observations));
  invalid_observations[0].track_id = track_ids[1];
  invalid_publication = publication;
  invalid_publication.observations = invalid_observations;
  invalid_publication.parameter_fingerprint[0] = 44;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) !=
        LARDON3D_PROJECT_DB_OK);
  memcpy(invalid_observations, observations, sizeof(invalid_observations));
  invalid_observations[0].feature_index = 99;
  invalid_publication.parameter_fingerprint[0] = 45;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) !=
        LARDON3D_PROJECT_DB_OK);
  memcpy(invalid_observations, observations, sizeof(invalid_observations));
  invalid_observations[1] = invalid_observations[0];
  invalid_publication.parameter_fingerprint[0] = 46;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_publication, &reconstruction) !=
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseRegisteredImage invalid_pose = poses[0];
  invalid_pose.rotation_cw[0] = 2.0;
  Lardon3DSparsePublication invalid_pose_publication = publication;
  invalid_pose_publication.registered_images = &invalid_pose;
  invalid_pose_publication.registered_image_count = 1;
  invalid_pose_publication.parameter_fingerprint[0] = 37;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_pose_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid_pose = poses[0];
  invalid_pose.translation_cw[0] = NAN;
  invalid_pose_publication.registered_images = &invalid_pose;
  invalid_pose_publication.parameter_fingerprint[0] = 38;
  CHECK(lardon3d_sparse_reconstruction_publish(
            db, &invalid_pose_publication, &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(reconstruction.component_count == 2);
  CHECK(reconstruction.registered_image_count == 4);
  CHECK(reconstruction.landmark_count == 4);
  CHECK(page_fixture(db, reconstruction.reconstruction_id, image_ids, track_ids,
                     (uint64_t[4]){features[0].feature_set_id,
                                   features[1].feature_set_id,
                                   features[2].feature_set_id,
                                   features[3].feature_set_id}));

  Lardon3DSparseComponent unsorted_components[2] = {components[1], components[0]};
  Lardon3DSparsePublication unsorted_publication = publication;
  unsorted_publication.components = unsorted_components;
  unsorted_publication.parameter_fingerprint[0] = 36;
  CHECK(lardon3d_sparse_reconstruction_publish(db, &unsorted_publication,
                                               &reconstruction) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  Lardon3DSparseLandmarkObservation invalid = observations[0];
  invalid.feature_set_id = features[2].feature_set_id;
  invalid_publication = publication;
  invalid_publication.observations = &invalid;
  invalid_publication.observation_count = 1;
  invalid_publication.parameter_fingerprint[0] = 35;
  CHECK(lardon3d_sparse_reconstruction_publish(db, &invalid_publication,
                                               &reconstruction) !=
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseReconstructionPage reconstruction_page = {0};
  Lardon3DSparseReconstruction reconstruction_items[2];
  reconstruction_page.items = reconstruction_items;
  CHECK(lardon3d_sparse_reconstruction_list(db, 0, 2, &reconstruction_page) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(reconstruction_page.count == 1);

  uint64_t reconstruction_id = reconstruction.reconstruction_id;
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseReconstruction reopened;
  CHECK(lardon3d_sparse_reconstruction_load(db, reconstruction_id, &reopened) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(reopened.component_count == 2 && reopened.registered_image_count == 4 &&
        reopened.landmark_count == 4);
  CHECK(page_fixture(db, reconstruction_id, image_ids, track_ids,
                     (uint64_t[4]){features[0].feature_set_id,
                                   features[1].feature_set_id,
                                   features[2].feature_set_id,
                                   features[3].feature_set_id}));
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(
            path,
            "PRAGMA ignore_check_constraints=ON;"
            "UPDATE sparse_reconstructions SET sfm_kind=99") == SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_sparse_reconstruction_load(db, reconstruction_id, &reopened) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(
            path,
            "PRAGMA ignore_check_constraints=ON;"
            "UPDATE sparse_reconstructions SET sfm_kind=1") == SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_sparse_reconstruction_load(db, reconstruction_id, &reopened) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(
            path,
            "UPDATE sparse_registered_images SET component_id="
            "(SELECT component_id FROM sparse_reconstruction_components "
            "WHERE reconstruction_id=(SELECT reconstruction_id FROM "
            "sparse_registered_images LIMIT 1) ORDER BY component_key DESC "
            "LIMIT 1) WHERE image_id=(SELECT MIN(image_id) FROM "
            "sparse_registered_images)") == SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseRegisteredImagePage wrong_component_camera_page = {0};
  Lardon3DSparseRegisteredImage wrong_component_camera;
  wrong_component_camera_page.items = &wrong_component_camera;
  CHECK(lardon3d_sparse_registered_image_list(db, reconstruction_id, 0, 1,
                                               &wrong_component_camera_page) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  Lardon3DSparseComponentPage wrong_component_page = {0};
  Lardon3DSparseComponent wrong_component;
  wrong_component_page.items = &wrong_component;
  CHECK(lardon3d_sparse_component_list(db, reconstruction_id, 0, 1,
                                       &wrong_component_page) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(
            path,
            "UPDATE sparse_registered_images SET component_id="
            "(SELECT component_id FROM sparse_reconstruction_components "
            "WHERE reconstruction_id=(SELECT reconstruction_id FROM "
            "sparse_registered_images LIMIT 1) AND component_key="
            "(SELECT MIN(component_key) FROM sparse_reconstruction_components)) "
            "WHERE image_id=(SELECT MIN(image_id) FROM sparse_registered_images);"
            "UPDATE sparse_landmarks SET component_id="
            "(SELECT component_id FROM sparse_reconstruction_components "
            "WHERE reconstruction_id=(SELECT reconstruction_id FROM "
            "sparse_landmarks LIMIT 1) ORDER BY component_key DESC LIMIT 1) "
            "WHERE landmark_id=(SELECT MIN(landmark_id) FROM sparse_landmarks)") ==
        SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseLandmarkPage wrong_component_landmark_page = {0};
  Lardon3DSparseLandmark wrong_component_landmark;
  wrong_component_landmark_page.items = &wrong_component_landmark;
  CHECK(lardon3d_sparse_landmark_list(db, reconstruction_id, 0, 1,
                                      &wrong_component_landmark_page) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(
            path,
            "UPDATE sparse_landmarks SET component_id="
            "(SELECT component_id FROM sparse_reconstruction_components "
            "WHERE reconstruction_id=(SELECT reconstruction_id FROM "
            "sparse_landmarks LIMIT 1) AND component_key="
            "(SELECT MIN(f.image_id) FROM track_observations AS t JOIN "
            "feature_sets AS f ON f.feature_set_id=t.feature_set_id JOIN "
            "sparse_reconstructions AS r ON r.track_set_id=t.track_set_id "
            "WHERE r.reconstruction_id=(SELECT reconstruction_id FROM "
            "sparse_landmarks LIMIT 1) AND t.track_id=(SELECT track_id FROM "
            "sparse_landmarks ORDER BY landmark_id LIMIT 1))) WHERE "
            "landmark_id=(SELECT MIN(landmark_id) FROM sparse_landmarks)") ==
        SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(path,
                 "UPDATE sparse_registered_images SET rotation_00=2.0") ==
        SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseRegisteredImagePage corrupt_camera_page = {0};
  Lardon3DSparseRegisteredImage corrupt_camera;
  corrupt_camera_page.items = &corrupt_camera;
  CHECK(lardon3d_sparse_registered_image_list(db, reconstruction_id, 0, 1,
                                              &corrupt_camera_page) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(db);
  db = NULL;
  CHECK(raw_exec(
            path,
            "UPDATE sparse_registered_images SET rotation_00=1.0;"
            "UPDATE sparse_landmark_observations SET feature_set_id="
            "(SELECT feature_set_id FROM feature_sets ORDER BY feature_set_id "
            "LIMIT 1 OFFSET 2) "
            "WHERE landmark_id=(SELECT landmark_id FROM sparse_landmarks "
            "ORDER BY landmark_id LIMIT 1) AND position_in_track=0") ==
        SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseObservationPage corrupt_observation_page = {0};
  Lardon3DSparseLandmarkObservation corrupt_observation;
  corrupt_observation_page.items = &corrupt_observation;
  CHECK(lardon3d_sparse_observation_list(db, reconstruction_id, 0, 0, 1,
                                         &corrupt_observation_page) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(db);
  unlink(path);
  return true;
}

int main(void) {
  return run_test() && run_multi_component_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
