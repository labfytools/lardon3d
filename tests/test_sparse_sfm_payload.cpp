#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <lardon3d/project_db.h>
#include <lardon3d/incremental_reconstruction_task.h>
#include <lardon3d/sparse_sfm_task.h>
}

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      return 1;                                                                           \
    }                                                                                     \
  } while (false)

static bool same_double(double a, double b) {
  uint64_t bits_a = 0;
  uint64_t bits_b = 0;
  std::memcpy(&bits_a, &a, sizeof(bits_a));
  std::memcpy(&bits_b, &b, sizeof(bits_b));
  return bits_a == bits_b;
}

static bool same_parameters(const Lardon3DSparseIncrementalParameters &a,
                            const Lardon3DSparseIncrementalParameters &b) {
  return a.minimum_seed_tracks == b.minimum_seed_tracks &&
         a.minimum_seed_landmarks == b.minimum_seed_landmarks &&
         a.minimum_pnp_correspondences == b.minimum_pnp_correspondences &&
         a.maximum_seed_candidates == b.maximum_seed_candidates &&
         a.maximum_registration_rounds == b.maximum_registration_rounds &&
         a.maximum_landmarks_per_round == b.maximum_landmarks_per_round &&
         a.maximum_images == b.maximum_images &&
         a.maximum_observations == b.maximum_observations && a.maximum_tracks == b.maximum_tracks &&
         same_double(a.reprojection_threshold_px, b.reprojection_threshold_px) &&
         same_double(a.minimum_track_parallax_rad, b.minimum_track_parallax_rad) &&
         same_double(a.relative_pose.robust_threshold_px, b.relative_pose.robust_threshold_px) &&
         same_double(a.relative_pose.confidence, b.relative_pose.confidence) &&
         a.relative_pose.max_iterations == b.relative_pose.max_iterations &&
         a.relative_pose.minimum_inliers == b.relative_pose.minimum_inliers &&
         same_double(a.relative_pose.minimum_inlier_ratio,
                     b.relative_pose.minimum_inlier_ratio) &&
         same_double(a.relative_pose.minimum_parallax_rad, b.relative_pose.minimum_parallax_rad) &&
         same_double(a.relative_pose.minimum_cheirality_ratio,
                     b.relative_pose.minimum_cheirality_ratio) &&
         a.relative_pose.deterministic_seed == b.relative_pose.deterministic_seed &&
         same_double(a.pnp.reprojection_threshold_px, b.pnp.reprojection_threshold_px) &&
         same_double(a.pnp.confidence, b.pnp.confidence) &&
         a.pnp.max_iterations == b.pnp.max_iterations &&
         a.pnp.minimum_inliers == b.pnp.minimum_inliers &&
         same_double(a.pnp.minimum_inlier_ratio, b.pnp.minimum_inlier_ratio) &&
         a.pnp.deterministic_seed == b.pnp.deterministic_seed &&
         a.refinement.max_iterations == b.refinement.max_iterations &&
         same_double(a.refinement.convergence_tolerance, b.refinement.convergence_tolerance);
}

int main() {
  char directory[] = "/tmp/lardon3d-sfm-payload-XXXXXX";
  CHECK(mkdtemp(directory) != nullptr);
  std::string path = std::string(directory) + "/project.db";
  Lardon3DProjectDb *database = nullptr;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  CHECK(lardon3d_project_db_open(path.c_str(), &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 18);

  sqlite3 *raw = nullptr;
  CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  const char *fixture_sql =
      "INSERT INTO track_sets(track_set_id,builder_kind,builder_version,parameter_fingerprint,"
      "verifier_kind,verifier_version,verifier_fingerprint,input_scope_hash,gvr_count,track_count,"
      "created_at) VALUES(41,'track_builder',1,zeroblob(32),1,1,zeroblob(32),zeroblob(32),1,0,0);"
      "INSERT INTO sparse_calibration_scopes(scope_id,scientific_hash,member_count) "
      "VALUES(43,randomblob(32),1);"
      "INSERT INTO sparse_reconstructions(reconstruction_id,track_set_id,"
      "calibration_scope_id,sfm_kind,sfm_version,parameter_fingerprint,component_count,"
      "registered_image_count,landmark_count,reprojection_rmse_px,reprojection_median_px,"
      "created_at) VALUES(42,41,43,1,1,zeroblob(32),1,2,1,0,0,0);";
  CHECK(sqlite3_exec(raw, fixture_sql, nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(raw);

  const uint64_t values[] = {0, 1, static_cast<uint64_t>(INT64_MAX),
                             static_cast<uint64_t>(INT64_MAX) + 1, UINT64_MAX};
  for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
    Lardon3DSparseIncrementalParameters p{};
    CHECK(lardon3d_sparse_incremental_parameters_default(&p));
    p.maximum_observations = values[index];
    p.maximum_tracks = values[index];
    p.relative_pose.deterministic_seed = values[index];
    p.pnp.deterministic_seed = values[index];
    p.reprojection_threshold_px = 3.125;
    p.relative_pose.confidence = 0.9125;
    p.pnp.confidence = 0.8875;
    uint64_t id = 100 + index;
    Lardon3DTaskDurableSnapshot snapshot{};
    snapshot.id = id;
    std::snprintf(snapshot.name, sizeof(snapshot.name), "Sparse payload %zu", index);
    snapshot.saved_state = TASK_PENDING;
    snapshot.recovery_state = TASK_PENDING;
    snapshot.estimate = {134217728, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
    Lardon3DProjectDbSparseSfmTask payload{id, 41, 43, 1, 1, p};
    CHECK(lardon3d_project_db_record_sparse_sfm_task(
              database, &snapshot, LARDON3D_SPARSE_SFM_TASK_KIND,
              LARDON3D_SPARSE_SFM_TASK_KIND_VERSION, nullptr, &payload, 0) ==
          LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbSparseSfmTask loaded{};
    CHECK(lardon3d_project_db_load_sparse_sfm_task(database, id, &loaded) ==
          LARDON3D_PROJECT_DB_OK);
    CHECK(loaded.task_id == id && loaded.track_set_id == 41 &&
          loaded.calibration_scope_id == 43 && loaded.sfm_kind == 1 && loaded.sfm_version == 1);
    CHECK(same_parameters(payload.parameters, loaded.parameters));
  }
  Lardon3DSparseIncrementalParameters p{};
  CHECK(lardon3d_sparse_incremental_parameters_default(&p));
  Lardon3DTaskDurableSnapshot wrong{};
  wrong.id = 999;
  std::snprintf(wrong.name, sizeof(wrong.name), "Wrong kind");
  wrong.saved_state = TASK_PENDING;
  wrong.recovery_state = TASK_PENDING;
  wrong.estimate = {134217728, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DProjectDbSparseSfmTask payload{999, 41, 43, 1, 1, p};
  CHECK(lardon3d_project_db_record_sparse_sfm_task(database, &wrong, "other.run", 1, nullptr,
                                                   &payload, 0) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "PRAGMA ignore_check_constraints=ON; UPDATE sparse_sfm_tasks SET "
                          "maximum_tracks=zeroblob(7) WHERE task_id=100;",
                     nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(raw);
  Lardon3DProjectDbSparseSfmTask corrupt{};
  CHECK(lardon3d_project_db_load_sparse_sfm_task(database, 100, &corrupt) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "PRAGMA foreign_keys=ON; DELETE FROM tasks WHERE task_id=101;",
                     nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(raw);
  CHECK(lardon3d_project_db_load_sparse_sfm_task(database, 101, &corrupt) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);

  Lardon3DTaskDurableSnapshot h_snapshot{};
  h_snapshot.id = 200;
  std::snprintf(h_snapshot.name, sizeof(h_snapshot.name), "Phase H payload");
  h_snapshot.saved_state = TASK_PENDING;
  h_snapshot.recovery_state = TASK_PENDING;
  h_snapshot.estimate = {269484032, 0, 0, 0, 1, 1, 1, 0, 1,
                         LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DProjectDbIncrementalReconstructionTask h_payload{
      200, 42, 41, 43, 1, 1, {}};
  h_payload.parameter_fingerprint[0] = 77;
  CHECK(lardon3d_project_db_record_incremental_reconstruction_task(
            database, &h_snapshot, LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND,
            LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND_VERSION, nullptr,
            &h_payload, 0) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbIncrementalReconstructionTask loaded_h{};
  CHECK(lardon3d_project_db_load_incremental_reconstruction_task(
            database, 200, &loaded_h) == LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_h.task_id == 200 && loaded_h.base_reconstruction_id == 42 &&
        loaded_h.extension_track_set_id == 41 &&
        loaded_h.calibration_scope_id == 43 && loaded_h.incremental_kind == 1 &&
        loaded_h.incremental_version == 1 &&
        std::memcmp(loaded_h.parameter_fingerprint,
                    h_payload.parameter_fingerprint, 32) == 0);
  lardon3d_project_db_close(database);
  CHECK(unlink(path.c_str()) == 0);
  CHECK(rmdir(directory) == 0);
  return 0;
}
