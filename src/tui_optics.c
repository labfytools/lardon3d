#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/tui_optics.h>

struct Lardon3DTuiOptics {
    Lardon3DProjectDb *database;
    Lardon3DTuiOpticsSnapshot view;
    uint64_t body_cursor;
    uint64_t lens_cursor;
    uint64_t configuration_cursor;
    uint64_t calibration_cursor;
};

enum {
    /* One extra row is a bounded look-ahead. A page of exactly sixteen must
     * not advertise a nonexistent next page, and the seventeenth row is not
     * retained until the caller explicitly advances the cursor. */
    TUI_OPTICS_FETCH_CAPACITY = LARDON3D_TUI_OPTICS_PAGE_CAPACITY + 1,
};

static void
set_message(Lardon3DTuiOptics *optics, const char *message)
{
    (void)snprintf(optics->view.message, sizeof(optics->view.message), "%s",
        message ? message : "");
}

static const char *
result_name(Lardon3DProjectDbResult result)
{
    switch (result) {
    case LARDON3D_PROJECT_DB_OK:
        return "OK";
    case LARDON3D_PROJECT_DB_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case LARDON3D_PROJECT_DB_NOT_FOUND:
        return "NOT_FOUND";
    case LARDON3D_PROJECT_DB_BUSY:
        return "BUSY";
    case LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA:
        return "UNSUPPORTED_SCHEMA";
    case LARDON3D_PROJECT_DB_CORRUPT:
        return "CORRUPT";
    case LARDON3D_PROJECT_DB_CONSTRAINT:
        return "CONSTRAINT";
    case LARDON3D_PROJECT_DB_IO_ERROR:
        return "IO_ERROR";
    }
    return "UNKNOWN";
}

static bool
report_result(
    Lardon3DTuiOptics *optics,
    const char *operation,
    Lardon3DProjectDbResult result
)
{
    if (result == LARDON3D_PROJECT_DB_OK) {
        return true;
    }
    (void)snprintf(optics->view.message, sizeof(optics->view.message),
        "%s: %s", operation, result_name(result));
    return false;
}

static bool
load_bodies(Lardon3DTuiOptics *optics, uint64_t cursor)
{
    Lardon3DOpticalCameraBodyProfile
        items[TUI_OPTICS_FETCH_CAPACITY] = {0};
    size_t count = 0;
    uint64_t next = cursor;
    Lardon3DProjectDbResult result = lardon3d_optical_camera_body_list(
        optics->database, cursor, items,
        TUI_OPTICS_FETCH_CAPACITY, &count, &next);
    if (!report_result(optics, "Cannot list camera bodies", result)) {
        return false;
    }
    memcpy(optics->view.bodies, items, sizeof(optics->view.bodies));
    optics->body_cursor = cursor;
    optics->view.body_count = count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY
        ? LARDON3D_TUI_OPTICS_PAGE_CAPACITY : count;
    optics->view.selected_body = 0;
    optics->view.bodies_have_next =
        count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY && next > cursor;
    return true;
}

static bool
load_lenses(Lardon3DTuiOptics *optics, uint64_t cursor)
{
    Lardon3DOpticalLensProfile items[TUI_OPTICS_FETCH_CAPACITY] = {0};
    size_t count = 0;
    uint64_t next = cursor;
    Lardon3DProjectDbResult result = lardon3d_optical_lens_list(
        optics->database, cursor, items,
        TUI_OPTICS_FETCH_CAPACITY, &count, &next);
    if (!report_result(optics, "Cannot list lenses", result)) {
        return false;
    }
    memcpy(optics->view.lenses, items, sizeof(optics->view.lenses));
    optics->lens_cursor = cursor;
    optics->view.lens_count = count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY
        ? LARDON3D_TUI_OPTICS_PAGE_CAPACITY : count;
    optics->view.selected_lens = 0;
    optics->view.lenses_have_next =
        count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY && next > cursor;
    return true;
}

static bool
load_configurations(Lardon3DTuiOptics *optics, uint64_t cursor)
{
    Lardon3DOpticalConfiguration
        items[TUI_OPTICS_FETCH_CAPACITY] = {0};
    size_t count = 0;
    uint64_t next = cursor;
    Lardon3DProjectDbResult result = lardon3d_optical_configuration_list(
        optics->database, cursor, items,
        TUI_OPTICS_FETCH_CAPACITY, &count, &next);
    if (!report_result(optics, "Cannot list optical configurations", result)) {
        return false;
    }
    memcpy(optics->view.configurations, items,
        sizeof(optics->view.configurations));
    optics->configuration_cursor = cursor;
    optics->view.configuration_count =
        count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY
            ? LARDON3D_TUI_OPTICS_PAGE_CAPACITY : count;
    optics->view.selected_configuration = 0;
    optics->view.configurations_have_next =
        count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY && next > cursor;
    return true;
}

static bool
load_calibrations(Lardon3DTuiOptics *optics, uint64_t cursor)
{
    if (!optics->view.capture_configuration_found) {
        set_message(optics,
            "Inspect a configured Capture before paging calibrations.");
        return false;
    }
    Lardon3DOpticalCalibrationProfile
        items[TUI_OPTICS_FETCH_CAPACITY] = {0};
    size_t count = 0;
    uint64_t next = cursor;
    Lardon3DProjectDbResult result =
        lardon3d_optical_calibration_profile_list_compatible(
            optics->database,
            optics->view.capture_configuration.optical_configuration_id,
            cursor, items, TUI_OPTICS_FETCH_CAPACITY,
            &count, &next);
    if (!report_result(optics, "Cannot list compatible calibrations", result)) {
        return false;
    }
    memcpy(optics->view.calibrations, items,
        sizeof(optics->view.calibrations));
    optics->calibration_cursor = cursor;
    optics->view.calibration_count =
        count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY
            ? LARDON3D_TUI_OPTICS_PAGE_CAPACITY : count;
    optics->view.selected_calibration = 0;
    optics->view.calibrations_have_next =
        count > LARDON3D_TUI_OPTICS_PAGE_CAPACITY && next > cursor;
    return true;
}

Lardon3DTuiOptics *
lardon3d_tui_optics_create(void)
{
    Lardon3DTuiOptics *optics = calloc(1, sizeof(*optics));
    if (optics) {
        optics->view.capture_status = LARDON3D_TUI_OPTICS_NO_PROJECT;
        set_message(optics, "Load a project to inspect optical profiles.");
    }
    return optics;
}

void
lardon3d_tui_optics_destroy(Lardon3DTuiOptics *optics)
{
    free(optics);
}

void
lardon3d_tui_optics_unbind(Lardon3DTuiOptics *optics)
{
    if (!optics) {
        return;
    }
    *optics = (Lardon3DTuiOptics) {0};
    optics->view.capture_status = LARDON3D_TUI_OPTICS_NO_PROJECT;
    set_message(optics, "Load a project to inspect optical profiles.");
}

bool
lardon3d_tui_optics_bind(
    Lardon3DTuiOptics *optics,
    Lardon3DProjectDb *database
)
{
    if (!optics || !database) {
        return false;
    }
    lardon3d_tui_optics_unbind(optics);
    optics->database = database;
    optics->view.project_bound = true;
    optics->view.capture_status = LARDON3D_TUI_OPTICS_UNRESOLVED;
    if (!load_bodies(optics, 0) || !load_lenses(optics, 0)
        || !load_configurations(optics, 0)) {
        optics->database = NULL;
        optics->view.project_bound = false;
        optics->view.capture_status = LARDON3D_TUI_OPTICS_CORRUPT;
        return false;
    }
    set_message(optics,
        "Profiles are immutable; edit by creating and selecting a new version.");
    return true;
}

bool
lardon3d_tui_optics_snapshot(
    const Lardon3DTuiOptics *optics,
    Lardon3DTuiOpticsSnapshot *snapshot
)
{
    if (snapshot) {
        *snapshot = (Lardon3DTuiOpticsSnapshot) {0};
    }
    if (!optics || !snapshot) {
        return false;
    }
    *snapshot = optics->view;
    return true;
}

bool
lardon3d_tui_optics_select_pane(
    Lardon3DTuiOptics *optics,
    Lardon3DTuiOpticsPane pane
)
{
    if (!optics || pane < LARDON3D_TUI_OPTICS_PANE_BODY
        || pane > LARDON3D_TUI_OPTICS_PANE_CALIBRATION) {
        return false;
    }
    optics->view.active_pane = pane;
    return true;
}

bool
lardon3d_tui_optics_move_selection(
    Lardon3DTuiOptics *optics,
    int direction
)
{
    if (!optics || (direction != -1 && direction != 1)) {
        return false;
    }
    size_t *selection = NULL;
    size_t count = 0;
    switch (optics->view.active_pane) {
    case LARDON3D_TUI_OPTICS_PANE_BODY:
        selection = &optics->view.selected_body;
        count = optics->view.body_count;
        break;
    case LARDON3D_TUI_OPTICS_PANE_LENS:
        selection = &optics->view.selected_lens;
        count = optics->view.lens_count;
        break;
    case LARDON3D_TUI_OPTICS_PANE_CONFIGURATION:
        selection = &optics->view.selected_configuration;
        count = optics->view.configuration_count;
        break;
    case LARDON3D_TUI_OPTICS_PANE_CALIBRATION:
        selection = &optics->view.selected_calibration;
        count = optics->view.calibration_count;
        break;
    }
    if (!selection || count == 0) {
        return false;
    }
    if (direction < 0) {
        if (*selection == 0) {
            return false;
        }
        --*selection;
    } else {
        if (*selection + 1 >= count) {
            return false;
        }
        ++*selection;
    }
    return true;
}

bool
lardon3d_tui_optics_page(
    Lardon3DTuiOptics *optics,
    Lardon3DTuiOpticsPane pane,
    bool next
)
{
    if (!optics || !optics->database) {
        return false;
    }
    uint64_t cursor = 0;
    if (next) {
        switch (pane) {
        case LARDON3D_TUI_OPTICS_PANE_BODY:
            if (!optics->view.bodies_have_next || optics->view.body_count == 0)
                return false;
            cursor = optics->view.bodies[optics->view.body_count - 1]
                .camera_body_profile_id;
            break;
        case LARDON3D_TUI_OPTICS_PANE_LENS:
            if (!optics->view.lenses_have_next || optics->view.lens_count == 0)
                return false;
            cursor = optics->view.lenses[optics->view.lens_count - 1]
                .lens_profile_id;
            break;
        case LARDON3D_TUI_OPTICS_PANE_CONFIGURATION:
            if (!optics->view.configurations_have_next
                || optics->view.configuration_count == 0) return false;
            cursor = optics->view.configurations[
                optics->view.configuration_count - 1].optical_configuration_id;
            break;
        case LARDON3D_TUI_OPTICS_PANE_CALIBRATION:
            if (!optics->view.calibrations_have_next
                || optics->view.calibration_count == 0) return false;
            cursor = optics->view.calibrations[
                optics->view.calibration_count - 1].calibration_profile_id;
            break;
        }
    }
    switch (pane) {
    case LARDON3D_TUI_OPTICS_PANE_BODY:
        return load_bodies(optics, cursor);
    case LARDON3D_TUI_OPTICS_PANE_LENS:
        return load_lenses(optics, cursor);
    case LARDON3D_TUI_OPTICS_PANE_CONFIGURATION:
        return load_configurations(optics, cursor);
    case LARDON3D_TUI_OPTICS_PANE_CALIBRATION:
        return load_calibrations(optics, cursor);
    }
    return false;
}

bool
lardon3d_tui_optics_create_body(
    Lardon3DTuiOptics *optics,
    const Lardon3DOpticalCameraBodyProfile *input
)
{
    if (!optics || !optics->database || !input) {
        return false;
    }
    Lardon3DOpticalCameraBodyProfile output;
    Lardon3DProjectDbResult result = lardon3d_optical_camera_body_create(
        optics->database, input, &output);
    if (!report_result(optics, "Cannot create immutable camera body", result)) {
        return false;
    }
    /* The create result is already a validated caller-owned copy. Keep that
     * exact immutable identity selected even when its ID lies beyond the
     * first bounded list page; `[` remains the explicit return-to-first-page. */
    optics->view.bodies[0] = output;
    optics->view.body_count = 1;
    optics->view.selected_body = 0;
    optics->view.bodies_have_next = false;
    optics->body_cursor = output.camera_body_profile_id > 0
        ? output.camera_body_profile_id - 1 : 0;
    (void)snprintf(optics->view.message, sizeof(optics->view.message),
        "Immutable camera body #%llu selected/created.",
        (unsigned long long)output.camera_body_profile_id);
    return true;
}

bool
lardon3d_tui_optics_create_lens(
    Lardon3DTuiOptics *optics,
    const Lardon3DOpticalLensProfile *input
)
{
    if (!optics || !optics->database || !input) {
        return false;
    }
    Lardon3DOpticalLensProfile output;
    Lardon3DProjectDbResult result = lardon3d_optical_lens_create(
        optics->database, input, &output);
    if (!report_result(optics, "Cannot create immutable lens", result)) {
        return false;
    }
    optics->view.lenses[0] = output;
    optics->view.lens_count = 1;
    optics->view.selected_lens = 0;
    optics->view.lenses_have_next = false;
    optics->lens_cursor = output.lens_profile_id > 0
        ? output.lens_profile_id - 1 : 0;
    (void)snprintf(optics->view.message, sizeof(optics->view.message),
        "Immutable lens #%llu selected/created%s.",
        (unsigned long long)output.lens_profile_id,
        output.interface_kind == LARDON3D_OPTICAL_LENS_MANUAL
            ? " (manual/no EXIF is normal)" : "");
    return true;
}

static bool
selected_body_lens(
    const Lardon3DTuiOptics *optics,
    uint64_t *body_id,
    uint64_t *lens_id
)
{
    if (!optics || !body_id || !lens_id
        || optics->view.selected_body >= optics->view.body_count
        || optics->view.selected_lens >= optics->view.lens_count) {
        return false;
    }
    *body_id = optics->view.bodies[optics->view.selected_body]
        .camera_body_profile_id;
    *lens_id = optics->view.lenses[optics->view.selected_lens]
        .lens_profile_id;
    return *body_id != 0 && *lens_id != 0;
}

static bool
selected_configuration(
    const Lardon3DTuiOptics *optics,
    uint64_t *configuration_id
)
{
    if (!optics || !configuration_id
        || optics->view.selected_configuration
            >= optics->view.configuration_count) {
        return false;
    }
    *configuration_id = optics->view.configurations[
        optics->view.selected_configuration].optical_configuration_id;
    return *configuration_id != 0;
}

bool
lardon3d_tui_optics_create_configuration(
    Lardon3DTuiOptics *optics,
    bool has_focal_length,
    uint32_t focal_length_um
)
{
    uint64_t body_id;
    uint64_t lens_id;
    if (!optics || !optics->database
        || !selected_body_lens(optics, &body_id, &lens_id)) {
        if (optics) set_message(optics, "Select an existing body and lens first.");
        return false;
    }
    Lardon3DOpticalConfiguration input = {
        .camera_body_profile_id = body_id,
        .lens_profile_id = lens_id,
        .has_focal_length = has_focal_length,
        .focal_length_um = focal_length_um,
    };
    Lardon3DOpticalConfiguration output;
    Lardon3DProjectDbResult result = lardon3d_optical_configuration_create(
        optics->database, &input, &output);
    if (!report_result(optics, "Cannot create immutable configuration", result)) {
        return false;
    }
    optics->view.configurations[0] = output;
    optics->view.configuration_count = 1;
    optics->view.selected_configuration = 0;
    optics->view.configurations_have_next = false;
    optics->configuration_cursor = output.optical_configuration_id > 0
        ? output.optical_configuration_id - 1 : 0;
    (void)snprintf(optics->view.message, sizeof(optics->view.message),
        "Immutable optical configuration #%llu selected/created.",
        (unsigned long long)output.optical_configuration_id);
    return true;
}

bool
lardon3d_tui_optics_assign_campaign_group(
    Lardon3DTuiOptics *optics,
    uint64_t campaign_task_id,
    uint32_t group_id
)
{
    uint64_t configuration_id;
    if (!optics || !optics->database
        || !selected_configuration(optics, &configuration_id)) {
        if (optics) set_message(optics, "Select an optical configuration first.");
        return false;
    }
    Lardon3DProjectDbResult result = lardon3d_optical_campaign_group_assign(
        optics->database, campaign_task_id, group_id, configuration_id);
    if (!report_result(optics, "Cannot assign campaign group", result)) {
        return false;
    }
    (void)snprintf(optics->view.message, sizeof(optics->view.message),
        "Campaign Task #%llu group %u assigned to configuration #%llu.",
        (unsigned long long)campaign_task_id, group_id,
        (unsigned long long)configuration_id);
    return true;
}

bool
lardon3d_tui_optics_assign_capture(
    Lardon3DTuiOptics *optics,
    uint64_t capture_id
)
{
    uint64_t configuration_id;
    if (!optics || !optics->database
        || !selected_configuration(optics, &configuration_id)) {
        if (optics) set_message(optics, "Select an optical configuration first.");
        return false;
    }
    Lardon3DProjectDbResult result =
        lardon3d_optical_capture_assign_explicit(
            optics->database, capture_id, configuration_id);
    if (!report_result(optics, "Cannot assign Capture", result)) {
        return false;
    }
    return lardon3d_tui_optics_inspect_capture(optics, capture_id);
}

static void
clear_capture_view(Lardon3DTuiOptics *optics, uint64_t capture_id)
{
    optics->view.capture_inspected = true;
    optics->view.capture_id = capture_id;
    optics->view.capture_status = LARDON3D_TUI_OPTICS_UNRESOLVED;
    optics->view.capture_assignment_found = false;
    optics->view.capture_assignment = (Lardon3DOpticalCaptureAssignment) {0};
    optics->view.capture_configuration_found = false;
    optics->view.capture_configuration = (Lardon3DOpticalConfiguration) {0};
    optics->view.capture_body_found = false;
    optics->view.capture_body = (Lardon3DOpticalCameraBodyProfile) {0};
    optics->view.capture_lens_found = false;
    optics->view.capture_lens = (Lardon3DOpticalLensProfile) {0};
    optics->view.capture_selection_found = false;
    optics->view.capture_selection =
        (Lardon3DOpticalCaptureCalibrationSelection) {0};
    optics->view.calibration_count = 0;
    optics->view.selected_calibration = 0;
    optics->view.calibrations_have_next = false;
    optics->calibration_cursor = 0;
}

bool
lardon3d_tui_optics_inspect_capture(
    Lardon3DTuiOptics *optics,
    uint64_t capture_id
)
{
    if (!optics || !optics->database || capture_id == 0) {
        return false;
    }
    clear_capture_view(optics, capture_id);
    Lardon3DProjectDbCapture capture;
    Lardon3DProjectDbResult result = lardon3d_project_db_load_capture(
        optics->database, capture_id, &capture);
    if (!report_result(optics, "Cannot inspect Capture identity", result)) {
        optics->view.capture_status = result == LARDON3D_PROJECT_DB_CORRUPT
            ? LARDON3D_TUI_OPTICS_CORRUPT
            : LARDON3D_TUI_OPTICS_UNRESOLVED;
        return false;
    }
    result =
        lardon3d_optical_capture_assignment_load(
            optics->database, capture_id, &optics->view.capture_assignment);
    if (result == LARDON3D_PROJECT_DB_NOT_FOUND) {
        set_message(optics, lardon3d_tui_optics_status_explanation(
            LARDON3D_TUI_OPTICS_UNRESOLVED, false));
        return true;
    }
    if (!report_result(optics, "Cannot inspect Capture assignment", result)) {
        optics->view.capture_status = result == LARDON3D_PROJECT_DB_CORRUPT
            ? LARDON3D_TUI_OPTICS_CORRUPT
            : LARDON3D_TUI_OPTICS_UNRESOLVED;
        return false;
    }
    optics->view.capture_assignment_found = true;
    result = lardon3d_optical_configuration_load(
        optics->database,
        optics->view.capture_assignment.optical_configuration_id,
        &optics->view.capture_configuration);
    if (!report_result(optics, "Cannot load Capture configuration", result)) {
        optics->view.capture_status = LARDON3D_TUI_OPTICS_CORRUPT;
        return false;
    }
    optics->view.capture_configuration_found = true;
    result = lardon3d_optical_camera_body_load(
        optics->database,
        optics->view.capture_configuration.camera_body_profile_id,
        &optics->view.capture_body);
    if (!report_result(optics, "Cannot load Capture camera body", result)) {
        optics->view.capture_status = LARDON3D_TUI_OPTICS_CORRUPT;
        return false;
    }
    optics->view.capture_body_found = true;
    result = lardon3d_optical_lens_load(
        optics->database, optics->view.capture_configuration.lens_profile_id,
        &optics->view.capture_lens);
    if (!report_result(optics, "Cannot load Capture lens", result)) {
        optics->view.capture_status = LARDON3D_TUI_OPTICS_CORRUPT;
        return false;
    }
    optics->view.capture_lens_found = true;

    if (!load_calibrations(optics, 0)) {
        optics->view.capture_status = LARDON3D_TUI_OPTICS_CORRUPT;
        return false;
    }
    result = lardon3d_optical_capture_calibration_selection_load(
        optics->database, capture_id, &optics->view.capture_selection);
    bool selection_compatible = false;
    if (result == LARDON3D_PROJECT_DB_OK) {
        optics->view.capture_selection_found = true;
        selection_compatible =
            optics->view.capture_selection.optical_configuration_id
                == optics->view.capture_assignment.optical_configuration_id;
    } else if (result != LARDON3D_PROJECT_DB_NOT_FOUND) {
        report_result(optics, "Cannot load Capture calibration selection", result);
        optics->view.capture_status = result == LARDON3D_PROJECT_DB_CORRUPT
            ? LARDON3D_TUI_OPTICS_CORRUPT
            : LARDON3D_TUI_OPTICS_INCOMPATIBLE;
        return false;
    }
    bool manual = optics->view.capture_lens.interface_kind
        == LARDON3D_OPTICAL_LENS_MANUAL;
    optics->view.capture_status = lardon3d_tui_optics_classify(
        true, true, true, manual, optics->view.calibration_count,
        optics->view.capture_selection_found, selection_compatible);
    set_message(optics, lardon3d_tui_optics_status_explanation(
        optics->view.capture_status, manual));
    return true;
}

bool
lardon3d_tui_optics_select_capture_calibration(
    Lardon3DTuiOptics *optics
)
{
    if (!optics || !optics->database || !optics->view.capture_inspected
        || optics->view.selected_calibration
            >= optics->view.calibration_count) {
        if (optics) set_message(optics,
            "Inspect a configured Capture and select a compatible calibration.");
        return false;
    }
    uint64_t profile_id = optics->view.calibrations[
        optics->view.selected_calibration].calibration_profile_id;
    Lardon3DProjectDbResult result =
        lardon3d_optical_capture_calibration_select(
            optics->database, optics->view.capture_id, profile_id);
    if (!report_result(optics, "Cannot select Capture calibration", result)) {
        return false;
    }
    return lardon3d_tui_optics_inspect_capture(
        optics, optics->view.capture_id);
}

bool
lardon3d_tui_optics_lookup_exact_metadata(
    Lardon3DTuiOptics *optics,
    const char *body_make,
    const char *body_model,
    const char *lens_make,
    const char *lens_model
)
{
    if (!optics || !optics->database || !body_make || !body_model
        || !lens_make || !lens_model) {
        return false;
    }
    optics->view.metadata_lookup_performed = true;
    optics->view.metadata_body_found = false;
    optics->view.metadata_lens_found = false;
    optics->view.metadata_body = (Lardon3DOpticalCameraBodyProfile) {0};
    optics->view.metadata_lens = (Lardon3DOpticalLensProfile) {0};

    Lardon3DProjectDbResult body_result = LARDON3D_PROJECT_DB_NOT_FOUND;
    if (body_make[0] && body_model[0]) {
        body_result = lardon3d_optical_camera_body_find_exact_alias(
            optics->database, body_make, body_model,
            &optics->view.metadata_body);
        if (body_result == LARDON3D_PROJECT_DB_OK) {
            optics->view.metadata_body_found = true;
        } else if (body_result != LARDON3D_PROJECT_DB_NOT_FOUND) {
            return report_result(optics, "Camera metadata lookup failed",
                body_result);
        }
    }
    Lardon3DProjectDbResult lens_result = LARDON3D_PROJECT_DB_NOT_FOUND;
    if (lens_model[0]) {
        lens_result = lardon3d_optical_lens_find_exact_alias(
            optics->database, lens_make, lens_model,
            &optics->view.metadata_lens);
        if (lens_result == LARDON3D_PROJECT_DB_OK) {
            optics->view.metadata_lens_found = true;
        } else if (lens_result != LARDON3D_PROJECT_DB_NOT_FOUND) {
            return report_result(optics, "Lens metadata lookup failed",
                lens_result);
        }
    }
    (void)snprintf(optics->view.message, sizeof(optics->view.message),
        "Exact metadata: body %s, lens %s%s.",
        optics->view.metadata_body_found ? "MATCH" : "UNRESOLVED",
        optics->view.metadata_lens_found ? "MATCH" : "UNRESOLVED",
        lens_model[0] ? "" : " (no lens electronics is normal)");
    return true;
}
