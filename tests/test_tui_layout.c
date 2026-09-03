#include <ncurses.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/layout.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "TUI layout failure line %d: %s\n",      \
                __LINE__, #condition);                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

size_t
lardon3d_image_catalog_count(const Lardon3DImageCatalog *catalog)
{
    (void)catalog;
    return 0;
}

void
lardon3d_image_catalog_format_size(
    uint64_t size_bytes,
    char *text,
    size_t text_size
)
{
    (void)snprintf(text, text_size, "%llu B",
        (unsigned long long)size_bytes);
}

size_t
lardon3d_image_view_count(const Lardon3DImageView *view)
{
    (void)view;
    return 0;
}

const Lardon3DImageEntry *
lardon3d_image_view_get(const Lardon3DImageView *view, size_t visible_index)
{
    (void)view;
    (void)visible_index;
    return NULL;
}

Lardon3DImageSort
lardon3d_image_view_sort(const Lardon3DImageView *view)
{
    (void)view;
    return LARDON3D_IMAGE_SORT_IMPORT_ORDER;
}

const char *
lardon3d_image_view_filter(const Lardon3DImageView *view)
{
    (void)view;
    return "";
}

const char *
lardon3d_image_view_sort_name(Lardon3DImageSort sort)
{
    (void)sort;
    return "import";
}

size_t
lardon3d_image_view_selection(const Lardon3DImageView *view)
{
    (void)view;
    return 0;
}

size_t
lardon3d_image_view_offset(const Lardon3DImageView *view)
{
    (void)view;
    return 0;
}

const char *
lardon3d_task_state_name(Lardon3DTaskState state)
{
    (void)state;
    return "RUNNING";
}

const char *
lardon3d_ssd_state_name(Lardon3DSsdState state)
{
    static const char *const names[] = {
        "ABSENT", "DETECTED", "ENABLING", "ENABLED", "IN_USE",
        "DRAINING", "SAFE_TO_UNPLUG", "ERROR",
    };
    return state >= LARDON3D_SSD_ABSENT && state <= LARDON3D_SSD_ERROR
        ? names[state] : "UNKNOWN";
}

const char *
lardon3d_tui_ssd_action_name(Lardon3DTuiSsdAction action)
{
    (void)action;
    return "OBSERVE";
}

static bool
separator_intact(int rows)
{
    chtype observed = mvwinch(stdscr, rows - 4, 5);
    return (observed & A_CHARTEXT) == (ACS_HLINE & A_CHARTEXT);
}

static bool
row_contains(int row, int columns, const char *needle)
{
    char line[256] = {0};
    int width = columns < (int)sizeof(line) - 1
        ? columns : (int)sizeof(line) - 1;
    return mvwinnstr(stdscr, row, 0, line, width) != ERR
        && strstr(line, needle) != NULL;
}

static bool
screen_contains(int rows, int columns, const char *needle)
{
    for (int row = 0; row < rows; ++row) {
        if (row_contains(row, columns, needle)) {
            return true;
        }
    }
    return false;
}

static bool
draw_supported_size(
    int rows,
    int columns,
    Lardon3DAppState *state,
    Lardon3DRuntimeSnapshot *runtime,
    Lardon3DTuiSsdAsyncSnapshot *operation,
    Lardon3DTuiOpticsSnapshot *optics,
    Lardon3DTuiPalette *palette,
    Lardon3DTuiInteractionMode interaction_mode
)
{
    CHECK(resizeterm(rows, columns) == OK);
    for (int screen = LARDON3D_SCREEN_HOME;
         screen <= LARDON3D_SCREEN_SSD; ++screen) {
        state->screen = (Lardon3DScreen)screen;
        const char *input = interaction_mode
                == LARDON3D_TUI_INTERACTION_TEXT_INPUT
            ? "50" : NULL;
        lardon3d_layout_draw_runtime(state, input, "test input", NULL,
            runtime, operation, optics, 0, palette, interaction_mode,
            rows, columns);
        CHECK(separator_intact(rows));
        CHECK(row_contains(rows - 2, columns, "F10 SSD"));
        if (screen == LARDON3D_SCREEN_HOME) {
            CHECK(row_contains(8, columns, "NOT_APPLICABLE"));
            if (rows < 30 || columns < 100) {
                CHECK(row_contains(10, columns, "50%"));
                CHECK(row_contains(10, columns, "[####----]"));
            }
        }
        if (screen == LARDON3D_SCREEN_SSD) {
            CHECK(row_contains(4, columns, "SAFE TO UNPLUG"));
        }
    }
    return true;
}

static bool
run_test(void)
{
    CHECK(setenv("TERM", "xterm-256color", 1) == 0);
    FILE *output = tmpfile();
    FILE *input = tmpfile();
    CHECK(output && input);
    SCREEN *screen = newterm(NULL, output, input);
    CHECK(screen);
    (void)set_term(screen);

    Lardon3DAppState state = {.running = true};
    (void)snprintf(state.status_message, sizeof(state.status_message),
        "status");
    Lardon3DRuntimeSnapshot runtime = {
        .task_count = 1,
        .active_task_known = true,
        .active_task_index = 0,
        .active_progress = {
            .present = true,
            .durable_counts_known = true,
            .completed = 2,
            .total = 4,
            .percentage_known = true,
            .percentage = 50,
            .eta_state = LARDON3D_TUI_ETA_CALCULATING,
        },
        .resources = {
            .valid = true,
            .governor_pressure = LARDON3D_RESOURCE_PRESSURE_YELLOW,
            .cpu_logical_total = 8,
            .cpu_active = 2,
            .cpu_admitted_known = true,
            .cpu_admitted = 2,
            .cpu_available = 4,
            .gpu_present = true,
            .gpu_backend = LARDON3D_TUI_GPU_BACKEND_ORB_VULKAN,
            .ram_total_bytes = UINT64_C(16) << 30,
            .ram_available_bytes = UINT64_C(8) << 30,
            .ram_reserve_bytes = UINT64_C(3) << 30,
            .swap_total_known = true,
            .swap_used_known = true,
            .batch_known = true,
            .batch_size = 8,
            .inflight_known = true,
            .inflight_limit = 2,
            .helpers_known = true,
            .helper_limit = 1,
        },
        .ssd_controller_available = true,
        .ssd = {
            .state = LARDON3D_SSD_SAFE_TO_UNPLUG,
            .model_known = true,
            .pairing_valid = true,
        },
    };
    runtime.tasks[0] = (Lardon3DTaskObservation) {
        .id = 1,
        .state = TASK_RUNNING,
        .progress = 50,
        .durable_progress_known = true,
        .durable_completed = 2,
        .durable_total = 4,
    };
    (void)snprintf(runtime.tasks[0].name,
        sizeof(runtime.tasks[0].name), "active");
    (void)snprintf(runtime.resources.governor_reason,
        sizeof(runtime.resources.governor_reason), "caution band");
    (void)snprintf(runtime.resources.cpu_reason,
        sizeof(runtime.resources.cpu_reason), "whole cores");
    (void)snprintf(runtime.resources.gpu_backend_reason,
        sizeof(runtime.resources.gpu_backend_reason), "Vulkan ready");
    for (size_t index = 0; index < LARDON3D_TUI_STAGE_COUNT; ++index) {
        runtime.stages[index].stage = (Lardon3DTuiStage)index;
        runtime.stages[index].state = index == LARDON3D_TUI_STAGE_DENSE
            ? LARDON3D_TUI_STAGE_NOT_APPLICABLE
            : LARDON3D_TUI_STAGE_READY;
    }
    Lardon3DTuiSsdAsyncSnapshot operation = {
        .running = true,
        .action = LARDON3D_TUI_SSD_ACTION_OBSERVE,
    };
    Lardon3DTuiOpticsSnapshot optics = {
        .project_bound = true,
        .capture_status = LARDON3D_TUI_OPTICS_UNRESOLVED,
    };
    Lardon3DTuiPalette palette;
    lardon3d_tui_palette_plan(false, 0, &palette);

    CHECK(draw_supported_size(30, 100, &state, &runtime, &operation,
        &optics, &palette, LARDON3D_TUI_INTERACTION_IDLE));
    CHECK(draw_supported_size(20, 72, &state, &runtime, &operation,
        &optics, &palette, LARDON3D_TUI_INTERACTION_IDLE));
    CHECK(draw_supported_size(15, 60, &state, &runtime, &operation,
        &optics, &palette, LARDON3D_TUI_INTERACTION_IDLE));
    /* This final growth proves resize decisions are not sticky. */
    CHECK(draw_supported_size(30, 100, &state, &runtime, &operation,
        &optics, &palette, LARDON3D_TUI_INTERACTION_IDLE));

    state.screen = LARDON3D_SCREEN_RESOURCES;
    CHECK(resizeterm(30, 100) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(row_contains(16, 100, "Governor SSD: UNREGISTERED"));
    runtime.resources.external_storage_registered = true;
    runtime.resources.external_storage_status =
        LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE;
    runtime.resources.scratch_new_allocations_allowed = false;
    runtime.resources.scratch_total_known = true;
    runtime.resources.scratch_free_known = true;
    runtime.resources.scratch_total_bytes = UINT64_C(400) << 30;
    runtime.resources.scratch_free_bytes = UINT64_C(350) << 30;
    runtime.resources.scratch_leases = 2;
    runtime.resources.external_swap_total_known = true;
    runtime.resources.external_swap_used_known = true;
    runtime.resources.external_swap_total_bytes = UINT64_C(8) << 30;
    runtime.resources.external_swap_used_bytes = UINT64_C(1) << 30;
    (void)snprintf(runtime.resources.external_storage_identity,
        sizeof(runtime.resources.external_storage_identity), "drive-1");
    (void)snprintf(runtime.resources.external_storage_reason,
        sizeof(runtime.resources.external_storage_reason), "drain pending");
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette,
        LARDON3D_TUI_INTERACTION_IDLE, 30, 100);
    CHECK(row_contains(16, 100, "Governor SSD IN_USE"));
    CHECK(row_contains(16, 100, "alloc no"));
    CHECK(row_contains(16, 100, "leases 2"));
    CHECK(row_contains(17, 100, "swap total/used"));
    CHECK(resizeterm(15, 60) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette,
        LARDON3D_TUI_INTERACTION_IDLE, 15, 60);
    CHECK(row_contains(10, 60, "Governor SSD IN_USE"));
    CHECK(row_contains(13, 60, "F10 SSD"));
    CHECK(resizeterm(30, 100) == OK);

    runtime.resources.cpu_admitted_known = false;
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(row_contains(6, 100, "/UNKNOWN/"));
    runtime.resources.cpu_admitted_known = true;

    state.screen = LARDON3D_SCREEN_SSD;
    runtime.ssd_controller_available = false;
    CHECK(resizeterm(15, 60) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        15, 60);
    CHECK(row_contains(4, 60, "UNKNOWN"));
    CHECK(!row_contains(10, 60, "INACTIVE"));
    runtime.ssd_controller_available = true;

    operation.controller_snapshot_actionable = true;
    runtime.ssd = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ERROR,
        .pairing_valid = true,
    };
    (void)snprintf(runtime.ssd.reason, sizeof(runtime.ssd.reason),
        "EXACT HAZARD REASON");
    CHECK(resizeterm(15, 60) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        15, 60);
    CHECK(row_contains(6, 60, "EXACT HAZARD REASON"));

    operation.running = true;
    operation.action = LARDON3D_TUI_SSD_ACTION_ENABLE;
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        15, 60);
    CHECK(row_contains(4, 60, "ENABLING"));
    operation.running = false;

    operation.controller_snapshot_known = true;
    operation.controller_snapshot_actionable = false;
    operation.controller_snapshot = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ERROR,
    };
    runtime.ssd = operation.controller_snapshot;
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        15, 60);
    CHECK(row_contains(7, 60, "UNKNOWN"));
    CHECK(!row_contains(10, 60, "INACTIVE"));
    CHECK(!row_contains(10, 60, "UNMOUNTED"));

    state.screen = LARDON3D_SCREEN_PROJECTS;
    CHECK(resizeterm(30, 100) == OK);
    lardon3d_layout_draw_runtime(&state, "project", "name", NULL, &runtime,
        &operation, &optics, 0, &palette,
        LARDON3D_TUI_INTERACTION_TEXT_INPUT, 30, 100);
    CHECK(row_contains(28, 100, "Enter confirm"));
    CHECK(row_contains(28, 100, "ESC cancel"));
    CHECK(!row_contains(28, 100, "Q quit"));
    CHECK(resizeterm(15, 60) == OK);
    lardon3d_layout_draw_runtime(&state, "project", "name", NULL, &runtime,
        &operation, &optics, 0, &palette,
        LARDON3D_TUI_INTERACTION_TEXT_INPUT, 15, 60);
    CHECK(row_contains(13, 60, "F10 SSD"));
    CHECK(row_contains(13, 60, "Enter confirm"));

    state.screen = LARDON3D_SCREEN_IMPORT;
    Lardon3DImportTaskSnapshot import = {
        .status = LARDON3D_IMPORT_TASK_RUNNING,
        .processed = 1,
        .total = 2,
    };
    CHECK(resizeterm(30, 100) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", &import, &runtime,
        &operation, &optics, 0, &palette,
        LARDON3D_TUI_INTERACTION_IMPORT_RUNNING, 30, 100);
    CHECK(row_contains(28, 100, "X cancel import"));
    CHECK(row_contains(28, 100, "Q/ESC"));
    CHECK(row_contains(9, 100, "X: cancel"));
    CHECK(resizeterm(15, 60) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", &import, &runtime,
        &operation, &optics, 0, &palette,
        LARDON3D_TUI_INTERACTION_IMPORT_RUNNING, 15, 60);
    CHECK(row_contains(13, 60, "F10 SSD"));
    CHECK(row_contains(13, 60, "X cancel"));

    state.screen = LARDON3D_SCREEN_TASKS;
    CHECK(resizeterm(30, 100) == OK);
    runtime.task_count = 1;
    runtime.tasks[0] = (Lardon3DTaskObservation) {
        .id = 7,
        .state = TASK_COMPLETED,
        .progress = 100,
        .has_task_kind = true,
        .task_kind_version = 1,
        .durable_progress_known = true,
        .durable_completed = 2,
        .durable_total = 7,
    };
    (void)snprintf(runtime.tasks[0].name,
        sizeof(runtime.tasks[0].name), "durable incomplete");
    (void)snprintf(runtime.tasks[0].task_kind,
        sizeof(runtime.tasks[0].task_kind), "raw.develop");
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(screen_contains(30, 100, "2/7"));
    CHECK(screen_contains(30, 100, "INTEGRITY ERROR"));
    runtime.tasks[0].durable_progress_known = false;
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(screen_contains(30, 100, "Scientific progress"));
    CHECK(!screen_contains(30, 100, "Runtime progress"));

    state.screen = LARDON3D_SCREEN_HOME;
    runtime.active_task_known = true;
    runtime.active_task_index = 0;
    runtime.active_progress = (Lardon3DTuiProgressView) {
        .present = true,
        .task_id = 7,
        .runtime_percentage = true,
        .percentage_known = true,
        .percentage = 50,
        .throughput_known = true,
        .units_per_second = 2.5,
        .eta_state = LARDON3D_TUI_ETA_CALCULATING,
    };
    runtime.tasks[0].has_task_kind = false;
    runtime.tasks[0].state = TASK_RUNNING;
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(screen_contains(30, 100, "2.5%/s"));
    CHECK(!screen_contains(30, 100, "2.5 unit/s"));

    state.screen = LARDON3D_SCREEN_OPTICS;
    optics = (Lardon3DTuiOpticsSnapshot) {0};
    (void)snprintf(optics.message, sizeof(optics.message),
        "Cannot list camera bodies: BUSY");
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(screen_contains(30, 100, "BUSY"));
    CHECK(screen_contains(30, 100, "R:"));
    CHECK(screen_contains(30, 100, "Project DB"));

    optics = (Lardon3DTuiOpticsSnapshot) {
        .project_bound = true,
        .active_pane = LARDON3D_TUI_OPTICS_PANE_CALIBRATION,
        .calibration_count = LARDON3D_TUI_OPTICS_PAGE_CAPACITY,
        .calibrations_have_next = true,
    };
    optics.calibrations[0].calibration_profile_id = 1;
    optics.calibrations[0].profile_version = 1;
    (void)snprintf(optics.calibrations[0].name,
        sizeof(optics.calibrations[0].name), "first profile");
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        30, 100);
    CHECK(screen_contains(30, 100, "1/16 shown"));
    CHECK(screen_contains(30, 100, ", more"));
    CHECK(screen_contains(30, 100, "[ first"));
    CHECK(screen_contains(30, 100, "] next page"));

    /* The legacy symbol accepts only baseline-sized objects. This call also
     * proves a NULL task array with a nonzero count is safely treated empty. */
    Lardon3DTaskSnapshot legacy = {
        .id = 55,
        .state = TASK_RUNNING,
        .progress = 25,
    };
    Lardon3DTaskQueueSummary legacy_summary = {.running = 1, .total = 1};
    Lardon3DResourceAvailability legacy_resources = {.cpu_available = 1};
    state.screen = LARDON3D_SCREEN_TASKS;
    lardon3d_layout_draw(&state, NULL, "", NULL, &legacy, 1,
        &legacy_summary, &legacy_resources, 30, 100);
    CHECK(screen_contains(30, 100, "#55"));
    lardon3d_layout_draw(&state, NULL, "", NULL, NULL, 1,
        &legacy_summary, &legacy_resources, 30, 100);
    CHECK(screen_contains(30, 100, "No retained"));

    CHECK(resizeterm(14, 59) == OK);
    lardon3d_layout_draw_runtime(&state, NULL, "", NULL, &runtime,
        &operation, &optics, 0, &palette, LARDON3D_TUI_INTERACTION_IDLE,
        14, 59);
    char line[60] = {0};
    CHECK(mvwinnstr(stdscr, 7, 0, line, 59) != ERR);
    CHECK(strstr(line, "Terminal too small") != NULL);

    (void)endwin();
    delscreen(screen);
    CHECK(fclose(input) == 0);
    CHECK(fclose(output) == 0);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
