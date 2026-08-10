#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/project_db.h>
#include <lardon3d/sparse_sfm_model.h>

static bool exec_sql(const char *path, const char *sql) {
  sqlite3 *connection = NULL;
  bool ok = sqlite3_open(path, &connection) == SQLITE_OK;
  if (ok)
    ok = sqlite3_exec(connection, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) ==
         SQLITE_OK;
  char *message = NULL;
  if (ok)
    ok = sqlite3_exec(connection, sql, NULL, NULL, &message) == SQLITE_OK;
  if (!ok && connection)
    fprintf(stderr, "resource SQL failed: %s\n",
            message ? message : sqlite3_errmsg(connection));
  sqlite3_free(message);
  if (connection)
    ok = sqlite3_close(connection) == SQLITE_OK && ok;
  return ok;
}

static bool seed_upstream(const char *path, uint64_t landmark_count) {
  char *sql = NULL;
  int required = snprintf(
      NULL, 0,
      "PRAGMA foreign_keys=ON;"
      "INSERT INTO scansets(scanset_id,name,created_at,updated_at) "
      "VALUES(1,'resource',1,1);"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,created_at) "
      "SELECT x,randomblob(32),printf('resource/image-%%d',x),1,1,1 FROM n;"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO images(image_id,scanset_id,asset_id,original_name,source_path,"
      "producer_task_id,imported_at) SELECT x,1,x,printf('image-%%d',x),"
      "printf('resource/image-%%d',x),NULL,1 FROM n;"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO feature_assets(feature_asset_id,sha256,path,size_bytes,durability,created_at) "
      "SELECT x,randomblob(32),printf('resource/feature-%%d',x),1,0,1 FROM n;"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO feature_sets(feature_set_id,image_id,feature_asset_id,extractor_kind,"
      "extractor_version,parameter_fingerprint,source_image_sha256,feature_count,"
      "descriptor_type,descriptor_dimension,producer_task_id,created_at) SELECT x,x,x,"
      "'resource',1,randomblob(32),randomblob(32),5,1,32,NULL,1 FROM n;"
      "INSERT INTO track_sets(track_set_id,builder_kind,builder_version,parameter_fingerprint,"
      "verifier_kind,verifier_version,verifier_fingerprint,input_scope_hash,gvr_count,"
      "track_count,created_at) VALUES(1,'resource',1,zeroblob(32),1,1,zeroblob(32),"
      "zeroblob(32),1,%llu,1);"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<%llu) "
      "INSERT INTO tracks(track_id,track_set_id,observation_count) SELECT x,1,5 FROM n;"
      "WITH RECURSIVE t(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM t WHERE x<%llu), "
      "p(x) AS (SELECT 0 UNION ALL SELECT x+1 FROM p WHERE x<4) "
      "INSERT INTO track_observations(track_set_id,track_id,feature_set_id,feature_index,"
      "position_in_track) SELECT 1,t.x,CASE WHEN t.x<=%llu THEN 1+((((t.x-1)%%50)*5+p.x)%%250) "
      "ELSE 251+((((t.x-1-%llu)%%50)*5+p.x)%%250) END,CASE WHEN t.x<=%llu "
      "THEN ((t.x-1)/50) ELSE ((t.x-1-%llu)/50) END,p.x "
      "FROM t CROSS JOIN p;",
      (unsigned long long)landmark_count,
      (unsigned long long)landmark_count,
      (unsigned long long)landmark_count,
      (unsigned long long)(landmark_count / 2),
      (unsigned long long)(landmark_count / 2),
      (unsigned long long)(landmark_count / 2),
      (unsigned long long)(landmark_count / 2));
  if (required < 0)
    return false;
  sql = malloc((size_t)required + 1);
  if (!sql)
    return false;
  (void)snprintf(
      sql, (size_t)required + 1,
      "PRAGMA foreign_keys=ON;"
      "INSERT INTO scansets(scanset_id,name,created_at,updated_at) VALUES(1,'resource',1,1);"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,created_at) "
      "SELECT x,randomblob(32),printf('resource/image-%%d',x),1,1,1 FROM n;"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO images(image_id,scanset_id,asset_id,original_name,source_path,producer_task_id,imported_at) "
      "SELECT x,1,x,printf('image-%%d',x),printf('resource/image-%%d',x),NULL,1 FROM n;"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO feature_assets(feature_asset_id,sha256,path,size_bytes,durability,created_at) "
      "SELECT x,randomblob(32),printf('resource/feature-%%d',x),1,0,1 FROM n;"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<500) "
      "INSERT INTO feature_sets(feature_set_id,image_id,feature_asset_id,"
      "extractor_kind,extractor_version,parameter_fingerprint,source_image_sha256,"
      "feature_count,descriptor_type,descriptor_dimension,producer_task_id,created_at) "
      "SELECT x,x,x,'resource',1,randomblob(32),randomblob(32),5,1,32,NULL,1 FROM n;"
      "INSERT INTO track_sets(track_set_id,builder_kind,builder_version,"
      "parameter_fingerprint,verifier_kind,verifier_version,"
      "verifier_fingerprint,input_scope_hash,gvr_count,track_count,created_at) "
      "VALUES(1,'resource',1,zeroblob(32),1,1,zeroblob(32),zeroblob(32),1,%llu,1);"
      "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<%llu) "
      "INSERT INTO tracks(track_id,track_set_id,observation_count) SELECT x,1,5 FROM n;"
      "WITH RECURSIVE t(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM t WHERE x<%llu),"
      "p(x) AS (SELECT 0 UNION ALL SELECT x+1 FROM p WHERE x<4) "
      "INSERT INTO track_observations(track_set_id,track_id,feature_set_id,"
      "feature_index,position_in_track) SELECT 1,t.x,CASE WHEN t.x<=%llu "
      "THEN 1+((((t.x-1)%%50)*5+p.x)%%250) ELSE "
      "251+((((t.x-1-%llu)%%50)*5+p.x)%%250) END,CASE WHEN t.x<=%llu "
      "THEN ((t.x-1)/50) ELSE ((t.x-1-%llu)/50) END,p.x FROM t CROSS JOIN p;",
      (unsigned long long)landmark_count,
      (unsigned long long)landmark_count,
      (unsigned long long)landmark_count,
      (unsigned long long)(landmark_count / 2),
      (unsigned long long)(landmark_count / 2),
      (unsigned long long)(landmark_count / 2),
      (unsigned long long)(landmark_count / 2));
  bool ok = exec_sql(path, sql);
  free(sql);
  return ok;
}

static bool publish_fixture(const char *path, uint64_t landmark_count) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *db = NULL;
  if (lardon3d_project_db_open(path, &db, error) != LARDON3D_PROJECT_DB_OK)
    return false;
  Lardon3DSparseCalibration calibration = {
      .model_kind = 1, .model_version = 1, .width = 4000, .height = 3000,
      .fx = 2000, .fy = 2000, .cx = 2000, .cy = 1500, .provenance_kind = 1};
  Lardon3DSparseCalibration stored;
  if (lardon3d_sparse_calibration_create(db, &calibration, &stored) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  Lardon3DSparseCalibrationMember *members = calloc(500, sizeof(*members));
  for (size_t index = 0; index < 500; ++index) {
    members[index].image_id = index + 1;
    members[index].calibration_id = stored.calibration_id;
    memcpy(members[index].calibration_hash, stored.scientific_hash, 32);
  }
  Lardon3DSparseCalibrationScope scope;
  bool ok = lardon3d_sparse_calibration_scope_create(db, members, 500, &scope) ==
            LARDON3D_PROJECT_DB_OK;
  free(members);
  if (!ok)
    fprintf(stderr, "scope or allocation failed\n");
  if (!ok)
    return false;
  Lardon3DSparseComponent components[2] = {
      {.component_key = 1, .registered_image_count = 250,
       .landmark_count = landmark_count / 2},
      {.component_key = 251, .registered_image_count = 250,
       .landmark_count = landmark_count / 2}};
  Lardon3DSparseRegisteredImage *poses = calloc(500, sizeof(*poses));
  Lardon3DSparseLandmark *landmarks = calloc(landmark_count, sizeof(*landmarks));
  Lardon3DSparseLandmarkObservation *observations =
      calloc(landmark_count * 5, sizeof(*observations));
  if (!poses || !landmarks || !observations)
    return false;
  for (size_t index = 0; index < 500; ++index)
    poses[index] = (Lardon3DSparseRegisteredImage){
        .image_id = index + 1,
        .component_key = index < 250 ? 1 : 251,
        .rotation_cw = {1, 0, 0, 0, 1, 0, 0, 0, 1},
        .translation_cw = {(double)index, 0, 0}};
  for (uint64_t track = 0; track < landmark_count; ++track) {
    size_t component_base = track < landmark_count / 2 ? 0 : 250;
    uint64_t local_track = track < landmark_count / 2
                               ? track
                               : track - landmark_count / 2;
    uint64_t component_key = component_base ? 251 : 1;
    landmarks[track] = (Lardon3DSparseLandmark){
        .track_id = track + 1, .component_key = component_key, .x = 0,
        .y = 0, .z = 1, .reprojection_rmse_px = 0.1,
        .reprojection_median_px = 0.1, .observation_count = 5};
    for (uint32_t position = 0; position < 5; ++position) {
      size_t offset = (size_t)track * 5 + position;
      uint64_t feature_set = (uint64_t)component_base +
                             ((local_track % 50) * 5 + position) % 250 + 1;
      observations[offset] = (Lardon3DSparseLandmarkObservation){
          .track_id = track + 1,
          .feature_set_id = feature_set,
          .feature_index = (uint32_t)(local_track / 50),
          .position_in_track = position};
    }
  }
  Lardon3DSparsePublication publication = {
      .track_set_id = 1,
      .calibration_scope_id = scope.scope_id,
      .sfm_kind = 1,
      .sfm_version = 1,
      .components = components,
      .component_count = 2,
      .registered_images = poses,
      .registered_image_count = 500,
      .landmarks = landmarks,
      .landmark_count = landmark_count,
      .observations = observations,
      .observation_count = landmark_count * 5,
      .reprojection_rmse_px = 0.1,
      .reprojection_median_px = 0.1,
      .created_at = 1};
  publication.parameter_fingerprint[0] = 91;
  Lardon3DSparseReconstruction reconstruction;
  Lardon3DProjectDbResult publication_result =
      lardon3d_sparse_reconstruction_publish(db, &publication, &reconstruction);
  ok = publication_result == LARDON3D_PROJECT_DB_OK;
  if (!ok) {
    char detail[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    (void)lardon3d_project_db_last_error(db, detail);
    fprintf(stderr, "publish result %d: %s\n", publication_result, detail);
  }
  if (ok)
    printf("reconstruction_id=%llu landmarks=%llu observations=%llu\n",
           (unsigned long long)reconstruction.reconstruction_id,
           (unsigned long long)landmark_count,
           (unsigned long long)(landmark_count * 5));
  free(poses);
  free(landmarks);
  free(observations);
  lardon3d_project_db_close(db);
  return ok;
}

static bool page_fixture(const char *path, uint64_t reconstruction_id,
                         unsigned passes) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *db = NULL;
  if (lardon3d_project_db_open(path, &db, error) != LARDON3D_PROJECT_DB_OK)
    return false;
  uint64_t totals[4] = {0};
  for (unsigned pass = 0; pass < passes; ++pass) {
    Lardon3DSparseComponent components_items[64];
    Lardon3DSparseComponentPage components = {.items = components_items};
    uint64_t cursor = 0;
    while (lardon3d_sparse_component_list(db, reconstruction_id, cursor, 64,
                                          &components) == LARDON3D_PROJECT_DB_OK &&
           components.count) {
      totals[0] += components.count;
      cursor = components.next_component_key;
    }
    Lardon3DSparseRegisteredImage cameras[64];
    Lardon3DSparseRegisteredImagePage camera_page = {.items = cameras};
    cursor = 0;
    while (lardon3d_sparse_registered_image_list(
               db, reconstruction_id, cursor, 64, &camera_page) ==
               LARDON3D_PROJECT_DB_OK &&
           camera_page.count) {
      totals[1] += camera_page.count;
      cursor = camera_page.next_image_id;
    }
    Lardon3DSparseLandmark landmarks[64];
    Lardon3DSparseLandmarkPage landmark_page = {.items = landmarks};
    cursor = 0;
    while (lardon3d_sparse_landmark_list(db, reconstruction_id, cursor, 64,
                                         &landmark_page) ==
               LARDON3D_PROJECT_DB_OK &&
           landmark_page.count) {
      totals[2] += landmark_page.count;
      cursor = landmark_page.next_track_id;
    }
    Lardon3DSparseLandmarkObservation observations[64];
    Lardon3DSparseObservationPage observation_page = {.items = observations};
    uint64_t landmark_cursor = 0;
    uint32_t position_cursor = 0;
    while (lardon3d_sparse_observation_list(
               db, reconstruction_id, landmark_cursor, position_cursor, 64,
               &observation_page) == LARDON3D_PROJECT_DB_OK &&
           observation_page.count) {
      totals[3] += observation_page.count;
      landmark_cursor = observation_page.next_landmark_id;
      position_cursor = observation_page.next_position_in_track;
    }
  }
  printf("passes=%u components=%llu cameras=%llu landmarks=%llu observations=%llu\n",
         passes, (unsigned long long)totals[0], (unsigned long long)totals[1],
         (unsigned long long)totals[2], (unsigned long long)totals[3]);
  lardon3d_project_db_close(db);
  return true;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  const char *mode = argv[1];
  const char *path = argv[2];
  if (strcmp(mode, "publish") == 0) {
    uint64_t landmarks = argc > 3 ? strtoull(argv[3], NULL, 10) : 100000;
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    Lardon3DProjectDb *database = NULL;
    if (lardon3d_project_db_open(path, &database, error) !=
            LARDON3D_PROJECT_DB_OK ||
        !database) {
      fprintf(stderr, "open resource database failed: %s\n", error);
      return 1;
    }
    lardon3d_project_db_close(database);
    if (!seed_upstream(path, landmarks)) {
      fprintf(stderr, "seed upstream failed\n");
      return 1;
    }
    if (!publish_fixture(path, landmarks)) {
      fprintf(stderr, "publish fixture failed\n");
      return 1;
    }
    return 0;
  }
  if (strcmp(mode, "page") == 0) {
    unsigned passes = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 5;
    return page_fixture(path, argc > 3 ? strtoull(argv[3], NULL, 10) : 1,
                        passes)
               ? 0
               : 1;
  }
  return 2;
}
