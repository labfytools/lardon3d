#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/optical_profiles.h>
#include <lardon3d/project_db.h>
#include <lardon3d/sparse_sfm_model.h>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "optical profiles failure at line %d: %s\n", __LINE__, \
              #condition);                                                      \
      return false;                                                             \
    }                                                                           \
  } while (0)

static bool raw_sql(const char *path, const char *sql) {
  sqlite3 *database = NULL;
  if (sqlite3_open(path, &database) != SQLITE_OK)
    return false;
  int code = sqlite3_exec(database, sql, NULL, NULL, NULL);
  return sqlite3_close(database) == SQLITE_OK && code == SQLITE_OK;
}

static bool raw_integer(const char *path, const char *sql,
                        sqlite3_int64 *value) {
  sqlite3 *database = NULL;
  sqlite3_stmt *statement = NULL;
  bool ok = sqlite3_open(path, &database) == SQLITE_OK &&
            sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW &&
            sqlite3_column_type(statement, 0) == SQLITE_INTEGER;
  if (ok)
    *value = sqlite3_column_int64(statement, 0);
  (void)sqlite3_finalize(statement);
  if (database)
    ok = sqlite3_close(database) == SQLITE_OK && ok;
  return ok;
}

static bool all_bytes_zero(const void *value, size_t size) {
  const unsigned char *bytes = value;
  for (size_t index = 0; index < size; ++index)
    if (bytes[index] != 0)
      return false;
  return true;
}

static bool raw_object_sql(const char *path, const char *name, char *output,
                           size_t capacity) {
  sqlite3 *database = NULL;
  sqlite3_stmt *statement = NULL;
  bool ok = sqlite3_open(path, &database) == SQLITE_OK &&
            sqlite3_prepare_v2(
                database,
                "SELECT sql FROM sqlite_master WHERE name=?1 AND sql IS NOT NULL",
                -1, &statement, NULL) == SQLITE_OK;
  if (ok)
    (void)sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
  if (ok)
    ok = sqlite3_step(statement) == SQLITE_ROW &&
         sqlite3_column_type(statement, 0) == SQLITE_TEXT;
  if (ok) {
    int bytes = sqlite3_column_bytes(statement, 0);
    const unsigned char *text = sqlite3_column_text(statement, 0);
    ok = bytes >= 0 && (size_t)bytes < capacity && text;
    if (ok) {
      memcpy(output, text, (size_t)bytes);
      output[bytes] = '\0';
    }
  }
  (void)sqlite3_finalize(statement);
  if (database)
    ok = sqlite3_close(database) == SQLITE_OK && ok;
  return ok;
}

static bool make_database_path(char directory[64], char path[256]) {
  memcpy(directory, "/tmp/lardon3d-optical-XXXXXX", 29);
  if (!mkdtemp(directory))
    return false;
  int bytes = snprintf(path, 256, "%s/project.db", directory);
  return bytes > 0 && bytes < 256;
}

static bool create_sparse_calibration(Lardon3DProjectDb *database,
                                      unsigned char fingerprint_byte,
                                      uint32_t width, uint32_t height,
                                      Lardon3DSparseCalibration *output) {
  Lardon3DSparseCalibration input = {
      .model_kind = LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE,
      .model_version = LARDON3D_SPARSE_SFM_CALIBRATION_VERSION,
      .width = width,
      .height = height,
      .fx = (double)width * 0.75,
      .fy = (double)height,
      .cx = (double)width * 0.5,
      .cy = (double)height * 0.5,
      .provenance_kind = LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT,
  };
  input.provenance_fingerprint[0] = fingerprint_byte;
  return lardon3d_sparse_calibration_create(database, &input, output) ==
         LARDON3D_PROJECT_DB_OK;
}

static bool seed_campaign(Lardon3DProjectDb *database, uint64_t task_id,
                          uint64_t scanset_id, uint32_t group_count) {
  Lardon3DTaskDurableSnapshot snapshot = {
      .id = task_id,
      .saved_state = TASK_PENDING,
      .recovery_state = TASK_PENDING,
  };
  memcpy(snapshot.name, "optical campaign", 17);
  static const unsigned char request[] = {0x4c, 0x33, 0x44, 0x4f};
  Lardon3DProjectDbAcquisitionCampaignTask campaign = {
      .task_id = task_id,
      .scanset_id = scanset_id,
      .next_group_id = 0,
      .group_count = group_count,
      .request = request,
      .request_size = sizeof(request),
  };
  return lardon3d_project_db_record_acquisition_campaign_task(
             database, &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
             LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, NULL, &campaign,
             1) == LARDON3D_PROJECT_DB_OK;
}

typedef struct {
  uint64_t scanset_id;
  uint64_t configuration_id;
  uint64_t alternate_configuration_id;
  uint64_t capture_id;
} RepairOpticalFixture;

static bool seed_repair_optical_fixture(Lardon3DProjectDb *database,
                                        RepairOpticalFixture *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  Lardon3DProjectDbScanSet scanset;
  if (lardon3d_project_db_create_scanset(database, "repair optical fixture",
                                         &scanset) != LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalCameraBodyProfile body_input = {0};
  memcpy(body_input.manufacturer, "Generic", sizeof("Generic"));
  memcpy(body_input.model, "Repair body", sizeof("Repair body"));
  memcpy(body_input.name, "Repair body profile",
         sizeof("Repair body profile"));
  Lardon3DOpticalCameraBodyProfile body;
  if (lardon3d_optical_camera_body_create(database, &body_input, &body) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalLensProfile lens_input = {
      .interface_kind = LARDON3D_OPTICAL_LENS_ELECTRONIC,
      .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_ZOOM,
      .minimum_focal_um = 16000,
      .maximum_focal_um = 50000,
  };
  memcpy(lens_input.manufacturer, "Generic", sizeof("Generic"));
  memcpy(lens_input.model, "Repair zoom", sizeof("Repair zoom"));
  memcpy(lens_input.name, "Repair 16-50 zoom",
         sizeof("Repair 16-50 zoom"));
  Lardon3DOpticalLensProfile lens;
  if (lardon3d_optical_lens_create(database, &lens_input, &lens) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalConfiguration configuration_input = {
      .camera_body_profile_id = body.camera_body_profile_id,
      .lens_profile_id = lens.lens_profile_id,
      .has_focal_length = true,
      .focal_length_um = 16000,
  };
  Lardon3DOpticalConfiguration configuration;
  if (lardon3d_optical_configuration_create(database, &configuration_input,
                                             &configuration) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  configuration_input.focal_length_um = 24000;
  Lardon3DOpticalConfiguration alternate;
  if (lardon3d_optical_configuration_create(database, &configuration_input,
                                             &alternate) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DProjectDbCapture capture;
  if (lardon3d_project_db_create_capture(database, scanset.scanset_id, 1,
                                         &capture) != LARDON3D_PROJECT_DB_OK)
    return false;
  fixture->scanset_id = scanset.scanset_id;
  fixture->configuration_id = configuration.optical_configuration_id;
  fixture->alternate_configuration_id = alternate.optical_configuration_id;
  fixture->capture_id = capture.capture_id;
  return true;
}

static bool expect_group_assign_corrupt_without_row(
    const char *path, uint64_t task_id, uint64_t configuration_id) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  bool ok = lardon3d_optical_campaign_group_assign(
                database, task_id, 1, configuration_id) ==
            LARDON3D_PROJECT_DB_CORRUPT;
  lardon3d_project_db_close(database);
  sqlite3_int64 count = -1;
  return ok &&
         raw_integer(path,
                     "SELECT COUNT(*) FROM acquisition_campaign_group_optics",
                     &count) &&
         count == 0;
}

static bool expect_retain_result_unadvanced(
    const char *path, uint64_t task_id, uint64_t capture_id,
    Lardon3DProjectDbResult expected) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  bool ok = lardon3d_project_db_retain_acquisition_campaign_capture(
                database, task_id, 1, capture_id, 1) == expected;
  unsigned char request[16];
  Lardon3DProjectDbAcquisitionCampaignTask campaign;
  Lardon3DProjectDbAcquisitionCampaignCapture mapping;
  ok = ok &&
       lardon3d_project_db_load_acquisition_campaign_task(
           database, task_id, request, sizeof(request), &campaign) ==
           LARDON3D_PROJECT_DB_OK &&
       campaign.next_group_id == 0 &&
       lardon3d_project_db_load_acquisition_campaign_capture(
           database, task_id, 1, &mapping) == LARDON3D_PROJECT_DB_NOT_FOUND;
  lardon3d_project_db_close(database);
  return ok;
}

static bool test_profiles_assignments_and_calibrations(void) {
  char directory[64];
  char path[256];
  CHECK(make_database_path(directory, path));
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) ==
        LARDON3D_PROJECT_DB_SCHEMA_VERSION);

  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_project_db_create_scanset(database, "mixed optical campaign",
                                            &scanset) == LARDON3D_PROJECT_DB_OK);

  Lardon3DOpticalCameraBodyProfile sony_input = {0};
  memcpy(sony_input.manufacturer, "Sony", 5);
  memcpy(sony_input.model, "ILCE-6000", 10);
  memcpy(sony_input.name, "A6000 body", 11);
  Lardon3DOpticalCameraBodyProfile sony;
  CHECK(lardon3d_optical_camera_body_create(database, &sony_input, &sony) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalCameraBodyProfile retry_body;
  CHECK(lardon3d_optical_camera_body_create(database, &sony_input, &retry_body) ==
            LARDON3D_PROJECT_DB_OK &&
        retry_body.camera_body_profile_id == sony.camera_body_profile_id);

  Lardon3DOpticalCameraBodyAlias body_alias;
  CHECK(lardon3d_optical_camera_body_alias_add(
            database, sony.camera_body_profile_id, "SONY", "ILCE-6000",
            &body_alias) == LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalCameraBodyProfile alias_body;
  CHECK(lardon3d_optical_camera_body_find_exact_alias(
            database, "SONY", "ILCE-6000", &alias_body) ==
            LARDON3D_PROJECT_DB_OK &&
        alias_body.camera_body_profile_id == sony.camera_body_profile_id);
  memset(&alias_body, 0x7f, sizeof(alias_body));
  CHECK(lardon3d_optical_camera_body_find_exact_alias(
            database, "sony", "ILCE-6000", &alias_body) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        alias_body.camera_body_profile_id == 0 && alias_body.name[0] == '\0');

  Lardon3DOpticalCameraBodyProfile second_body_input = {0};
  memcpy(second_body_input.manufacturer, "Samsung", 8);
  memcpy(second_body_input.model, "SM-G990B", 9);
  memcpy(second_body_input.name, "S21 FE body", 12);
  Lardon3DOpticalCameraBodyProfile second_body;
  CHECK(lardon3d_optical_camera_body_create(database, &second_body_input,
                                             &second_body) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalCameraBodyAlias conflict_alias;
  CHECK(lardon3d_optical_camera_body_alias_add(
            database, second_body.camera_body_profile_id, "SONY", "ILCE-6000",
            &conflict_alias) == LARDON3D_PROJECT_DB_CONSTRAINT);

  Lardon3DOpticalLensProfile zoom_input = {
      .interface_kind = LARDON3D_OPTICAL_LENS_ELECTRONIC,
      .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_ZOOM,
      .minimum_focal_um = 16000,
      .maximum_focal_um = 50000,
  };
  memcpy(zoom_input.manufacturer, "Sony", 5);
  memcpy(zoom_input.model, "SELP1650", 9);
  memcpy(zoom_input.name, "E PZ 16-50 OSS", 15);
  Lardon3DOpticalLensProfile zoom;
  CHECK(lardon3d_optical_lens_create(database, &zoom_input, &zoom) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalLensAlias zoom_alias;
  CHECK(lardon3d_optical_lens_alias_add(
            database, zoom.lens_profile_id, "SONY",
            "E PZ 16-50mm F3.5-5.6 OSS", &zoom_alias) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalLensProfile exact_lens;
  CHECK(lardon3d_optical_lens_find_exact_alias(
            database, "SONY", "E PZ 16-50mm F3.5-5.6 OSS", &exact_lens) ==
            LARDON3D_PROJECT_DB_OK &&
        exact_lens.lens_profile_id == zoom.lens_profile_id);

  /* A manual lens has no electronics and therefore legitimately has an empty
     alias page. It remains fully usable by explicit profile/config selection. */
  Lardon3DOpticalLensProfile meike_input = {
      .interface_kind = LARDON3D_OPTICAL_LENS_MANUAL,
      .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_PRIME,
      .minimum_focal_um = 12000,
      .maximum_focal_um = 12000,
  };
  memcpy(meike_input.manufacturer, "Meike", 6);
  memcpy(meike_input.model, "12mm F2.8", sizeof("12mm F2.8"));
  memcpy(meike_input.name, "Meike manual 12 mm", 19);
  Lardon3DOpticalLensProfile meike;
  CHECK(lardon3d_optical_lens_create(database, &meike_input, &meike) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalLensAlias no_alias_items[2];
  size_t no_alias_count = 99;
  uint64_t no_alias_next = 99;
  CHECK(lardon3d_optical_lens_alias_list(
            database, meike.lens_profile_id, 0, no_alias_items, 2,
            &no_alias_count, &no_alias_next) == LARDON3D_PROJECT_DB_OK &&
        no_alias_count == 0 && no_alias_next == 0);

  Lardon3DOpticalLensProfile prime_input = {
      .interface_kind = LARDON3D_OPTICAL_LENS_ELECTRONIC,
      .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_PRIME,
      .minimum_focal_um = 50000,
      .maximum_focal_um = 50000,
  };
  memcpy(prime_input.manufacturer, "Generic", 8);
  memcpy(prime_input.model, "50 Prime", 9);
  memcpy(prime_input.name, "New 50 mm profile", 18);
  Lardon3DOpticalLensProfile prime;
  CHECK(lardon3d_optical_lens_create(database, &prime_input, &prime) ==
        LARDON3D_PROJECT_DB_OK);

  Lardon3DOpticalLensProfile lens_page[2];
  size_t lens_count = 0;
  uint64_t lens_next = 0;
  CHECK(lardon3d_optical_lens_list(database, 0, lens_page, 2, &lens_count,
                                   &lens_next) == LARDON3D_PROJECT_DB_OK &&
        lens_count == 2 && lens_next == meike.lens_profile_id);
  CHECK(lardon3d_optical_lens_list(database, lens_next, lens_page, 2,
                                   &lens_count, &lens_next) ==
            LARDON3D_PROJECT_DB_OK &&
        lens_count == 1 && lens_page[0].lens_profile_id == prime.lens_profile_id);

  Lardon3DOpticalLensProfile unknown_manual_input = {
      .interface_kind = LARDON3D_OPTICAL_LENS_MANUAL,
      .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_UNKNOWN,
  };
  memcpy(unknown_manual_input.name, "Unidentified manual lens",
         sizeof("Unidentified manual lens"));
  Lardon3DOpticalLensProfile unknown_manual;
  CHECK(lardon3d_optical_lens_create(database, &unknown_manual_input,
                                     &unknown_manual) ==
            LARDON3D_PROJECT_DB_OK &&
        unknown_manual.manufacturer[0] == '\0' &&
        unknown_manual.model[0] == '\0' &&
        unknown_manual.minimum_focal_um == 0 &&
        unknown_manual.maximum_focal_um == 0);

  Lardon3DOpticalConfiguration configuration_input = {
      .camera_body_profile_id = sony.camera_body_profile_id,
      .lens_profile_id = zoom.lens_profile_id,
      .has_focal_length = true,
      .focal_length_um = 16000,
  };
  Lardon3DOpticalConfiguration config16;
  CHECK(lardon3d_optical_configuration_create(database, &configuration_input,
                                               &config16) ==
        LARDON3D_PROJECT_DB_OK);
  configuration_input.focal_length_um = 24000;
  Lardon3DOpticalConfiguration config24;
  CHECK(lardon3d_optical_configuration_create(database, &configuration_input,
                                               &config24) ==
        LARDON3D_PROJECT_DB_OK);
  configuration_input.focal_length_um = 50000;
  Lardon3DOpticalConfiguration config50;
  CHECK(lardon3d_optical_configuration_create(database, &configuration_input,
                                               &config50) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(config16.optical_configuration_id != config24.optical_configuration_id &&
        config24.optical_configuration_id != config50.optical_configuration_id);
  configuration_input.focal_length_um = 51000;
  Lardon3DOpticalConfiguration invalid_configuration;
  CHECK(lardon3d_optical_configuration_create(database, &configuration_input,
                                               &invalid_configuration) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);

  Lardon3DOpticalConfiguration meike_configuration_input = {
      .camera_body_profile_id = sony.camera_body_profile_id,
      .lens_profile_id = meike.lens_profile_id,
      .has_focal_length = true,
      .focal_length_um = 12000,
  };
  Lardon3DOpticalConfiguration meike_configuration;
  CHECK(lardon3d_optical_configuration_create(
            database, &meike_configuration_input, &meike_configuration) ==
        LARDON3D_PROJECT_DB_OK);
  meike_configuration_input.focal_length_um = 13000;
  CHECK(lardon3d_optical_configuration_create(
            database, &meike_configuration_input, &invalid_configuration) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);

  Lardon3DProjectDbCapture captures[5];
  for (size_t index = 0; index < 5; ++index)
    CHECK(lardon3d_project_db_create_capture(database, scanset.scanset_id,
                                              (int64_t)index + 1,
                                              &captures[index]) ==
          LARDON3D_PROJECT_DB_OK);

  Lardon3DOpticalCaptureAssignment unresolved;
  memset(&unresolved, 0x7f, sizeof(unresolved));
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[4].capture_id, &unresolved) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        unresolved.capture_id == 0 && unresolved.optical_configuration_id == 0);
  CHECK(lardon3d_optical_capture_assign_explicit(
            database, captures[3].capture_id,
            meike_configuration.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_capture_assign_explicit(
            database, captures[3].capture_id,
            meike_configuration.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_capture_assign_explicit(
            database, captures[3].capture_id,
            config16.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  Lardon3DOpticalCaptureAssignment manual_assignment;
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[3].capture_id, &manual_assignment) ==
            LARDON3D_PROJECT_DB_OK &&
        manual_assignment.provenance ==
            LARDON3D_OPTICAL_ASSIGNMENT_CALLER_EXPLICIT &&
        !manual_assignment.has_campaign_origin);

  CHECK(seed_campaign(database, 101, scanset.scanset_id, 1));
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 101, 1, config16.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 101, 1, captures[3].capture_id, 1) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  Lardon3DProjectDbAcquisitionCampaignCapture conflicted_mapping;
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 101, 1, &conflicted_mapping) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  unsigned char conflict_request[16];
  Lardon3DProjectDbAcquisitionCampaignTask conflict_campaign;
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 101, conflict_request, sizeof(conflict_request),
            &conflict_campaign) == LARDON3D_PROJECT_DB_OK &&
        conflict_campaign.next_group_id == 0);

  CHECK(seed_campaign(database, 100, scanset.scanset_id, 3));
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 100, 1, config16.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 100, 3, config50.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 100, 1, config16.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 100, 1, config24.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  Lardon3DOpticalCampaignGroupAssignment loaded_group;
  CHECK(lardon3d_optical_campaign_group_load(database, 100, 1, &loaded_group) ==
            LARDON3D_PROJECT_DB_OK &&
        loaded_group.optical_configuration_id ==
            config16.optical_configuration_id);
  memset(&loaded_group, 0x7f, sizeof(loaded_group));
  CHECK(lardon3d_optical_campaign_group_load(database, 100, 2, &loaded_group) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        loaded_group.campaign_task_id == 0);

  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 100, 1, captures[0].capture_id, 1) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalCaptureAssignment campaign_assignment;
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[0].capture_id, &campaign_assignment) ==
            LARDON3D_PROJECT_DB_OK &&
        campaign_assignment.optical_configuration_id ==
            config16.optical_configuration_id &&
        campaign_assignment.provenance == LARDON3D_OPTICAL_ASSIGNMENT_CAMPAIGN &&
        campaign_assignment.has_campaign_origin &&
        campaign_assignment.campaign_task_id == 100 &&
        campaign_assignment.campaign_group_id == 1);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 100, 1, captures[0].capture_id, 1) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 100, 2, config24.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);

  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 100, 2, captures[1].capture_id, 2) ==
        LARDON3D_PROJECT_DB_OK);
  memset(&unresolved, 0x7f, sizeof(unresolved));
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[1].capture_id, &unresolved) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        unresolved.capture_id == 0);

  /* Failure after mapping+optics insertion but before cursor publication must
     roll back all three durable facts as one unit. */
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_CAMPAIGN_OPTICS_COPY", "1", 1) ==
        0);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 100, 3, captures[2].capture_id, 3) !=
        LARDON3D_PROJECT_DB_OK);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_CAMPAIGN_OPTICS_COPY") == 0);
  Lardon3DProjectDbAcquisitionCampaignCapture missing_mapping;
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 100, 3, &missing_mapping) == LARDON3D_PROJECT_DB_NOT_FOUND);
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[2].capture_id, &unresolved) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  unsigned char campaign_request[16];
  Lardon3DProjectDbAcquisitionCampaignTask campaign_state;
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 100, campaign_request, sizeof(campaign_request),
            &campaign_state) == LARDON3D_PROJECT_DB_OK &&
        campaign_state.next_group_id == 2);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 100, 3, captures[2].capture_id, 3) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[2].capture_id, &campaign_assignment) ==
            LARDON3D_PROJECT_DB_OK &&
        campaign_assignment.optical_configuration_id ==
            config50.optical_configuration_id);

  Lardon3DSparseCalibration calibration_a;
  Lardon3DSparseCalibration calibration_b;
  Lardon3DSparseCalibration calibration_other;
  CHECK(create_sparse_calibration(database, 1, 6000, 4000, &calibration_a));
  CHECK(create_sparse_calibration(database, 2, 6000, 4000, &calibration_b));
  CHECK(create_sparse_calibration(database, 3, 4000, 3000,
                                  &calibration_other));

  Lardon3DOpticalCalibrationProfile compatible[3];
  size_t compatible_count = 77;
  uint64_t compatible_next = 77;
  CHECK(lardon3d_optical_calibration_profile_list_compatible(
            database, config16.optical_configuration_id, 0, compatible, 3,
            &compatible_count, &compatible_next) == LARDON3D_PROJECT_DB_OK &&
        compatible_count == 0 && compatible_next == 0);

  Lardon3DOpticalCalibrationProfile profile_input = {
      .optical_configuration_id = config16.optical_configuration_id,
      .sparse_calibration_id = calibration_a.calibration_id,
      .profile_version = 1,
      .applicability = LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION,
      .created_at = 100,
  };
  memcpy(profile_input.name, "Lab calibration", 16);
  memcpy(profile_input.provenance, "Dedicated checkerboard session", 31);
  Lardon3DOpticalCalibrationProfile profile_a;
  CHECK(lardon3d_optical_calibration_profile_create(database, &profile_input,
                                                     &profile_a) ==
        LARDON3D_PROJECT_DB_OK);
  compatible_count = 0;
  compatible_next = 0;
  CHECK(lardon3d_optical_calibration_profile_list_compatible(
            database, config16.optical_configuration_id, 0, compatible, 3,
            &compatible_count, &compatible_next) == LARDON3D_PROJECT_DB_OK &&
        compatible_count == 1 &&
        compatible[0].calibration_profile_id == profile_a.calibration_profile_id);
  Lardon3DOpticalCalibrationProfile profile_retry;
  CHECK(lardon3d_optical_calibration_profile_create(database, &profile_input,
                                                     &profile_retry) ==
            LARDON3D_PROJECT_DB_OK &&
        profile_retry.calibration_profile_id == profile_a.calibration_profile_id);
  profile_input.sparse_calibration_id = calibration_b.calibration_id;
  memcpy(profile_input.name, "Field calibration", 18);
  profile_input.created_at = 101;
  Lardon3DOpticalCalibrationProfile profile_b;
  CHECK(lardon3d_optical_calibration_profile_create(database, &profile_input,
                                                     &profile_b) ==
        LARDON3D_PROJECT_DB_OK);
  profile_input.optical_configuration_id = config50.optical_configuration_id;
  profile_input.sparse_calibration_id = calibration_other.calibration_id;
  memcpy(profile_input.name, "Fifty calibration", 18);
  profile_input.created_at = 102;
  Lardon3DOpticalCalibrationProfile profile_other;
  CHECK(lardon3d_optical_calibration_profile_create(database, &profile_input,
                                                     &profile_other) ==
        LARDON3D_PROJECT_DB_OK);
  profile_input.sparse_calibration_id = INT64_MAX;
  memcpy(profile_input.name, "Missing calibration", 20);
  CHECK(lardon3d_optical_calibration_profile_create(database, &profile_input,
                                                     &profile_retry) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        profile_retry.calibration_profile_id == 0);

  compatible_count = 0;
  compatible_next = 0;
  CHECK(lardon3d_optical_calibration_profile_list_compatible(
            database, config16.optical_configuration_id, 0, compatible, 3,
            &compatible_count, &compatible_next) == LARDON3D_PROJECT_DB_OK &&
        compatible_count == 2 &&
        compatible[0].calibration_profile_id == profile_a.calibration_profile_id &&
        compatible[1].calibration_profile_id == profile_b.calibration_profile_id);
  Lardon3DOpticalCaptureCalibrationSelection selection;
  memset(&selection, 0x7f, sizeof(selection));
  CHECK(lardon3d_optical_capture_calibration_selection_load(
            database, captures[0].capture_id, &selection) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        selection.capture_id == 0);
  CHECK(lardon3d_optical_capture_calibration_select(
            database, captures[0].capture_id,
            profile_other.calibration_profile_id) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_optical_capture_calibration_select(
            database, captures[0].capture_id, profile_a.calibration_profile_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_capture_calibration_select(
            database, captures[0].capture_id, profile_a.calibration_profile_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_capture_calibration_select(
            database, captures[0].capture_id, profile_b.calibration_profile_id) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_optical_capture_calibration_selection_load(
            database, captures[0].capture_id, &selection) ==
            LARDON3D_PROJECT_DB_OK &&
        selection.calibration_profile_id == profile_a.calibration_profile_id &&
        selection.optical_configuration_id == config16.optical_configuration_id &&
        selection.sparse_calibration_id == calibration_a.calibration_id);
  CHECK(lardon3d_optical_capture_calibration_select(
            database, captures[1].capture_id, profile_a.calibration_profile_id) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);

  lardon3d_project_db_close(database);
  database = NULL;

  /* Dynamic SQLite typing is adversarially corrupted after disabling checks;
     public loaders must reject before narrowing and leave outputs zeroed. */
  char corruption[1024];
  int corruption_bytes = snprintf(
      corruption, sizeof(corruption),
      "PRAGMA ignore_check_constraints=ON;UPDATE lens_profiles SET "
      "focal_range_kind='zoom-ish' WHERE lens_profile_id=%llu;",
      (unsigned long long)zoom.lens_profile_id);
  CHECK(corruption_bytes > 0 && (size_t)corruption_bytes < sizeof(corruption));
  CHECK(raw_sql(path, corruption));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  memset(&exact_lens, 0x7f, sizeof(exact_lens));
  CHECK(lardon3d_optical_lens_load(database, zoom.lens_profile_id, &exact_lens) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        exact_lens.lens_profile_id == 0 && exact_lens.name[0] == '\0');
  lardon3d_project_db_close(database);
  database = NULL;

  corruption_bytes = snprintf(
      corruption, sizeof(corruption),
      "PRAGMA ignore_check_constraints=ON;UPDATE lens_profiles SET "
      "focal_range_kind=3 WHERE lens_profile_id=%llu;UPDATE "
      "optical_configurations SET focal_length_um='16000x' WHERE "
      "optical_configuration_id=%llu;",
      (unsigned long long)zoom.lens_profile_id,
      (unsigned long long)config16.optical_configuration_id);
  CHECK(corruption_bytes > 0 && (size_t)corruption_bytes < sizeof(corruption));
  CHECK(raw_sql(path, corruption));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalConfiguration corrupt_configuration;
  memset(&corrupt_configuration, 0x7f, sizeof(corrupt_configuration));
  CHECK(lardon3d_optical_configuration_load(
            database, config16.optical_configuration_id,
            &corrupt_configuration) == LARDON3D_PROJECT_DB_CORRUPT &&
        corrupt_configuration.optical_configuration_id == 0);
  lardon3d_project_db_close(database);
  database = NULL;

  corruption_bytes = snprintf(
      corruption, sizeof(corruption),
      "PRAGMA foreign_keys=OFF;UPDATE optical_configurations SET "
      "focal_length_um=16000 WHERE optical_configuration_id=%llu;"
      "UPDATE captures SET scanset_id=9223372036854775807 WHERE "
      "capture_id=%llu;",
      (unsigned long long)config16.optical_configuration_id,
      (unsigned long long)captures[3].capture_id);
  CHECK(corruption_bytes > 0 && (size_t)corruption_bytes < sizeof(corruption));
  CHECK(raw_sql(path, corruption));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  memset(&manual_assignment, 0x7f, sizeof(manual_assignment));
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[3].capture_id, &manual_assignment) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        manual_assignment.capture_id == 0);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(raw_sql(path,
                "PRAGMA ignore_check_constraints=ON;UPDATE "
                "acquisition_campaign_captures SET group_id='1x' WHERE "
                "task_id=100 AND group_id=1;"));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 100, 1, config16.optical_configuration_id) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  memset(&loaded_group, 0x7f, sizeof(loaded_group));
  CHECK(lardon3d_optical_campaign_group_load(database, 100, 1, &loaded_group) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        loaded_group.campaign_task_id == 0);
  memset(&campaign_assignment, 0x7f, sizeof(campaign_assignment));
  CHECK(lardon3d_optical_capture_assignment_load(
            database, captures[0].capture_id, &campaign_assignment) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        campaign_assignment.capture_id == 0);
  lardon3d_project_db_close(database);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

static bool test_calibration_conflict_result_contract(void) {
  char directory[64];
  char path[256];
  CHECK(make_database_path(directory, path));
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  RepairOpticalFixture fixture;
  CHECK(seed_repair_optical_fixture(database, &fixture));
  Lardon3DSparseCalibration calibration;
  CHECK(create_sparse_calibration(database, 71, 6000, 4000, &calibration));

  Lardon3DOpticalCalibrationProfile input = {
      .optical_configuration_id = fixture.configuration_id,
      .sparse_calibration_id = calibration.calibration_id,
      .profile_version = 1,
      .applicability = LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION,
      .created_at = 10,
  };
  memcpy(input.name, "Retry identity", sizeof("Retry identity"));
  memcpy(input.provenance, "Original provenance",
         sizeof("Original provenance"));
  Lardon3DOpticalCalibrationProfile created;
  CHECK(lardon3d_optical_calibration_profile_create(database, &input, &created) ==
        LARDON3D_PROJECT_DB_OK);

  Lardon3DOpticalCalibrationProfile conflict = input;
  memset(conflict.provenance, 0, sizeof(conflict.provenance));
  memcpy(conflict.provenance, "Different valid provenance",
         sizeof("Different valid provenance"));
  Lardon3DOpticalCalibrationProfile conflict_output;
  memset(&conflict_output, 0x7f, sizeof(conflict_output));
  CHECK(lardon3d_optical_calibration_profile_create(
            database, &conflict, &conflict_output) ==
            LARDON3D_PROJECT_DB_CONSTRAINT &&
        conflict_output.calibration_profile_id == 0 &&
        conflict_output.provenance[0] == '\0');
  Lardon3DOpticalCalibrationProfile unchanged;
  CHECK(lardon3d_optical_calibration_profile_load(
            database, created.calibration_profile_id, &unchanged) ==
            LARDON3D_PROJECT_DB_OK &&
        strcmp(unchanged.provenance, input.provenance) == 0 &&
        unchanged.created_at == input.created_at);
  lardon3d_project_db_close(database);
  database = NULL;
  sqlite3_int64 count = 0;
  CHECK(raw_integer(path, "SELECT COUNT(*) FROM optical_calibration_profiles",
                    &count) &&
        count == 1);

  /* A BLOB in the TEXT property is durable corruption, not a second kind of
     caller conflict, even when the natural-key fields still match. */
  CHECK(raw_sql(path,
                "PRAGMA ignore_check_constraints=ON;UPDATE "
                "optical_calibration_profiles SET provenance=X'626164';"));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  memset(&conflict_output, 0x7f, sizeof(conflict_output));
  CHECK(lardon3d_optical_calibration_profile_create(
            database, &input, &conflict_output) == LARDON3D_PROJECT_DB_CORRUPT &&
        conflict_output.calibration_profile_id == 0 &&
        conflict_output.provenance[0] == '\0');
  lardon3d_project_db_close(database);
  CHECK(raw_integer(path, "SELECT COUNT(*) FROM optical_calibration_profiles",
                    &count) &&
        count == 1);
  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

static bool test_calibration_dependency_corruption_precedence(void) {
  char directory[64];
  char path[256];
  CHECK(make_database_path(directory, path));
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  RepairOpticalFixture fixture;
  CHECK(seed_repair_optical_fixture(database, &fixture));
  Lardon3DSparseCalibration calibration;
  Lardon3DSparseCalibration alternate_calibration;
  CHECK(create_sparse_calibration(database, 81, 6000, 4000, &calibration));
  CHECK(create_sparse_calibration(database, 82, 6000, 4000,
                                  &alternate_calibration));

  Lardon3DOpticalCalibrationProfile input = {
      .optical_configuration_id = fixture.configuration_id,
      .sparse_calibration_id = calibration.calibration_id,
      .profile_version = 1,
      .applicability = LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION,
      .created_at = 20,
  };
  memcpy(input.name, "Dependency identity", sizeof("Dependency identity"));
  memcpy(input.provenance, "Dependency provenance",
         sizeof("Dependency provenance"));
  Lardon3DOpticalCalibrationProfile created;
  CHECK(lardon3d_optical_calibration_profile_create(database, &input, &created) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;

  char sql[512];
  char unchanged_query[512];
  int unchanged_bytes = snprintf(
      unchanged_query, sizeof(unchanged_query),
      "SELECT COUNT(*) FROM optical_calibration_profiles WHERE "
      "calibration_profile_id=%llu AND optical_configuration_id=%llu AND "
      "sparse_calibration_id=%llu AND name='Dependency identity' AND "
      "profile_version=1 AND provenance='Dependency provenance' AND "
      "applicability=1 AND created_at=20",
      (unsigned long long)created.calibration_profile_id,
      (unsigned long long)fixture.configuration_id,
      (unsigned long long)calibration.calibration_id);
  CHECK(unchanged_bytes > 0 &&
        (size_t)unchanged_bytes < sizeof(unchanged_query));
  int bytes = snprintf(
      sql, sizeof(sql),
      "UPDATE sparse_calibrations SET scientific_hash=zeroblob(32) WHERE "
      "calibration_id=%llu",
      (unsigned long long)calibration.calibration_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(sql));
  CHECK(raw_sql(path, sql));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);

  /* The natural-key row is durable, so its malformed scientific dependency
     must win over both exact-retry and caller-conflict classification. */
  Lardon3DOpticalCalibrationProfile output;
  memset(&output, 0x7f, sizeof(output));
  CHECK(lardon3d_optical_calibration_profile_create(database, &input, &output) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        all_bytes_zero(&output, sizeof(output)));
  Lardon3DOpticalCalibrationProfile alternate = input;
  alternate.sparse_calibration_id = alternate_calibration.calibration_id;
  memset(&output, 0x7f, sizeof(output));
  CHECK(lardon3d_optical_calibration_profile_create(
            database, &alternate, &output) == LARDON3D_PROJECT_DB_CORRUPT &&
        all_bytes_zero(&output, sizeof(output)));
  lardon3d_project_db_close(database);
  database = NULL;

  sqlite3_int64 count = 0;
  CHECK(raw_integer(path, unchanged_query, &count) && count == 1);

  bytes = snprintf(sql, sizeof(sql),
                   "PRAGMA foreign_keys=OFF;DELETE FROM sparse_calibrations "
                   "WHERE calibration_id=%llu",
                   (unsigned long long)calibration.calibration_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(sql));
  CHECK(raw_sql(path, sql));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);

  memset(&output, 0x7f, sizeof(output));
  CHECK(lardon3d_optical_calibration_profile_create(database, &input, &output) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        all_bytes_zero(&output, sizeof(output)));
  memset(&output, 0x7f, sizeof(output));
  CHECK(lardon3d_optical_calibration_profile_create(
            database, &alternate, &output) == LARDON3D_PROJECT_DB_CORRUPT &&
        all_bytes_zero(&output, sizeof(output)));
  lardon3d_project_db_close(database);

  CHECK(raw_integer(path, "SELECT COUNT(*) FROM optical_calibration_profiles",
                    &count) &&
        count == 1);
  CHECK(raw_integer(path, unchanged_query, &count) && count == 1);
  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

static bool test_campaign_request_corruption_prevents_optics_mutation(void) {
  char directory[64];
  char path[256];
  CHECK(make_database_path(directory, path));
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  RepairOpticalFixture fixture;
  CHECK(seed_repair_optical_fixture(database, &fixture));
  CHECK(seed_campaign(database, 200, fixture.scanset_id, 1));
  lardon3d_project_db_close(database);

  CHECK(raw_sql(path,
                "UPDATE acquisition_campaign_tasks SET request='not-a-blob' "
                "WHERE task_id=200;"));
  CHECK(expect_group_assign_corrupt_without_row(
      path, 200, fixture.configuration_id));
  CHECK(raw_sql(path,
                "PRAGMA ignore_check_constraints=ON;UPDATE "
                "acquisition_campaign_tasks SET request=X'' WHERE task_id=200;"));
  CHECK(expect_group_assign_corrupt_without_row(
      path, 200, fixture.configuration_id));

  char oversized_update[256];
  int bytes = snprintf(
      oversized_update, sizeof(oversized_update),
      "UPDATE acquisition_campaign_tasks SET request=zeroblob(%zu) WHERE "
      "task_id=200;",
      LARDON3D_ACQUISITION_CAMPAIGN_TASK_REQUEST_MAX_BYTES + 1u);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(oversized_update));
  CHECK(raw_sql(path, oversized_update));
  CHECK(expect_group_assign_corrupt_without_row(
      path, 200, fixture.configuration_id));

  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

static bool test_capture_optics_conflict_and_corruption_rollback(void) {
  char directory[64];
  char path[256];
  CHECK(make_database_path(directory, path));
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  RepairOpticalFixture fixture;
  CHECK(seed_repair_optical_fixture(database, &fixture));
  CHECK(seed_campaign(database, 300, fixture.scanset_id, 1));
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 300, 1, fixture.configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_optical_capture_assign_explicit(
            database, fixture.capture_id,
            fixture.alternate_configuration_id) == LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);

  CHECK(expect_retain_result_unadvanced(
      path, 300, fixture.capture_id, LARDON3D_PROJECT_DB_CONSTRAINT));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalCaptureAssignment assignment;
  CHECK(lardon3d_optical_capture_assignment_load(
            database, fixture.capture_id, &assignment) ==
            LARDON3D_PROJECT_DB_OK &&
        assignment.provenance ==
            LARDON3D_OPTICAL_ASSIGNMENT_CALLER_EXPLICIT &&
        assignment.optical_configuration_id ==
            fixture.alternate_configuration_id);
  lardon3d_project_db_close(database);

  char mutation[512];
  int bytes = snprintf(
      mutation, sizeof(mutation),
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "capture_optical_configurations SET assignment_provenance=X'31' WHERE "
      "capture_id=%llu;",
      (unsigned long long)fixture.capture_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(mutation));
  CHECK(raw_sql(path, mutation));
  CHECK(expect_retain_result_unadvanced(
      path, 300, fixture.capture_id, LARDON3D_PROJECT_DB_CORRUPT));

  bytes = snprintf(
      mutation, sizeof(mutation),
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "capture_optical_configurations SET assignment_provenance=9,"
      "campaign_task_id=NULL,campaign_group_id=NULL WHERE capture_id=%llu;",
      (unsigned long long)fixture.capture_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(mutation));
  CHECK(raw_sql(path, mutation));
  CHECK(expect_retain_result_unadvanced(
      path, 300, fixture.capture_id, LARDON3D_PROJECT_DB_CORRUPT));

  bytes = snprintf(
      mutation, sizeof(mutation),
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "capture_optical_configurations SET assignment_provenance=2,"
      "campaign_task_id=300,campaign_group_id=1 WHERE capture_id=%llu;",
      (unsigned long long)fixture.capture_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(mutation));
  CHECK(raw_sql(path, mutation));
  CHECK(expect_retain_result_unadvanced(
      path, 300, fixture.capture_id, LARDON3D_PROJECT_DB_CORRUPT));

  bytes = snprintf(
      mutation, sizeof(mutation),
      "PRAGMA foreign_keys=OFF;PRAGMA ignore_check_constraints=ON;DELETE FROM "
      "capture_optical_configurations WHERE capture_id=%llu;INSERT INTO "
      "capture_optical_configurations(capture_id,optical_configuration_id,"
      "assignment_provenance,campaign_task_id,campaign_group_id) VALUES("
      "%llu,%llu,1,300,1);",
      (unsigned long long)fixture.capture_id,
      (unsigned long long)fixture.capture_id,
      (unsigned long long)fixture.configuration_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(mutation));
  CHECK(raw_sql(path, mutation));
  /* This exact-looking row is ahead of the cursor and must not be repaired by
     the mapping insertion that is later rolled back. */
  CHECK(expect_retain_result_unadvanced(
      path, 300, fixture.capture_id, LARDON3D_PROJECT_DB_CORRUPT));
  sqlite3_int64 count = 0;
  CHECK(raw_integer(path,
                    "SELECT COUNT(*) FROM capture_optical_configurations",
                    &count) &&
        count == 1);
  CHECK(raw_integer(path, "SELECT COUNT(*) FROM acquisition_campaign_captures",
                    &count) &&
        count == 0);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

static bool test_malformed_configuration_rolls_back_campaign_retention(void) {
  char directory[64];
  char path[256];
  CHECK(make_database_path(directory, path));
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  RepairOpticalFixture fixture;
  CHECK(seed_repair_optical_fixture(database, &fixture));
  CHECK(seed_campaign(database, 301, fixture.scanset_id, 1));
  CHECK(lardon3d_optical_campaign_group_assign(
            database, 301, 1, fixture.configuration_id) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);

  char mutation[512];
  int bytes = snprintf(
      mutation, sizeof(mutation),
      "PRAGMA ignore_check_constraints=ON;UPDATE optical_configurations SET "
      "focal_length_um='16000x' WHERE optical_configuration_id=%llu;",
      (unsigned long long)fixture.configuration_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(mutation));
  CHECK(raw_sql(path, mutation));
  CHECK(expect_retain_result_unadvanced(
      path, 301, fixture.capture_id, LARDON3D_PROJECT_DB_CORRUPT));
  sqlite3_int64 count = 0;
  CHECK(raw_integer(path, "SELECT COUNT(*) FROM acquisition_campaign_captures",
                    &count) &&
        count == 0);
  CHECK(raw_integer(path,
                    "SELECT COUNT(*) FROM capture_optical_configurations",
                    &count) &&
        count == 0);

  bytes = snprintf(
      mutation, sizeof(mutation),
      "PRAGMA ignore_check_constraints=ON;UPDATE optical_configurations SET "
      "focal_length_um=16000 WHERE optical_configuration_id=%llu;UPDATE "
      "lens_profiles SET focal_range_kind=2,minimum_focal_um=24000,"
      "maximum_focal_um=24000;",
      (unsigned long long)fixture.configuration_id);
  CHECK(bytes > 0 && (size_t)bytes < sizeof(mutation));
  CHECK(raw_sql(path, mutation));
  CHECK(expect_retain_result_unadvanced(
      path, 301, fixture.capture_id, LARDON3D_PROJECT_DB_CORRUPT));
  CHECK(raw_integer(path, "SELECT COUNT(*) FROM acquisition_campaign_captures",
                    &count) &&
        count == 0);
  CHECK(raw_integer(path,
                    "SELECT COUNT(*) FROM capture_optical_configurations",
                    &count) &&
        count == 0);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

static bool downgrade_to_v22_fixture(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) != LARDON3D_PROJECT_DB_OK)
    return false;
  Lardon3DProjectDbScanSet a6000;
  Lardon3DProjectDbScanSet s21;
  Lardon3DProjectDbCapture capture;
  Lardon3DSparseCalibration calibration;
  bool ok = lardon3d_project_db_create_scanset(database, "A6000 originals",
                                                &a6000) ==
                LARDON3D_PROJECT_DB_OK &&
            lardon3d_project_db_create_scanset(database, "S21 originals", &s21) ==
                LARDON3D_PROJECT_DB_OK &&
            lardon3d_project_db_create_capture(database, a6000.scanset_id, 1,
                                                &capture) ==
                LARDON3D_PROJECT_DB_OK &&
            create_sparse_calibration(database, 91, 6000, 4000, &calibration) &&
            seed_campaign(database, 500, a6000.scanset_id, 1) &&
            lardon3d_project_db_retain_acquisition_campaign_capture(
                database, 500, 1, capture.capture_id, 1) ==
                LARDON3D_PROJECT_DB_OK;
  lardon3d_project_db_close(database);
  if (!ok)
    return false;
  /* This removes only objects introduced by the test-created v23 database,
     yielding a deterministic v22 fixture without rewriting any real project. */
  return raw_sql(
      path,
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE IF EXISTS feature_extract_batch_tasks;"
      "DROP TABLE raw_development_batch_tasks;"
      "DROP TABLE capture_calibration_selections;"
      "DROP TABLE optical_calibration_profiles;"
      "DROP TABLE capture_optical_configurations;"
      "DROP INDEX acquisition_campaign_capture_identity_v23;"
      "DROP TABLE acquisition_campaign_group_optics;"
      "DROP TABLE optical_configurations;"
      "DROP TABLE lens_profile_aliases;DROP TABLE lens_profiles;"
      "DROP TABLE camera_body_aliases;DROP TABLE camera_body_profiles;"
      "UPDATE metadata SET value=22 WHERE key='schema_version';COMMIT;");
}

static bool test_migration_rollback_retry_and_equivalence(void) {
  char migrated_directory[64];
  char migrated_path[256];
  char fresh_directory[64];
  char fresh_path[256];
  CHECK(make_database_path(migrated_directory, migrated_path));
  CHECK(make_database_path(fresh_directory, fresh_path));
  CHECK(downgrade_to_v22_fixture(migrated_path));

  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V23", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(migrated_path, &database, error) !=
            LARDON3D_PROJECT_DB_OK &&
        database == NULL);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V23") == 0);
  sqlite3_int64 value = 0;
  CHECK(raw_integer(migrated_path,
                    "SELECT value FROM metadata WHERE key='schema_version'",
                    &value) &&
        value == 22);
  CHECK(raw_integer(migrated_path,
                    "SELECT COUNT(*) FROM sqlite_master WHERE name="
                    "'camera_body_profiles'",
                    &value) &&
        value == 0);
  CHECK(raw_integer(migrated_path, "SELECT COUNT(*) FROM captures", &value) &&
        value == 1);
  CHECK(raw_integer(migrated_path, "SELECT COUNT(*) FROM sparse_calibrations",
                    &value) &&
        value == 1);
  CHECK(raw_integer(migrated_path,
                    "SELECT COUNT(*) FROM acquisition_campaign_captures",
                    &value) &&
        value == 1);

  CHECK(lardon3d_project_db_open(migrated_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) ==
            LARDON3D_PROJECT_DB_SCHEMA_VERSION);
  Lardon3DOpticalCaptureAssignment migrated_unresolved;
  CHECK(lardon3d_optical_capture_assignment_load(database, 1,
                                                 &migrated_unresolved) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        migrated_unresolved.capture_id == 0);
  unsigned char migrated_request[16];
  Lardon3DProjectDbAcquisitionCampaignTask migrated_campaign;
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 500, migrated_request, sizeof(migrated_request),
            &migrated_campaign) == LARDON3D_PROJECT_DB_OK &&
        migrated_campaign.next_group_id == 1);
  Lardon3DProjectDbAcquisitionCampaignCapture migrated_mapping;
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 500, 1, &migrated_mapping) == LARDON3D_PROJECT_DB_OK &&
        migrated_mapping.capture_id == 1);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(raw_integer(migrated_path, "SELECT COUNT(*) FROM captures", &value) &&
        value == 1);
  CHECK(raw_integer(migrated_path, "SELECT COUNT(*) FROM sparse_calibrations",
                    &value) &&
        value == 1);
  const char *empty_tables[] = {
      "camera_body_profiles",
      "camera_body_aliases",
      "lens_profiles",
      "lens_profile_aliases",
      "optical_configurations",
      "acquisition_campaign_group_optics",
      "capture_optical_configurations",
      "optical_calibration_profiles",
      "capture_calibration_selections",
  };
  char query[256];
  for (size_t index = 0; index < sizeof(empty_tables) / sizeof(empty_tables[0]);
       ++index) {
    CHECK(snprintf(query, sizeof(query), "SELECT COUNT(*) FROM %s",
                   empty_tables[index]) > 0);
    CHECK(raw_integer(migrated_path, query, &value) && value == 0);
  }

  CHECK(lardon3d_project_db_open(fresh_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;
  const char *objects[] = {
      "camera_body_profiles",
      "camera_body_aliases",
      "camera_body_aliases_profile_idx",
      "lens_profiles",
      "lens_profile_aliases",
      "lens_profile_aliases_profile_idx",
      "optical_configurations",
      "optical_configurations_body_idx",
      "optical_configurations_lens_idx",
      "acquisition_campaign_group_optics",
      "acquisition_campaign_capture_identity_v23",
      "capture_optical_configurations",
      "capture_optical_configurations_config_idx",
      "optical_calibration_profiles",
      "optical_calibration_profiles_config_idx",
      "optical_calibration_profiles_sparse_idx",
      "capture_calibration_selections",
  };
  char migrated_sql[8192];
  char fresh_sql[8192];
  for (size_t index = 0; index < sizeof(objects) / sizeof(objects[0]); ++index) {
    CHECK(raw_object_sql(migrated_path, objects[index], migrated_sql,
                         sizeof(migrated_sql)));
    CHECK(raw_object_sql(fresh_path, objects[index], fresh_sql,
                         sizeof(fresh_sql)));
    CHECK(strcmp(migrated_sql, fresh_sql) == 0);
  }

  CHECK(unlink(migrated_path) == 0);
  CHECK(rmdir(migrated_directory) == 0);
  CHECK(unlink(fresh_path) == 0);
  CHECK(rmdir(fresh_directory) == 0);
  return true;
}

int main(void) {
  return test_profiles_assignments_and_calibrations() &&
                 test_calibration_conflict_result_contract() &&
                 test_calibration_dependency_corruption_precedence() &&
                 test_campaign_request_corruption_prevents_optics_mutation() &&
                 test_capture_optics_conflict_and_corruption_rollback() &&
                 test_malformed_configuration_rolls_back_campaign_retention() &&
                 test_migration_rollback_retry_and_equivalence()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
