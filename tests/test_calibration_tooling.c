#include <sqlite3.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/calibration_tooling.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "tooling failure %d: %s\n", __LINE__, #x); return false; } } while (0)

static bool sql(const char *path, const char *text) {
  sqlite3 *db = NULL; if (sqlite3_open(path, &db) != SQLITE_OK) return false;
  int rc = sqlite3_exec(db, text, NULL, NULL, NULL); return sqlite3_close(db) == SQLITE_OK && rc == SQLITE_OK;
}
static bool scalar(const char *path, const char *text, int *value) {
  sqlite3 *db = NULL; sqlite3_stmt *statement = NULL;
  if (sqlite3_open(path, &db) != SQLITE_OK || sqlite3_prepare_v2(db, text, -1, &statement, NULL) != SQLITE_OK) {
    if (statement) sqlite3_finalize(statement);
    if (db) sqlite3_close(db);
    return false;
  }
  bool ok = sqlite3_step(statement) == SQLITE_ROW;
  if (ok) *value = sqlite3_column_int(statement, 0);
  return sqlite3_finalize(statement) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK && ok;
}
static void hashes(unsigned char value[32], unsigned char seed) { memset(value, seed, 32); }
static void params(double output[8]) { const double p[8] = {3000,3001,2000,1100,.01,-.01,.001,-.001}; memcpy(output,p,sizeof(p)); }

/* Synthetic only: these values model an already-completed external session and
 * never represent a physical calibration or a real project. */
static void fixture(Lardon3DCalibrationToolingEvidence *e, Lardon3DCalibrationToolingView views[60],
                    Lardon3DCalibrationToolingEntry entries[1], Lardon3DCalibrationToolingCoordinateCheck coordinates[1200]) {
  memset(e, 0, sizeof(*e)); memset(views, 0, sizeof(Lardon3DCalibrationToolingView)*60);
  memset(entries, 0, sizeof(Lardon3DCalibrationToolingEntry)); memset(coordinates, 0, sizeof(Lardon3DCalibrationToolingCoordinateCheck)*1200);
  hashes(e->target_sha256,1); hashes(e->optical_state_sha256,2); hashes(e->solver_executable_sha256,3); hashes(e->solver_configuration_sha256,4); hashes(e->initialization_evidence_sha256,5); hashes(e->validation_evidence_sha256,6);
  e->target_family=LARDON3D_CALIBRATION_TOOLING_TARGET_CHARUCO_9X7_DICT_5X5_100; e->target_squares_x=9; e->target_squares_y=7;
  e->target_square_length_mm=30; e->target_marker_length_mm=21; e->target_active_width_mm=270; e->target_active_height_mm=210; e->target_white_border_mm=30;
  for (size_t i=0;i<10;++i) e->target_measurements_mm[i]=30.0;
  e->measurement_resolution_mm=.1; e->target_flatness_mm=NAN; e->holdout_rmse_px=.4; e->holdout_maximum_residual_px=.8;
  for (size_t i=0;i<60;++i) {
    static const uint32_t quadrants[10]={0,0,1,1,2,2,3,3,4,4};
    static const uint32_t distances[10]={0,1,2,0,1,2,0,1,2,0};
    static const double angles[10]={25,40,55,25,40,55,25,40,55,25};
    size_t group=i/6; hashes(views[i].source_sha256,(unsigned char)(i+20)); views[i].accepted=1; views[i].holdout=(i%6)==4; views[i].quadrant=quadrants[group];
    views[i].distance_band=distances[group]; views[i].target_corner_quadrant_mask=7; views[i].corner_count=40; views[i].residual_count=40; views[i].target_occupancy=.4;
    views[i].normal_angle_degrees=angles[group]; views[i].distance_metres=1.0+(double)distances[group]*.3;
    views[i].corner_rms_px=.1; views[i].clipped_fraction=0; views[i].reprojection_rmse_px=.4; views[i].maximum_residual_px=.8;
  }
  entries[0].image_id=1; hashes(entries[0].representation_sha256,0x11); hashes(entries[0].optical_state_sha256,2); entries[0].width=4000; entries[0].height=2250;
  double p[8]; params(p); memcpy(&entries[0].fx,p,sizeof(p)); memcpy(&entries[0].fit_fx,p,sizeof(p));
  for (size_t repeat=0;repeat<3;++repeat) memcpy(entries[0].repeated_parameters[repeat],p,sizeof(p));
  entries[0].support_images=60; entries[0].support_observations=2400; entries[0].reprojection_rmse_px=.4; entries[0].maximum_parameter_delta=0; entries[0].validation_flags=15;
  for(size_t i=0;i<1200;++i) { memcpy(coordinates[i].source_sha256,views[i/20].source_sha256,32); coordinates[i].orientation_degrees=0; coordinates[i].dx_px=.001; coordinates[i].dy_px=-.001; }
  e->views=views; e->view_count=60; e->entries=entries; e->entry_count=1; e->coordinate_checks=coordinates; e->coordinate_check_count=1200;
}

static bool rejects_major_cases(void) {
  Lardon3DCalibrationToolingEvidence e; Lardon3DCalibrationToolingView v[60]; Lardon3DCalibrationToolingEntry x[1]; Lardon3DCalibrationToolingCoordinateCheck c[1200];
  fixture(&e,v,x,c); CHECK(lardon3d_calibration_tooling_validate(&e)==LARDON3D_CALIBRATION_TOOLING_OK);
  e.view_count=39; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); e.view_count=60;
  v[0].corner_count=1; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); v[0].corner_count=40;
  for (size_t i=0;i<3;++i) v[i].quadrant=4;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  for (size_t i=0;i<3;++i) v[i].quadrant=0;
  for (size_t i=0;i<9;++i) v[i*3].normal_angle_degrees=1;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  for (size_t i=0;i<9;++i) v[i*3].normal_angle_degrees=25;
  for (size_t i=0;i<7;++i) v[i*3].distance_band=1;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  for (size_t i=0;i<7;++i) v[i*3].distance_band=0;
  e.target_marker_length_mm=20; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); e.target_marker_length_mm=21;
  e.target_measurements_mm[0]=31; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); e.target_measurements_mm[0]=30;
  v[0].corner_rms_px=.3; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); v[0].corner_rms_px=.1;
  x[0].reprojection_rmse_px=.6; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); x[0].reprojection_rmse_px=.4;
  v[0].reprojection_rmse_px=.8; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); v[0].reprojection_rmse_px=.4;
  v[0].maximum_residual_px=2; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); v[0].maximum_residual_px=.8;
  v[0].high_residual_count=40; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); v[0].high_residual_count=0;
  e.holdout_rmse_px=.8; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); e.holdout_rmse_px=.4;
  x[0].repeated_parameters[1][0]+=1; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); x[0].repeated_parameters[1][0]-=1;
  x[0].maximum_parameter_delta=.2; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); x[0].maximum_parameter_delta=0;
  fixture(&e,v,x,c); for (size_t i=18;i<60;++i) v[i].normal_angle_degrees=0;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  fixture(&e,v,x,c); v[4].holdout=0;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  fixture(&e,v,x,c); v[0].corner_rms_px=-.1;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  fixture(&e,v,x,c); x[0].reprojection_rmse_px=-.1;
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  fixture(&e,v,x,c); memset(e.solver_configuration_sha256,0,32);
  CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  fixture(&e,v,x,c);
  v[0].accepted=0; v[0].rejection_reason=7; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); v[0].accepted=1; v[0].rejection_reason=0;
  memset(e.optical_state_sha256,0,32); CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); hashes(e.optical_state_sha256,2);
  e.coordinate_check_count=1199; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); e.coordinate_check_count=1200;
  x[0].width=0; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); x[0].width=4000;
  x[0].fx=0; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); x[0].fx=3000;
  x[0].cx=4000; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK); x[0].cx=2000;
  e.extra_distortion_coefficient_count=1; CHECK(lardon3d_calibration_tooling_validate(&e)!=LARDON3D_CALIBRATION_TOOLING_OK);
  return true;
}

static bool run(void) {
  CHECK(rejects_major_cases());
  Lardon3DCalibrationToolingEvidence e; Lardon3DCalibrationToolingView views[60]; Lardon3DCalibrationToolingEntry entries[1]; Lardon3DCalibrationToolingCoordinateCheck coords[1200]; fixture(&e,views,entries,coords);
  unsigned char a[292], b[292], ha[32], hb[32]; size_t na=0, nb=0;
  CHECK(lardon3d_calibration_tooling_produce(&e,a,sizeof(a),&na,ha)==LARDON3D_CALIBRATION_TOOLING_OK);
  CHECK(lardon3d_calibration_tooling_produce(&e,b,sizeof(b),&nb,hb)==LARDON3D_CALIBRATION_TOOLING_OK && na==292 && na==nb && memcmp(a,b,na)==0 && memcmp(ha,hb,32)==0);
  CHECK(lardon3d_calibration_tooling_produce(&e,b,291,&nb,hb)==LARDON3D_CALIBRATION_TOOLING_CAPACITY);
  char dir[]="/tmp/lardon3d-calibration-tooling-XXXXXX"; CHECK(mkdtemp(dir)); char path[512]; CHECK(snprintf(path,sizeof(path),"%s/project.db",dir)>0);
  Lardon3DProjectDb *db=NULL; char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]; CHECK(lardon3d_project_db_open(path,&db,error)==LARDON3D_PROJECT_DB_OK); lardon3d_project_db_close(db);
  CHECK(sql(path,"INSERT INTO scansets VALUES(1,'s',1,1); INSERT INTO tasks VALUES(1,'q','photo_quality.triage',1,5,5,100,1,0,0,0,0,1); INSERT INTO tasks VALUES(2,'c','acquisition_campaign.run',1,5,5,100,1,0,0,0,0,1); INSERT INTO photo_quality_triage_tasks VALUES(1,1,2,1,X'01'); INSERT INTO photo_quality_triage_results VALUES(1,1,0,1,0,1,0,100,100,100,100,1,1,0,0,1,1,0,'GOOD'); INSERT INTO acquisition_campaign_tasks VALUES(2,1,2,2,X'02'); INSERT INTO captures VALUES(1,1,1); INSERT INTO acquisition_campaign_captures VALUES(2,2,1); INSERT INTO image_assets VALUES(1,X'1111111111111111111111111111111111111111111111111111111111111111','assets/images/11/1111111111111111111111111111111111111111111111111111111111111111',1,1,1); INSERT INTO images VALUES(1,1,1,'a.jpg','/a.jpg',NULL,1); INSERT INTO capture_images VALUES(1,1);"));
  CHECK(lardon3d_project_db_open(path,&db,error)==LARDON3D_PROJECT_DB_OK); Lardon3DProjectDbSelectedExecutionItem item={.item_index=0,.quality_group_id=1,.campaign_group_id=2,.capture_id=1,.representation_source=LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE}; Lardon3DProjectDbSelectedExecution execution;
  CHECK(lardon3d_project_db_create_selected_execution(db,1,2,&item,1,1,&execution)==LARDON3D_PROJECT_DB_OK); CHECK(lardon3d_project_db_record_selected_representation(db,execution.execution_id,0,1,1)==LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbSelectedExecutionItem selected; Lardon3DProjectDbImage image; Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_project_db_load_selected_execution_item(db,execution.execution_id,0,&selected)==LARDON3D_PROJECT_DB_OK && selected.has_image && selected.image_id==1);
  CHECK(lardon3d_project_db_load_image(db,1,&image,&asset)==LARDON3D_PROJECT_DB_OK && memcmp(asset.sha256,entries[0].representation_sha256,32)==0);
  entries[0].fx=0;
  CHECK(lardon3d_calibration_tooling_import(db,execution.execution_id,&e,a,sizeof(a),&na,&(Lardon3DCalibrationBootstrapOutput){0})==LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED);
  entries[0].fx=3000;
  lardon3d_project_db_close(db);
  int count=0; CHECK(scalar(path,"SELECT COUNT(*) FROM sparse_calibrations",&count) && count==0);
  CHECK(scalar(path,"SELECT COUNT(*) FROM sparse_calibration_scopes",&count) && count==0);
  CHECK(lardon3d_project_db_open(path,&db,error)==LARDON3D_PROJECT_DB_OK);
  Lardon3DCalibrationBootstrapOutput output; CHECK(lardon3d_calibration_tooling_import(db,execution.execution_id,&e,a,sizeof(a),&na,&output)==LARDON3D_CALIBRATION_TOOLING_OK); uint64_t scope=output.scope.scope_id;
  CHECK(lardon3d_calibration_tooling_import(db,execution.execution_id,&e,a,sizeof(a),&na,&output)==LARDON3D_CALIBRATION_TOOLING_OK && output.scope.scope_id==scope);
  Lardon3DSparseCalibrationMember member; size_t member_count=0; uint64_t next=0;
  CHECK(lardon3d_sparse_calibration_scope_list_members(db,scope,0,&member,1,&member_count,&next)==LARDON3D_PROJECT_DB_OK && member_count==1 && next==1 && member.image_id==1);
  CHECK(lardon3d_project_db_load_selected_execution(db,execution.execution_id,&execution)==LARDON3D_PROJECT_DB_OK && execution.stage==LARDON3D_SELECTED_EXECUTION_READY && execution.calibration_scope_id==scope);
  lardon3d_project_db_close(db); CHECK(unlink(path)==0); CHECK(rmdir(dir)==0); return true;
}
int main(void) { return run()?EXIT_SUCCESS:EXIT_FAILURE; }
