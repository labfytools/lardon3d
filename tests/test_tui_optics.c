#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/sparse_sfm_model.h>
#include <lardon3d/tui_optics.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "TUI optics failure line %d: %s\n",      \
                __LINE__, #condition);                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

static bool
copy_text(char *destination, size_t capacity, const char *source)
{
    int written = snprintf(destination, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static bool
create_sparse_calibration(
    Lardon3DProjectDb *database,
    Lardon3DSparseCalibration *output
)
{
    Lardon3DSparseCalibration input = {
        .model_kind = LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE,
        .model_version = LARDON3D_SPARSE_SFM_CALIBRATION_VERSION,
        .width = 6000,
        .height = 4000,
        .fx = 4500.0,
        .fy = 4500.0,
        .cx = 3000.0,
        .cy = 2000.0,
        .provenance_kind = LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT,
    };
    input.provenance_fingerprint[0] = 0x5a;
    return lardon3d_sparse_calibration_create(database, &input, output)
        == LARDON3D_PROJECT_DB_OK;
}

static bool
seed_campaign(
    Lardon3DProjectDb *database,
    uint64_t task_id,
    uint64_t scanset_id,
    uint32_t group_count
)
{
    Lardon3DTaskDurableSnapshot snapshot = {
        .id = task_id,
        .saved_state = TASK_PENDING,
        .recovery_state = TASK_PENDING,
    };
    CHECK(copy_text(snapshot.name, sizeof(snapshot.name), "TUI campaign"));
    static const unsigned char request[] = {0x4c, 0x33, 0x44, 0x4f};
    Lardon3DProjectDbAcquisitionCampaignTask campaign = {
        .task_id = task_id,
        .scanset_id = scanset_id,
        .group_count = group_count,
        .request = request,
        .request_size = sizeof(request),
    };
    return lardon3d_project_db_record_acquisition_campaign_task(
        database, &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
        LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, NULL,
        &campaign, 1) == LARDON3D_PROJECT_DB_OK;
}

static bool
run_test(void)
{
    char directory[] = "/tmp/lardon3d-tui-optics-XXXXXX";
    CHECK(mkdtemp(directory));
    char path[512];
    int written = snprintf(path, sizeof(path), "%s/project.db", directory);
    CHECK(written > 0 && (size_t)written < sizeof(path));
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    Lardon3DProjectDb *database = NULL;
    CHECK(lardon3d_project_db_open(path, &database, error)
        == LARDON3D_PROJECT_DB_OK);

    Lardon3DTuiOptics *optics = lardon3d_tui_optics_create();
    CHECK(optics);
    Lardon3DTuiOpticsSnapshot view;
    memset(&view, 0xa5, sizeof(view));
    CHECK(!lardon3d_tui_optics_snapshot(NULL, &view));
    CHECK(!view.project_bound && view.body_count == 0 && !view.message[0]);
    CHECK(!lardon3d_tui_optics_snapshot(optics, NULL));
    CHECK(!lardon3d_tui_optics_bind(NULL, database));
    CHECK(!lardon3d_tui_optics_bind(optics, NULL));
    lardon3d_tui_optics_unbind(NULL);
    lardon3d_tui_optics_destroy(NULL);
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(!view.project_bound);
    CHECK(view.capture_status == LARDON3D_TUI_OPTICS_NO_PROJECT);
    CHECK(lardon3d_tui_optics_bind(optics, database));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.project_bound);
    CHECK(view.body_count == 0 && view.lens_count == 0);

    /* A bind failure preserves its exact operational class and no DB borrow.
     * Releasing the lock then binding the same pointer proves explicit retry
     * is not suppressed by pointer identity. */
    lardon3d_tui_optics_unbind(optics);
    sqlite3 *locker = NULL;
    CHECK(sqlite3_open_v2(path, &locker,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK);
    CHECK(sqlite3_exec(locker, "PRAGMA busy_timeout=0;BEGIN EXCLUSIVE;",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!lardon3d_tui_optics_bind(optics, database));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(!view.project_bound && strstr(view.message, "BUSY") != NULL);
    CHECK(sqlite3_exec(locker, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(locker) == SQLITE_OK);
    CHECK(lardon3d_tui_optics_bind(optics, database));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.project_bound);

    Lardon3DOpticalCameraBodyProfile body = {0};
    CHECK(copy_text(body.manufacturer, sizeof(body.manufacturer), "Generic"));
    CHECK(copy_text(body.model, sizeof(body.model), "Test body"));
    CHECK(copy_text(body.name, sizeof(body.name), "Immutable test body"));
    CHECK(lardon3d_tui_optics_create_body(optics, &body));

    Lardon3DOpticalLensProfile meike = {
        .interface_kind = LARDON3D_OPTICAL_LENS_MANUAL,
        .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_PRIME,
        .minimum_focal_um = 50000,
        .maximum_focal_um = 50000,
    };
    CHECK(copy_text(meike.manufacturer, sizeof(meike.manufacturer), "Meike"));
    CHECK(copy_text(meike.model, sizeof(meike.model), "50mm F2"));
    CHECK(copy_text(meike.name, sizeof(meike.name), "Meike manual 50mm"));
    CHECK(lardon3d_tui_optics_create_lens(optics, &meike));
    CHECK(lardon3d_tui_optics_create_configuration(optics, true, 50000));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.body_count == 1 && view.lens_count == 1);
    CHECK(view.lenses[0].interface_kind == LARDON3D_OPTICAL_LENS_MANUAL);
    CHECK(strstr(view.message, "configuration") != NULL);
    uint64_t body_id = view.bodies[0].camera_body_profile_id;
    uint64_t manual_lens_id = view.lenses[0].lens_profile_id;
    uint64_t configuration_id =
        view.configurations[0].optical_configuration_id;
    CHECK(body_id && manual_lens_id && configuration_id);

    /* Manual/no-electronics profiles legitimately have no metadata alias. */
    Lardon3DOpticalLensAlias aliases[1];
    size_t alias_count = 99;
    uint64_t next_alias = 99;
    CHECK(lardon3d_optical_lens_alias_list(database, manual_lens_id, 0,
        aliases, 1, &alias_count, &next_alias) == LARDON3D_PROJECT_DB_OK);
    CHECK(alias_count == 0 && next_alias == 0);

    Lardon3DProjectDbScanSet scanset;
    CHECK(lardon3d_project_db_create_scanset(
        database, "TUI optics", &scanset) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbCapture capture;
    CHECK(lardon3d_project_db_create_capture(database, scanset.scanset_id,
        1, &capture) == LARDON3D_PROJECT_DB_OK);
    CHECK(seed_campaign(database, 100, scanset.scanset_id, 1));
    CHECK(lardon3d_tui_optics_assign_campaign_group(optics, 100, 1));
    Lardon3DOpticalCampaignGroupAssignment group_assignment;
    CHECK(lardon3d_optical_campaign_group_load(
        database, 100, 1, &group_assignment) == LARDON3D_PROJECT_DB_OK);
    CHECK(group_assignment.optical_configuration_id == configuration_id);

    CHECK(!lardon3d_tui_optics_inspect_capture(optics, UINT64_C(999999)));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(strstr(view.message, "NOT_FOUND") != NULL);
    CHECK(lardon3d_tui_optics_assign_capture(optics, capture.capture_id));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.capture_status == LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY);
    CHECK(view.capture_lens_found);
    CHECK(view.capture_lens.interface_kind == LARDON3D_OPTICAL_LENS_MANUAL);
    CHECK(strstr(view.message, "Manual/no-EXIF") != NULL);

    Lardon3DSparseCalibration sparse;
    CHECK(create_sparse_calibration(database, &sparse));
    Lardon3DOpticalCalibrationProfile profile = {
        .optical_configuration_id = configuration_id,
        .sparse_calibration_id = sparse.calibration_id,
        .profile_version = 1,
        .applicability =
            LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION,
    };
    CHECK(copy_text(profile.name, sizeof(profile.name), "Meike exact 50mm"));
    CHECK(copy_text(profile.provenance, sizeof(profile.provenance),
        "caller explicit test calibration"));
    Lardon3DOpticalCalibrationProfile created_profile;
    CHECK(lardon3d_optical_calibration_profile_create(
        database, &profile, &created_profile) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_tui_optics_inspect_capture(optics, capture.capture_id));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.calibration_count == 1);
    CHECK(view.capture_status == LARDON3D_TUI_OPTICS_SELECTION_REQUIRED);
    CHECK(lardon3d_tui_optics_select_capture_calibration(optics));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.capture_status == LARDON3D_TUI_OPTICS_SELECTED);
    CHECK(view.capture_selection_found);
    CHECK(view.capture_selection.calibration_profile_id
        == created_profile.calibration_profile_id);

    /* Compatible profiles remain bounded but every page is reachable. */
    for (size_t index = 0; index < LARDON3D_TUI_OPTICS_PAGE_CAPACITY; ++index) {
        Lardon3DOpticalCalibrationProfile extra = profile;
        CHECK(snprintf(extra.name, sizeof(extra.name), "Extra profile %02zu",
            index) > 0);
        Lardon3DOpticalCalibrationProfile extra_output;
        CHECK(lardon3d_optical_calibration_profile_create(
            database, &extra, &extra_output) == LARDON3D_PROJECT_DB_OK);
    }
    CHECK(lardon3d_tui_optics_inspect_capture(optics, capture.capture_id));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.calibration_count == LARDON3D_TUI_OPTICS_PAGE_CAPACITY);
    CHECK(view.calibrations_have_next);
    CHECK(lardon3d_tui_optics_select_pane(
        optics, LARDON3D_TUI_OPTICS_PANE_CALIBRATION));
    CHECK(lardon3d_tui_optics_page(
        optics, LARDON3D_TUI_OPTICS_PANE_CALIBRATION, true));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.calibration_count == 1 && !view.calibrations_have_next);
    CHECK(lardon3d_tui_optics_page(
        optics, LARDON3D_TUI_OPTICS_PANE_CALIBRATION, false));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.calibration_count == LARDON3D_TUI_OPTICS_PAGE_CAPACITY);

    Lardon3DOpticalCameraBodyAlias body_alias;
    CHECK(lardon3d_optical_camera_body_alias_add(database, body_id,
        "Exact Make", "Exact Model", &body_alias) == LARDON3D_PROJECT_DB_OK);
    Lardon3DOpticalLensProfile electronic = {
        .interface_kind = LARDON3D_OPTICAL_LENS_ELECTRONIC,
        .focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_ZOOM,
        .minimum_focal_um = 16000,
        .maximum_focal_um = 50000,
    };
    CHECK(copy_text(electronic.manufacturer,
        sizeof(electronic.manufacturer), "Generic"));
    CHECK(copy_text(electronic.model, sizeof(electronic.model), "16-50"));
    CHECK(copy_text(electronic.name, sizeof(electronic.name),
        "Electronic zoom"));
    CHECK(lardon3d_tui_optics_create_lens(optics, &electronic));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    uint64_t electronic_id = view.lenses[0].lens_profile_id;
    Lardon3DOpticalLensAlias lens_alias;
    CHECK(lardon3d_optical_lens_alias_add(database, electronic_id,
        "Exact Lens Make", "Exact Lens", &lens_alias)
        == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_tui_optics_lookup_exact_metadata(optics,
        "Exact Make", "Exact Model", "Exact Lens Make", "Exact Lens"));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.metadata_body_found && view.metadata_lens_found);
    CHECK(view.metadata_lens.lens_profile_id == electronic_id);

    CHECK(lardon3d_tui_optics_lookup_exact_metadata(optics,
        "Exact Make", "Exact Model", "", ""));
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(view.metadata_body_found && !view.metadata_lens_found);
    CHECK(strstr(view.message, "no lens electronics is normal") != NULL);

    lardon3d_tui_optics_unbind(optics);
    CHECK(lardon3d_tui_optics_snapshot(optics, &view));
    CHECK(!view.project_bound);
    lardon3d_tui_optics_destroy(optics);
    lardon3d_project_db_close(database);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
