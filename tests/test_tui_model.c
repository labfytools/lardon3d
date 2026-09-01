#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/tui_model.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "TUI model failure line %d: %s\n",       \
                __LINE__, #condition);                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* tui_model only needs the bounded state label; production supplies the same
 * public Task helper without changing any model decision. */
const char *
lardon3d_task_state_name(Lardon3DTaskState state)
{
    static const char *const names[] = {
        "PENDING", "RUNNING", "PAUSED", "CANCELLED", "FAILED", "COMPLETED",
    };
    return state >= TASK_PENDING && state <= TASK_COMPLETED
        ? names[state] : "UNKNOWN";
}

static Lardon3DTaskObservation
typed_task(const char *kind, Lardon3DTaskState state, unsigned int progress)
{
    Lardon3DTaskObservation task = {
        .id = 7,
        .has_task_kind = true,
        .task_kind_version = 1,
        .progress = progress,
        .state = state,
    };
    (void)snprintf(task.name, sizeof(task.name), "test task");
    (void)snprintf(task.task_kind, sizeof(task.task_kind), "%s", kind);
    (void)snprintf(task.message, sizeof(task.message), "observed");
    return task;
}

static bool
test_viewports_and_palette(void)
{
    CHECK(lardon3d_tui_viewport_classify(30, 100)
        == LARDON3D_TUI_VIEWPORT_FULL);
    CHECK(lardon3d_tui_viewport_classify(20, 72)
        == LARDON3D_TUI_VIEWPORT_COMPACT);
    CHECK(lardon3d_tui_viewport_classify(15, 60)
        == LARDON3D_TUI_VIEWPORT_COMPACT);
    CHECK(lardon3d_tui_viewport_classify(14, 60)
        == LARDON3D_TUI_VIEWPORT_TOO_SMALL);
    CHECK(lardon3d_tui_viewport_classify(15, 59)
        == LARDON3D_TUI_VIEWPORT_TOO_SMALL);

    Lardon3DTuiPalette palette;
    lardon3d_tui_palette_plan(false, 0, &palette);
    CHECK(!palette.color_enabled);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_ERROR] == 0);
    CHECK((palette.attributes[LARDON3D_TUI_SEMANTIC_ERROR]
        & LARDON3D_TUI_STYLE_BOLD) != 0);
    CHECK((palette.attributes[LARDON3D_TUI_SEMANTIC_DIM]
        & LARDON3D_TUI_STYLE_DIM) != 0);

    lardon3d_tui_palette_plan(true, 4, &palette);
    CHECK(palette.color_enabled);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_HEALTHY] == 1);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_ERROR] == 3);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_GPU] == 0);
    lardon3d_tui_palette_plan(true, 16, &palette);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_GPU] == 4);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_CPU] == 5);
    CHECK(palette.color_pair[LARDON3D_TUI_SEMANTIC_SSD] == 6);
    return true;
}

static bool
test_stage_matrix(void)
{
    static const struct {
        const char *kind;
        Lardon3DTuiStage stage;
    } cases[] = {
        {"acquisition_campaign.run", LARDON3D_TUI_STAGE_ACQUISITION},
        {"raw.develop", LARDON3D_TUI_STAGE_RAW},
        {"photo_quality.triage", LARDON3D_TUI_STAGE_QUALITY},
        {"features.extract", LARDON3D_TUI_STAGE_FEATURES},
        {"features.extract.sift", LARDON3D_TUI_STAGE_FEATURES},
        {"features.extract.rootsift", LARDON3D_TUI_STAGE_FEATURES},
        {"visual_index.update", LARDON3D_TUI_STAGE_VISUAL_INDEX},
        {"candidate_pair.generate", LARDON3D_TUI_STAGE_CANDIDATE},
        {"matcher.run", LARDON3D_TUI_STAGE_MATCHER},
        {"geometric_verifier.run", LARDON3D_TUI_STAGE_GV},
        {"track_builder.run", LARDON3D_TUI_STAGE_TRACKS},
        {"sparse_sfm.run", LARDON3D_TUI_STAGE_SPARSE_SFM},
        {"incremental_reconstruction.run", LARDON3D_TUI_STAGE_SPARSE_SFM},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        Lardon3DTaskObservation task = typed_task(
            cases[index].kind, TASK_RUNNING, 25);
        Lardon3DTuiStageView stages[LARDON3D_TUI_STAGE_COUNT];
        lardon3d_tui_stage_views_build(true, &task, 1,
            LARDON3D_RESOURCE_PRESSURE_GREEN, stages);
        CHECK(stages[cases[index].stage].state
            == LARDON3D_TUI_STAGE_RUNNING);
        CHECK(stages[LARDON3D_TUI_STAGE_DENSE].state
            == LARDON3D_TUI_STAGE_NOT_APPLICABLE);
    }

    static const struct {
        Lardon3DTaskState task_state;
        Lardon3DResourcePressure pressure;
        Lardon3DTuiStageState expected;
    } states[] = {
        {TASK_PENDING, LARDON3D_RESOURCE_PRESSURE_GREEN,
            LARDON3D_TUI_STAGE_QUEUED},
        {TASK_PENDING, LARDON3D_RESOURCE_PRESSURE_YELLOW,
            LARDON3D_TUI_STAGE_THROTTLED},
        {TASK_RUNNING, LARDON3D_RESOURCE_PRESSURE_RED,
            LARDON3D_TUI_STAGE_RUNNING},
        {TASK_PAUSED, LARDON3D_RESOURCE_PRESSURE_GREEN,
            LARDON3D_TUI_STAGE_BLOCKED},
        {TASK_CANCELLED, LARDON3D_RESOURCE_PRESSURE_GREEN,
            LARDON3D_TUI_STAGE_BLOCKED},
        {TASK_COMPLETED, LARDON3D_RESOURCE_PRESSURE_GREEN,
            LARDON3D_TUI_STAGE_COMPLETE},
        {TASK_FAILED, LARDON3D_RESOURCE_PRESSURE_GREEN,
            LARDON3D_TUI_STAGE_FAILED},
    };
    for (size_t index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
        Lardon3DTaskObservation task = typed_task(
            "raw.develop", states[index].task_state, 20);
        Lardon3DTuiStageView stages[LARDON3D_TUI_STAGE_COUNT];
        lardon3d_tui_stage_views_build(true, &task, 1,
            states[index].pressure, stages);
        CHECK(stages[LARDON3D_TUI_STAGE_RAW].state
            == states[index].expected);
    }

    /* Live execution wins over a newer pending record of the same kind; the
     * Queue deliberately exposes newest live submissions first. */
    Lardon3DTaskObservation concurrent[2] = {
        typed_task("matcher.run", TASK_PENDING, 0),
        typed_task("matcher.run", TASK_RUNNING, 25),
    };
    Lardon3DTuiStageView concurrent_stages[LARDON3D_TUI_STAGE_COUNT];
    lardon3d_tui_stage_views_build(true, concurrent, 2,
        LARDON3D_RESOURCE_PRESSURE_GREEN, concurrent_stages);
    CHECK(concurrent_stages[LARDON3D_TUI_STAGE_MATCHER].state
        == LARDON3D_TUI_STAGE_RUNNING);

    Lardon3DTuiStageView empty[LARDON3D_TUI_STAGE_COUNT];
    lardon3d_tui_stage_views_build(false, NULL, 0,
        LARDON3D_RESOURCE_PRESSURE_GREEN, empty);
    CHECK(empty[LARDON3D_TUI_STAGE_ACQUISITION].state
        == LARDON3D_TUI_STAGE_NOT_READY);
    CHECK(empty[LARDON3D_TUI_STAGE_DENSE].state
        == LARDON3D_TUI_STAGE_NOT_APPLICABLE);
    return true;
}

static bool
test_progress_eta(void)
{
    Lardon3DTuiProgressTracker tracker = {0};
    Lardon3DTuiProgressView view;
    Lardon3DTuiProgressSample sample = {
        .task_id = 42,
        .monotonic_ns = UINT64_C(1000000000),
        .task_state = TASK_RUNNING,
        .typed_task = true,
        .progress_percent = 20,
        .durable_counts_known = true,
        .durable_completed = 20,
        .durable_total = 100,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.percentage == 20);
    CHECK(view.eta_state == LARDON3D_TUI_ETA_CALCULATING);
    CHECK(view.resumed_prefix_excluded);
    CHECK(!view.throughput_known);

    sample.monotonic_ns += UINT64_C(1000000000);
    sample.durable_completed = 30;
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.eta_state == LARDON3D_TUI_ETA_CALCULATING);
    CHECK(!view.throughput_known);

    sample.monotonic_ns += UINT64_C(1000000000);
    sample.durable_completed = 50;
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.eta_state == LARDON3D_TUI_ETA_KNOWN);
    CHECK(view.eta_seconds == 4);
    CHECK(view.units_per_second > 13.4 && view.units_per_second < 13.6);

    sample.monotonic_ns += UINT64_C(5000000000);
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.eta_state == LARDON3D_TUI_ETA_STALLED);
    sample.pressure_limited = true;
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.eta_state == LARDON3D_TUI_ETA_THROTTLED);

    sample.pressure_limited = false;
    sample.task_state = TASK_COMPLETED;
    sample.durable_completed = 100;
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.percentage == 100);
    CHECK(view.eta_state == LARDON3D_TUI_ETA_COMPLETE);
    CHECK(view.eta_seconds == 0);

    sample = (Lardon3DTuiProgressSample) {
        .task_id = 44,
        .monotonic_ns = UINT64_C(19000000000),
        .task_state = TASK_RUNNING,
        .typed_task = true,
        .progress_percent = 99,
        .durable_counts_known = true,
        .durable_completed = UINT64_MAX - 1,
        .durable_total = UINT64_MAX,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.percentage == 99);

    sample = (Lardon3DTuiProgressSample) {
        .task_id = 45,
        .monotonic_ns = UINT64_C(30000000000),
        .task_state = TASK_PENDING,
        .typed_task = true,
        .progress_percent = 0,
        .durable_counts_known = true,
        .durable_total = 100,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    sample.monotonic_ns += UINT64_C(10000000000);
    sample.task_state = TASK_RUNNING;
    sample.durable_completed = 10;
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(!view.throughput_known
        && view.eta_state == LARDON3D_TUI_ETA_CALCULATING);
    sample.monotonic_ns += UINT64_C(5000000000);
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.eta_state == LARDON3D_TUI_ETA_STALLED);

    sample = (Lardon3DTuiProgressSample) {
        .task_id = 43,
        .monotonic_ns = UINT64_C(20000000000),
        .task_state = TASK_PENDING,
        .progress_percent = 0,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.eta_state == LARDON3D_TUI_ETA_INDETERMINATE);
    CHECK(!view.durable_counts_known);

    sample = (Lardon3DTuiProgressSample) {
        .task_id = 46,
        .monotonic_ns = UINT64_C(40000000000),
        .task_state = TASK_COMPLETED,
        .typed_task = true,
        .progress_percent = 100,
        .durable_counts_known = true,
        .durable_completed = 2,
        .durable_total = 7,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.integrity_error);
    CHECK(view.durable_counts_known && view.completed == 2 && view.total == 7);
    CHECK(view.percentage_known && view.percentage == 28);
    CHECK(view.eta_state == LARDON3D_TUI_ETA_INDETERMINATE);

    sample = (Lardon3DTuiProgressSample) {
        .task_id = 47,
        .monotonic_ns = UINT64_C(41000000000),
        .task_state = TASK_COMPLETED,
        .typed_task = true,
        .progress_percent = 100,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(!view.percentage_known && !view.runtime_percentage);
    CHECK(view.eta_state == LARDON3D_TUI_ETA_INDETERMINATE);

    sample = (Lardon3DTuiProgressSample) {
        .task_id = 48,
        .monotonic_ns = UINT64_C(42000000000),
        .task_state = TASK_COMPLETED,
        .progress_percent = 100,
    };
    CHECK(lardon3d_tui_progress_update(&tracker, &sample, &view));
    CHECK(view.percentage_known && view.percentage == 100);
    CHECK(view.runtime_percentage);
    CHECK(view.eta_state == LARDON3D_TUI_ETA_COMPLETE);
    CHECK(!lardon3d_tui_progress_update(&tracker,
        &(Lardon3DTuiProgressSample) {
            .task_id = 1,
            .progress_percent = 101,
        }, &view));
    return true;
}

static bool
test_durable_stage_truth_and_keys(void)
{
    Lardon3DTaskObservation incomplete = typed_task(
        "raw.develop", TASK_COMPLETED, 100);
    incomplete.durable_progress_known = true;
    incomplete.durable_completed = 2;
    incomplete.durable_total = 7;
    Lardon3DTuiStageView stages[LARDON3D_TUI_STAGE_COUNT];
    lardon3d_tui_stage_views_build(true, &incomplete, 1,
        LARDON3D_RESOURCE_PRESSURE_GREEN, stages);
    CHECK(stages[LARDON3D_TUI_STAGE_RAW].state
        == LARDON3D_TUI_STAGE_BLOCKED);
    CHECK(strstr(stages[LARDON3D_TUI_STAGE_RAW].reason, "2/7") != NULL);

    Lardon3DTaskObservation unknown = typed_task(
        "raw.develop", TASK_COMPLETED, 100);
    lardon3d_tui_stage_views_build(true, &unknown, 1,
        LARDON3D_RESOURCE_PRESSURE_GREEN, stages);
    CHECK(stages[LARDON3D_TUI_STAGE_RAW].state
        == LARDON3D_TUI_STAGE_COMPLETE);
    CHECK(strstr(stages[LARDON3D_TUI_STAGE_RAW].reason,
        "indeterminate") != NULL);

    Lardon3DTuiKeyContract keys = lardon3d_tui_key_contract(
        LARDON3D_TUI_INTERACTION_TEXT_INPUT);
    CHECK(keys.enter && keys.escape && keys.f10);
    CHECK(!keys.cancel_import && !keys.quit && !keys.navigate);
    keys = lardon3d_tui_key_contract(
        LARDON3D_TUI_INTERACTION_IMPORT_RUNNING);
    CHECK(keys.cancel_import && keys.f10);
    CHECK(!keys.enter && !keys.escape && !keys.quit && !keys.navigate);
    keys = lardon3d_tui_key_contract(LARDON3D_TUI_INTERACTION_IDLE);
    CHECK(keys.escape && keys.f10 && keys.quit && keys.navigate);
    CHECK(!keys.enter && !keys.cancel_import);
    return true;
}

static bool
test_labels_and_optics(void)
{
    CHECK(strcmp(lardon3d_tui_pressure_name(
        LARDON3D_RESOURCE_PRESSURE_GREEN), "GREEN") == 0);
    CHECK(strcmp(lardon3d_tui_pressure_name(
        LARDON3D_RESOURCE_PRESSURE_YELLOW), "YELLOW") == 0);
    CHECK(strcmp(lardon3d_tui_pressure_name(
        LARDON3D_RESOURCE_PRESSURE_RED), "RED") == 0);
    for (int backend = LARDON3D_TUI_GPU_BACKEND_UNKNOWN;
         backend <= LARDON3D_TUI_GPU_BACKEND_MIXED; ++backend) {
        CHECK(strcmp(lardon3d_tui_gpu_backend_name(
            (Lardon3DTuiGpuBackendStatus)backend), "UNKNOWN") != 0
            || backend == LARDON3D_TUI_GPU_BACKEND_UNKNOWN);
    }
    CHECK(lardon3d_tui_optics_classify(false, false, false, false, 0,
        false, false) == LARDON3D_TUI_OPTICS_NO_PROJECT);
    CHECK(lardon3d_tui_optics_classify(true, false, false, true, 0,
        false, false) == LARDON3D_TUI_OPTICS_UNRESOLVED);
    CHECK(lardon3d_tui_optics_classify(true, true, true, true, 0,
        false, false) == LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY);
    CHECK(lardon3d_tui_optics_classify(true, true, true, false, 2,
        false, false) == LARDON3D_TUI_OPTICS_SELECTION_REQUIRED);
    CHECK(lardon3d_tui_optics_classify(true, true, true, false, 1,
        true, true) == LARDON3D_TUI_OPTICS_SELECTED);
    CHECK(lardon3d_tui_optics_classify(true, true, true, false, 1,
        true, false) == LARDON3D_TUI_OPTICS_INCOMPATIBLE);
    CHECK(strstr(lardon3d_tui_optics_status_explanation(
        LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY, true),
        "Manual/no-EXIF") != NULL);
    return true;
}

int
main(void)
{
    return test_viewports_and_palette()
            && test_stage_matrix()
            && test_progress_eta()
            && test_durable_stage_truth_and_keys()
            && test_labels_and_optics()
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
