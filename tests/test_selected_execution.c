#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/project_db.h>

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "selected execution failure at line %d: %s\n", __LINE__, #condition); \
  return false; } } while (0)

static bool sql(const char *path, const char *text) {
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) return false;
  int code = sqlite3_exec(connection, text, NULL, NULL, NULL);
  return sqlite3_close(connection) == SQLITE_OK && code == SQLITE_OK;
}

static bool seed_v21(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  /* The two group namespaces intentionally use different values. This fixture
     proves that v22 retains the explicit bridge rather than equating IDs. */
  return sql(path,
      "INSERT INTO scansets(scanset_id,name,created_at,updated_at) VALUES(1,'scope',1,1);"
      "INSERT INTO tasks VALUES(1,'quality','photo_quality.triage',1,5,5,100,1,0,0,0,0,1);"
      "INSERT INTO tasks VALUES(2,'campaign','acquisition_campaign.run',1,5,5,100,1,0,0,0,0,1);"
      "INSERT INTO photo_quality_triage_tasks VALUES(1,1,2,1,X'01');"
      "INSERT INTO photo_quality_triage_results VALUES(1,1,0,1,0,1,0,100,100,100,100,"
      "1.0,1.0,0.0,0.0,1.0,1.0,0.0,'GOOD');"
      "INSERT INTO acquisition_campaign_tasks VALUES(2,1,2,2,X'02');"
      "INSERT INTO captures(capture_id,scanset_id,created_at) VALUES(1,1,1);"
      "INSERT INTO acquisition_campaign_captures VALUES(2,2,1);"
      "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,created_at) "
      "VALUES(1,zeroblob(32),'assets/images/00/test',1,1,1);"
      "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,created_at) "
      "VALUES(2,randomblob(32),'assets/images/00/raw',1,1,1);"
      "INSERT INTO capture_assets(capture_id,asset_id,role) VALUES(1,2,1);"
      "INSERT INTO capture_source_assets(capture_id,asset_id,source_kind) VALUES(1,2,2);"
      "INSERT INTO images(image_id,scanset_id,asset_id,original_name,source_path,imported_at) "
      "VALUES(1,1,1,'selected.jpg','/source/selected.jpg',1);"
      "INSERT INTO capture_images VALUES(1,1);"
      "INSERT INTO sparse_calibrations VALUES(1,randomblob(32),1,1,100,100,50,50,50,50,0,0,0,0,"
      "2,randomblob(32));"
      "INSERT INTO sparse_calibration_scopes VALUES(1,randomblob(32),1);"
      "INSERT INTO sparse_calibration_scope_images VALUES(1,1,1);"
      /* This fixture is a true v21 database; future additive objects must not
         remain merely because it was generated from a temporary current DB. */
      "DROP TABLE capture_calibration_selections;"
      "DROP TABLE optical_calibration_profiles;"
      "DROP TABLE capture_optical_configurations;"
      "DROP INDEX acquisition_campaign_capture_identity_v23;"
      "DROP TABLE acquisition_campaign_group_optics;"
      "DROP TABLE optical_configurations;"
      "DROP TABLE lens_profile_aliases;DROP TABLE lens_profiles;"
      "DROP TABLE camera_body_aliases;DROP TABLE camera_body_profiles;"
      "DROP TABLE selected_execution_items;DROP TABLE selected_executions;"
      "UPDATE metadata SET value=21 WHERE key='schema_version';");
}

static bool run(void) {
  char directory[] = "/tmp/lardon3d-selected-XXXXXX";
  CHECK(mkdtemp(directory) != NULL);
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/project.db", directory) > 0);
  CHECK(seed_v21(path));

  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V22", "1", 1) == 0);
  Lardon3DProjectDbResult migration_result = lardon3d_project_db_open(path, &database, error);
  CHECK(migration_result != LARDON3D_PROJECT_DB_OK);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V22") == 0);
  CHECK(sql(path, "CREATE TEMP TABLE migration_check(value INTEGER);"));
  sqlite3 *raw = NULL;
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  sqlite3_stmt *query = NULL;
  CHECK(sqlite3_prepare_v2(raw, "SELECT value FROM metadata WHERE key='schema_version'", -1,
                           &query, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 21);
  sqlite3_finalize(query);
  CHECK(sqlite3_close(raw) == SQLITE_OK);

  migration_result = lardon3d_project_db_open(path, &database, error);
  if (migration_result != LARDON3D_PROJECT_DB_OK)
    fprintf(stderr, "retry migration result=%d error=%s\n", (int)migration_result, error);
  CHECK(migration_result == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) ==
        LARDON3D_PROJECT_DB_SCHEMA_VERSION);
  Lardon3DProjectDbSelectedExecutionItem item = {
      .item_index = 0,
      .quality_group_id = 1,
      .campaign_group_id = 2,
      .capture_id = 1,
      .representation_source = LARDON3D_SELECTED_REPRESENTATION_RAW_ASSET,
      .source_asset_id = 2};
  Lardon3DProjectDbSelectedExecution execution;
  Lardon3DProjectDbSelectedExecutionItem unattached = item;
  unattached.source_asset_id = 1;
  CHECK(lardon3d_project_db_create_selected_execution(database, 1, 2, &unattached, 1, 10,
                                                       &execution) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_create_selected_execution(database, 1, 2, &item, 1, 10,
                                                       &execution) == LARDON3D_PROJECT_DB_OK);
  CHECK(execution.execution_id > 0 && execution.stage ==
        LARDON3D_SELECTED_EXECUTION_REPRESENTATIONS && execution.next_item_index == 0);
  uint64_t execution_id = execution.execution_id;
  CHECK(lardon3d_project_db_create_selected_execution(database, 1, 2, &item, 1, 10,
                                                       &execution) == LARDON3D_PROJECT_DB_OK);
  CHECK(execution.execution_id == execution_id);
  Lardon3DProjectDbSelectedExecutionItem conflict = item;
  conflict.representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE;
  conflict.source_asset_id = 0;
  CHECK(lardon3d_project_db_create_selected_execution(database, 1, 2, &conflict, 1, 10,
                                                       &execution) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_record_selected_representation(database, execution_id, 0, 1, 1) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_record_selected_representation(database, execution_id, 0, 1, 1) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution_id, &execution) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(execution.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
        execution.next_item_index == 1 && !execution.has_calibration_scope);
  Lardon3DProjectDbSelectedExecutionItem loaded;
  CHECK(lardon3d_project_db_load_selected_execution_item(database, execution_id, 0, &loaded) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded.quality_group_id == 1 && loaded.campaign_group_id == 2 &&
        loaded.capture_id == 1 &&
        loaded.representation_source == LARDON3D_SELECTED_REPRESENTATION_RAW_ASSET &&
        loaded.source_asset_id == 2 && loaded.has_image && loaded.image_id == 1);
  CHECK(lardon3d_project_db_assign_selected_calibration_scope(database, execution_id, 1) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_assign_selected_calibration_scope(database, execution_id, 1) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution_id, &execution) ==
        LARDON3D_PROJECT_DB_OK && execution.stage == LARDON3D_SELECTED_EXECUTION_READY &&
        execution.has_calibration_scope && execution.calibration_scope_id == 1);
  CHECK(lardon3d_project_db_create_selected_execution(database, 1, 2, &item, 1, 10,
                                                       &execution) == LARDON3D_PROJECT_DB_OK &&
        execution.execution_id == execution_id &&
        execution.stage == LARDON3D_SELECTED_EXECUTION_READY);
  lardon3d_project_db_close(database);
  database = NULL;

  /* CHECK constraints are bypassed deliberately to verify wide-value and
     stage/cursor corruption rejection at the public read boundary. */
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "PRAGMA ignore_check_constraints=ON;"
                          "UPDATE selected_execution_items SET representation_source=1 "
                          "WHERE execution_id=1;", NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution_item(database, execution_id, 0, &loaded) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(database);
  CHECK(sql(path, "UPDATE selected_execution_items SET representation_source=2 "
                  "WHERE execution_id=1;"));

  /* Numeric prefixes in TEXT demonstrate why positive-value checks alone are
     insufficient: sqlite3_column_int64() would coerce these to valid IDs. */
  CHECK(sql(path, "UPDATE selected_executions SET calibration_scope_id='1x' "
                  "WHERE execution_id=1;"));
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution_id, &execution) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(database);
  CHECK(sql(path, "UPDATE selected_executions SET calibration_scope_id=1 "
                  "WHERE execution_id=1;"));

  CHECK(sql(path, "UPDATE selected_execution_items SET source_asset_id='2x' "
                  "WHERE execution_id=1;"));
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution_item(database, execution_id, 0, &loaded) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(database);
  CHECK(sql(path, "UPDATE selected_execution_items SET source_asset_id=2 "
                  "WHERE execution_id=1;"));

  CHECK(sql(path, "UPDATE selected_execution_items SET image_id='1x' "
                  "WHERE execution_id=1;"));
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution_item(database, execution_id, 0, &loaded) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(database);
  CHECK(sql(path, "UPDATE selected_execution_items SET image_id=1 "
                  "WHERE execution_id=1;"));

  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "PRAGMA ignore_check_constraints=ON;"
                          "UPDATE selected_executions SET next_item_index=4294967296 "
                          "WHERE execution_id=1;", NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK);
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution_id, &execution) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(database);
  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) { return run() ? EXIT_SUCCESS : EXIT_FAILURE; }
