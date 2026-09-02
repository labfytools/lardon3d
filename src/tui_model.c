#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/tui_model.h>

enum {
    TUI_COMPACT_MINIMUM_ROWS = 15,
    TUI_COMPACT_MINIMUM_COLUMNS = 60,
    TUI_FULL_MINIMUM_ROWS = 30,
    TUI_FULL_MINIMUM_COLUMNS = 100,
};

static void
copy_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) {
        return;
    }
    (void)snprintf(destination, capacity, "%s", source ? source : "");
}

Lardon3DTuiViewport
lardon3d_tui_viewport_classify(int rows, int columns)
{
    if (rows < TUI_COMPACT_MINIMUM_ROWS
        || columns < TUI_COMPACT_MINIMUM_COLUMNS) {
        return LARDON3D_TUI_VIEWPORT_TOO_SMALL;
    }
    if (rows >= TUI_FULL_MINIMUM_ROWS
        && columns >= TUI_FULL_MINIMUM_COLUMNS) {
        return LARDON3D_TUI_VIEWPORT_FULL;
    }
    return LARDON3D_TUI_VIEWPORT_COMPACT;
}

void
lardon3d_tui_palette_plan(
    bool colors_supported,
    int color_pairs,
    Lardon3DTuiPalette *palette
)
{
    if (!palette) {
        return;
    }
    *palette = (Lardon3DTuiPalette) {0};
    palette->attributes[LARDON3D_TUI_SEMANTIC_HEALTHY] =
        LARDON3D_TUI_STYLE_BOLD;
    palette->attributes[LARDON3D_TUI_SEMANTIC_WARNING] =
        LARDON3D_TUI_STYLE_BOLD;
    palette->attributes[LARDON3D_TUI_SEMANTIC_ERROR] =
        LARDON3D_TUI_STYLE_BOLD;
    palette->attributes[LARDON3D_TUI_SEMANTIC_GPU] =
        LARDON3D_TUI_STYLE_BOLD;
    palette->attributes[LARDON3D_TUI_SEMANTIC_CPU] =
        LARDON3D_TUI_STYLE_BOLD;
    palette->attributes[LARDON3D_TUI_SEMANTIC_SSD] =
        LARDON3D_TUI_STYLE_BOLD;
    palette->attributes[LARDON3D_TUI_SEMANTIC_DIM] =
        LARDON3D_TUI_STYLE_DIM;

    if (!colors_supported || color_pairs <= 1) {
        return;
    }

    static const Lardon3DTuiSemantic colored[] = {
        LARDON3D_TUI_SEMANTIC_HEALTHY,
        LARDON3D_TUI_SEMANTIC_WARNING,
        LARDON3D_TUI_SEMANTIC_ERROR,
        LARDON3D_TUI_SEMANTIC_GPU,
        LARDON3D_TUI_SEMANTIC_CPU,
        LARDON3D_TUI_SEMANTIC_SSD,
    };
    palette->color_enabled = true;
    for (size_t index = 0; index < sizeof(colored) / sizeof(colored[0]);
         ++index) {
        int pair = (int)index + 1;
        if (pair < color_pairs) {
            palette->color_pair[colored[index]] = (short)pair;
        }
    }
}

const char *
lardon3d_tui_stage_name(Lardon3DTuiStage stage)
{
    static const char *const names[LARDON3D_TUI_STAGE_COUNT] = {
        "Acquisition",
        "RAW",
        "Quality",
        "Features",
        "Visual Index",
        "Candidate",
        "Matcher",
        "GV",
        "Tracks",
        "Sparse SfM",
        "Dense (future)",
    };
    return stage >= LARDON3D_TUI_STAGE_ACQUISITION
            && stage <= LARDON3D_TUI_STAGE_DENSE
        ? names[stage]
        : "Unknown";
}

const char *
lardon3d_tui_stage_state_name(Lardon3DTuiStageState state)
{
    switch (state) {
    case LARDON3D_TUI_STAGE_NOT_READY:
        return "NOT_READY";
    case LARDON3D_TUI_STAGE_READY:
        return "READY";
    case LARDON3D_TUI_STAGE_QUEUED:
        return "QUEUED";
    case LARDON3D_TUI_STAGE_RUNNING:
        return "RUNNING";
    case LARDON3D_TUI_STAGE_THROTTLED:
        return "THROTTLED";
    case LARDON3D_TUI_STAGE_BLOCKED:
        return "BLOCKED";
    case LARDON3D_TUI_STAGE_COMPLETE:
        return "COMPLETE";
    case LARDON3D_TUI_STAGE_FAILED:
        return "FAILED";
    case LARDON3D_TUI_STAGE_NOT_APPLICABLE:
        return "NOT_APPLICABLE";
    }
    return "UNKNOWN";
}

Lardon3DTuiSemantic
lardon3d_tui_stage_semantic(Lardon3DTuiStageState state)
{
    switch (state) {
    case LARDON3D_TUI_STAGE_READY:
    case LARDON3D_TUI_STAGE_COMPLETE:
        return LARDON3D_TUI_SEMANTIC_HEALTHY;
    case LARDON3D_TUI_STAGE_QUEUED:
    case LARDON3D_TUI_STAGE_RUNNING:
        return LARDON3D_TUI_SEMANTIC_CPU;
    case LARDON3D_TUI_STAGE_THROTTLED:
    case LARDON3D_TUI_STAGE_BLOCKED:
        return LARDON3D_TUI_SEMANTIC_WARNING;
    case LARDON3D_TUI_STAGE_FAILED:
        return LARDON3D_TUI_SEMANTIC_ERROR;
    case LARDON3D_TUI_STAGE_NOT_READY:
    case LARDON3D_TUI_STAGE_NOT_APPLICABLE:
        return LARDON3D_TUI_SEMANTIC_DIM;
    }
    return LARDON3D_TUI_SEMANTIC_NORMAL;
}

static bool
kind_to_stage(
    const Lardon3DTaskObservation *task,
    Lardon3DTuiStage *stage
)
{
    if (!task || !stage || !task->has_task_kind
        || task->task_kind_version != 1) {
        return false;
    }
    const char *kind = task->task_kind;
    if (strcmp(kind, "import.images") == 0
        || strcmp(kind, "acquisition_campaign.run") == 0) {
        *stage = LARDON3D_TUI_STAGE_ACQUISITION;
    } else if (strcmp(kind, "raw.develop") == 0 ||
               strcmp(kind, "raw.develop.batch") == 0) {
        *stage = LARDON3D_TUI_STAGE_RAW;
    } else if (strcmp(kind, "photo_quality.triage") == 0) {
        *stage = LARDON3D_TUI_STAGE_QUALITY;
    } else if (strcmp(kind, "features.extract") == 0
        || strcmp(kind, "features.extract.batch") == 0
        || strcmp(kind, "features.extract.sift") == 0
        || strcmp(kind, "features.extract.rootsift") == 0) {
        *stage = LARDON3D_TUI_STAGE_FEATURES;
    } else if (strcmp(kind, "visual_index.update") == 0) {
        *stage = LARDON3D_TUI_STAGE_VISUAL_INDEX;
    } else if (strcmp(kind, "candidate_pair.generate") == 0) {
        *stage = LARDON3D_TUI_STAGE_CANDIDATE;
    } else if (strcmp(kind, "matcher.run") == 0) {
        *stage = LARDON3D_TUI_STAGE_MATCHER;
    } else if (strcmp(kind, "geometric_verifier.run") == 0) {
        *stage = LARDON3D_TUI_STAGE_GV;
    } else if (strcmp(kind, "track_builder.run") == 0) {
        *stage = LARDON3D_TUI_STAGE_TRACKS;
    } else if (strcmp(kind, "sparse_sfm.run") == 0
        || strcmp(kind, "incremental_reconstruction.run") == 0) {
        *stage = LARDON3D_TUI_STAGE_SPARSE_SFM;
    } else {
        return false;
    }
    return true;
}

static Lardon3DTuiStageState
task_stage_state(
    const Lardon3DTaskObservation *task,
    Lardon3DResourcePressure pressure
)
{
    switch (task->state) {
    case TASK_PENDING:
        return pressure == LARDON3D_RESOURCE_PRESSURE_GREEN
            ? LARDON3D_TUI_STAGE_QUEUED
            : LARDON3D_TUI_STAGE_THROTTLED;
    case TASK_RUNNING:
        return LARDON3D_TUI_STAGE_RUNNING;
    case TASK_PAUSED:
    case TASK_CANCELLED:
        return LARDON3D_TUI_STAGE_BLOCKED;
    case TASK_FAILED:
        return LARDON3D_TUI_STAGE_FAILED;
    case TASK_COMPLETED:
        if (task->durable_progress_known
            && task->durable_completed < task->durable_total) {
            return LARDON3D_TUI_STAGE_BLOCKED;
        }
        return LARDON3D_TUI_STAGE_COMPLETE;
    }
    return LARDON3D_TUI_STAGE_NOT_READY;
}

static unsigned int
task_stage_relevance(Lardon3DTaskState state)
{
    /* Queue snapshots are newest-first, but a newly submitted pending Task
     * must not hide an older Task that is actually executing in the same
     * stage. Equal relevance retains the first/newest observation. */
    switch (state) {
    case TASK_RUNNING:
        return 3;
    case TASK_PAUSED:
        return 2;
    case TASK_PENDING:
        return 1;
    case TASK_CANCELLED:
    case TASK_FAILED:
    case TASK_COMPLETED:
        return 0;
    }
    return 0;
}

void
lardon3d_tui_stage_views_build(
    bool project_loaded,
    const Lardon3DTaskObservation *tasks,
    size_t task_count,
    Lardon3DResourcePressure pressure,
    Lardon3DTuiStageView stages[LARDON3D_TUI_STAGE_COUNT]
)
{
    if (!stages) {
        return;
    }
    for (size_t index = 0; index < LARDON3D_TUI_STAGE_COUNT; ++index) {
        stages[index] = (Lardon3DTuiStageView) {
            .stage = (Lardon3DTuiStage)index,
            .state = LARDON3D_TUI_STAGE_NOT_READY,
        };
        copy_text(stages[index].reason, sizeof(stages[index].reason),
            project_loaded ? "No eligible runtime Task observed"
                           : "Load a project first");
    }
    stages[LARDON3D_TUI_STAGE_DENSE].state =
        LARDON3D_TUI_STAGE_NOT_APPLICABLE;
    copy_text(stages[LARDON3D_TUI_STAGE_DENSE].reason,
        sizeof(stages[LARDON3D_TUI_STAGE_DENSE].reason),
        "Future stage; no production execution is exposed");
    if (project_loaded) {
        stages[LARDON3D_TUI_STAGE_ACQUISITION].state =
            LARDON3D_TUI_STAGE_READY;
        copy_text(stages[LARDON3D_TUI_STAGE_ACQUISITION].reason,
            sizeof(stages[LARDON3D_TUI_STAGE_ACQUISITION].reason),
            "Project is ready for acquisition/import work");
    }
    if (!tasks || task_count == 0) {
        return;
    }

    bool observed[LARDON3D_TUI_STAGE_COUNT] = {0};
    unsigned int relevance[LARDON3D_TUI_STAGE_COUNT] = {0};
    for (size_t index = 0; index < task_count; ++index) {
        Lardon3DTuiStage stage;
        if (!kind_to_stage(&tasks[index], &stage)) {
            continue;
        }
        unsigned int candidate_relevance = task_stage_relevance(
            tasks[index].state);
        if (observed[stage] && candidate_relevance <= relevance[stage]) {
            continue;
        }
        observed[stage] = true;
        relevance[stage] = candidate_relevance;
        stages[stage].state = task_stage_state(&tasks[index], pressure);
        if (tasks[index].state == TASK_COMPLETED
            && tasks[index].durable_progress_known
            && tasks[index].durable_completed < tasks[index].durable_total) {
            (void)snprintf(stages[stage].reason,
                sizeof(stages[stage].reason),
                "Durable progress incomplete: %llu/%llu",
                (unsigned long long)tasks[index].durable_completed,
                (unsigned long long)tasks[index].durable_total);
        } else if (tasks[index].state == TASK_COMPLETED
            && !tasks[index].durable_progress_known) {
            copy_text(stages[stage].reason, sizeof(stages[stage].reason),
                "Lifecycle complete; scientific progress indeterminate");
        } else {
            copy_text(stages[stage].reason, sizeof(stages[stage].reason),
                tasks[index].message[0] ? tasks[index].message
                    : lardon3d_task_state_name(tasks[index].state));
        }
    }

    /* READY is only a workflow hint after an observed successful predecessor;
     * it never claims that scientific prerequisites or an absent Task exist. */
    for (size_t index = 0; index + 1 < LARDON3D_TUI_STAGE_DENSE; ++index) {
        if (stages[index].state == LARDON3D_TUI_STAGE_COMPLETE
            && !observed[index + 1]) {
            stages[index + 1].state = LARDON3D_TUI_STAGE_READY;
            copy_text(stages[index + 1].reason,
                sizeof(stages[index + 1].reason),
                "Previous observed stage completed; eligibility not inferred");
        }
    }
}

static unsigned int
percentage(uint64_t completed, uint64_t total)
{
    if (total == 0) {
        return 0;
    }
    if (completed >= total) {
        return 100;
    }
    uint64_t quotient = total / 100U;
    uint64_t remainder = total % 100U;
    for (unsigned int percent = 99; percent > 0; --percent) {
        uint64_t threshold = quotient * percent;
        uint64_t residual_product = remainder * percent;
        threshold += residual_product / 100U;
        if (residual_product % 100U != 0) {
            ++threshold;
        }
        if (completed >= threshold) {
            return percent;
        }
    }
    return 0;
}

static void
progress_tracker_reset(
    Lardon3DTuiProgressTracker *tracker,
    const Lardon3DTuiProgressSample *sample,
    uint64_t completed
)
{
    *tracker = (Lardon3DTuiProgressTracker) {
        .task_id = sample->task_id,
        .prefix_completed = completed,
        .previous_completed = completed,
        .previous_sample_ns = sample->monotonic_ns,
        .last_positive_ns = sample->monotonic_ns,
        .previous_state = sample->task_state,
        .initialized = true,
        .using_durable_counts = sample->durable_counts_known,
    };
}

bool
lardon3d_tui_progress_update(
    Lardon3DTuiProgressTracker *tracker,
    const Lardon3DTuiProgressSample *sample,
    Lardon3DTuiProgressView *view
)
{
    if (view) {
        *view = (Lardon3DTuiProgressView) {0};
    }
    if (!tracker || !sample || !view || sample->task_id == 0
        || sample->progress_percent > 100
        || (sample->durable_counts_known
            && (sample->durable_total == 0
                || sample->durable_completed > sample->durable_total))) {
        return false;
    }

    bool measurement_known = sample->durable_counts_known
        || !sample->typed_task;
    uint64_t completed = sample->durable_counts_known
        ? sample->durable_completed
        : sample->progress_percent;
    uint64_t total = sample->durable_counts_known
        ? sample->durable_total
        : UINT64_C(100);
    if (!measurement_known) {
        *tracker = (Lardon3DTuiProgressTracker) {0};
        *view = (Lardon3DTuiProgressView) {
            .present = true,
            .task_id = sample->task_id,
            .eta_state = LARDON3D_TUI_ETA_INDETERMINATE,
        };
        return true;
    }
    if (!tracker->initialized || tracker->task_id != sample->task_id
        || tracker->using_durable_counts != sample->durable_counts_known) {
        progress_tracker_reset(tracker, sample, completed);
    } else if (sample->task_state == TASK_RUNNING
        && tracker->previous_state != TASK_RUNNING) {
        /* Admission/resume starts a fresh rate interval. Pending or paused
         * wall time is not processing throughput and must not inflate ETA. */
        tracker->previous_completed = completed;
        tracker->previous_sample_ns = sample->monotonic_ns;
        tracker->last_positive_ns = sample->monotonic_ns;
        if (completed > tracker->prefix_completed) {
            tracker->prefix_completed = completed;
        }
        tracker->ewma_units_per_second = 0.0;
        tracker->positive_sample_count = 0;
    } else if (completed < tracker->previous_completed
        || sample->monotonic_ns < tracker->previous_sample_ns) {
        /* A restored Task may expose an earlier durable prefix. Treat that as
         * a new baseline; negative work must never create a rate or huge ETA. */
        progress_tracker_reset(tracker, sample, completed);
    } else if (completed > tracker->previous_completed
        && sample->monotonic_ns > tracker->previous_sample_ns) {
        uint64_t delta = completed - tracker->previous_completed;
        uint64_t elapsed_ns = sample->monotonic_ns
            - tracker->previous_sample_ns;
        double rate = (double)delta * 1000000000.0 / (double)elapsed_ns;
        if (isfinite(rate) && rate > 0.0) {
            tracker->ewma_units_per_second =
                tracker->positive_sample_count == 0
                ? rate
                : tracker->ewma_units_per_second * 0.65 + rate * 0.35;
            if (tracker->positive_sample_count < UINT_MAX) {
                ++tracker->positive_sample_count;
            }
            tracker->last_positive_ns = sample->monotonic_ns;
        }
    }
    tracker->previous_completed = completed;
    tracker->previous_sample_ns = sample->monotonic_ns;
    tracker->previous_state = sample->task_state;

    *view = (Lardon3DTuiProgressView) {
        .present = true,
        .task_id = sample->task_id,
        .durable_counts_known = sample->durable_counts_known,
        .runtime_percentage = !sample->durable_counts_known,
        .completed = sample->durable_counts_known ? completed : 0,
        .total = sample->durable_counts_known ? total : 0,
        .percentage_known = true,
        .percentage = sample->durable_counts_known
            ? percentage(completed, total)
            : sample->progress_percent,
        .throughput_known = tracker->positive_sample_count >= 2,
        .units_per_second = tracker->ewma_units_per_second,
        .eta_state = LARDON3D_TUI_ETA_CALCULATING,
        .resumed_prefix_excluded = tracker->prefix_completed > 0,
    };

    if (sample->task_state == TASK_COMPLETED
        && sample->durable_counts_known && completed < total) {
        view->integrity_error = true;
        view->eta_state = LARDON3D_TUI_ETA_INDETERMINATE;
        return true;
    }
    if ((sample->durable_counts_known && completed == total)
        || (!sample->typed_task && sample->task_state == TASK_COMPLETED)) {
        view->percentage = 100;
        view->eta_state = LARDON3D_TUI_ETA_COMPLETE;
        view->eta_seconds = 0;
        return true;
    }
    if (sample->pressure_limited || sample->task_state == TASK_PAUSED) {
        view->eta_state = LARDON3D_TUI_ETA_THROTTLED;
        return true;
    }
    if (sample->task_state != TASK_RUNNING) {
        view->eta_state = LARDON3D_TUI_ETA_INDETERMINATE;
        return true;
    }
    if (sample->monotonic_ns >= tracker->last_positive_ns
        && sample->monotonic_ns - tracker->last_positive_ns
            >= UINT64_C(5000000000)) {
        view->eta_state = LARDON3D_TUI_ETA_STALLED;
        return true;
    }
    if (tracker->positive_sample_count < 2
        || !(tracker->ewma_units_per_second > 0.0)) {
        return true;
    }

    uint64_t remaining = total - completed;
    long double seconds = (long double)remaining
        / (long double)tracker->ewma_units_per_second;
    if (!isfinite((double)seconds) || seconds > (long double)UINT64_MAX) {
        view->eta_state = LARDON3D_TUI_ETA_INDETERMINATE;
        return true;
    }
    uint64_t rounded = (uint64_t)seconds;
    if ((long double)rounded < seconds) {
        ++rounded;
    }
    view->eta_state = LARDON3D_TUI_ETA_KNOWN;
    view->eta_seconds = rounded;
    return true;
}

Lardon3DTuiKeyContract
lardon3d_tui_key_contract(Lardon3DTuiInteractionMode mode)
{
    switch (mode) {
    case LARDON3D_TUI_INTERACTION_TEXT_INPUT:
        return (Lardon3DTuiKeyContract) {
            .enter = true,
            .escape = true,
            .f10 = true,
        };
    case LARDON3D_TUI_INTERACTION_IMPORT_RUNNING:
        return (Lardon3DTuiKeyContract) {
            .f10 = true,
            .cancel_import = true,
        };
    case LARDON3D_TUI_INTERACTION_IDLE:
        return (Lardon3DTuiKeyContract) {
            .escape = true,
            .f10 = true,
            .quit = true,
            .navigate = true,
        };
    }
    return (Lardon3DTuiKeyContract) {0};
}

const char *
lardon3d_tui_eta_state_name(Lardon3DTuiEtaState state)
{
    switch (state) {
    case LARDON3D_TUI_ETA_INDETERMINATE:
        return "indeterminate";
    case LARDON3D_TUI_ETA_CALCULATING:
        return "calculating";
    case LARDON3D_TUI_ETA_KNOWN:
        return "known";
    case LARDON3D_TUI_ETA_STALLED:
        return "stalled";
    case LARDON3D_TUI_ETA_THROTTLED:
        return "throttled";
    case LARDON3D_TUI_ETA_COMPLETE:
        return "complete";
    }
    return "unknown";
}

const char *
lardon3d_tui_gpu_backend_name(Lardon3DTuiGpuBackendStatus status)
{
    switch (status) {
    case LARDON3D_TUI_GPU_BACKEND_UNKNOWN:
        return "UNKNOWN";
    case LARDON3D_TUI_GPU_BACKEND_UNINITIALIZED:
        return "UNINITIALIZED";
    case LARDON3D_TUI_GPU_BACKEND_AVAILABLE:
        return "AVAILABLE";
    case LARDON3D_TUI_GPU_BACKEND_UNAVAILABLE:
        return "UNAVAILABLE";
    case LARDON3D_TUI_GPU_BACKEND_CPU:
        return "CPU";
    case LARDON3D_TUI_GPU_BACKEND_ORB_VULKAN:
        return "ORB_VULKAN";
    case LARDON3D_TUI_GPU_BACKEND_MIXED:
        return "MIXED";
    }
    return "UNKNOWN";
}

const char *
lardon3d_tui_pressure_name(Lardon3DResourcePressure pressure)
{
    switch (pressure) {
    case LARDON3D_RESOURCE_PRESSURE_GREEN:
        return "GREEN";
    case LARDON3D_RESOURCE_PRESSURE_YELLOW:
        return "YELLOW";
    case LARDON3D_RESOURCE_PRESSURE_RED:
        return "RED";
    }
    return "UNKNOWN";
}

Lardon3DTuiOpticsStatus
lardon3d_tui_optics_classify(
    bool project_loaded,
    bool assignment_found,
    bool assignment_valid,
    bool manual_lens,
    size_t compatible_calibration_count,
    bool selection_found,
    bool selection_compatible
)
{
    (void)manual_lens;
    if (!project_loaded) {
        return LARDON3D_TUI_OPTICS_NO_PROJECT;
    }
    if (!assignment_found) {
        return LARDON3D_TUI_OPTICS_UNRESOLVED;
    }
    if (!assignment_valid) {
        return LARDON3D_TUI_OPTICS_CORRUPT;
    }
    if (selection_found && !selection_compatible) {
        return LARDON3D_TUI_OPTICS_INCOMPATIBLE;
    }
    if (selection_found) {
        return LARDON3D_TUI_OPTICS_SELECTED;
    }
    if (compatible_calibration_count == 0) {
        return LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY;
    }
    return LARDON3D_TUI_OPTICS_SELECTION_REQUIRED;
}

const char *
lardon3d_tui_optics_status_name(Lardon3DTuiOpticsStatus status)
{
    switch (status) {
    case LARDON3D_TUI_OPTICS_NO_PROJECT:
        return "NO_PROJECT";
    case LARDON3D_TUI_OPTICS_UNRESOLVED:
        return "UNRESOLVED";
    case LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY:
        return "MISSING_CALIBRATION";
    case LARDON3D_TUI_OPTICS_SELECTION_REQUIRED:
        return "SELECTION_REQUIRED";
    case LARDON3D_TUI_OPTICS_SELECTED:
        return "SELECTED";
    case LARDON3D_TUI_OPTICS_INCOMPATIBLE:
        return "INCOMPATIBLE";
    case LARDON3D_TUI_OPTICS_CORRUPT:
        return "CORRUPT";
    }
    return "UNKNOWN";
}

const char *
lardon3d_tui_optics_status_explanation(
    Lardon3DTuiOpticsStatus status,
    bool manual_lens
)
{
    switch (status) {
    case LARDON3D_TUI_OPTICS_NO_PROJECT:
        return "Load a project to inspect optical assignments.";
    case LARDON3D_TUI_OPTICS_UNRESOLVED:
        return "No explicit optical configuration is assigned; nothing is guessed.";
    case LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY:
        return manual_lens
            ? "Manual/no-EXIF lens is valid; no compatible calibration exists."
            : "Optical configuration is explicit, but no compatible calibration exists.";
    case LARDON3D_TUI_OPTICS_SELECTION_REQUIRED:
        return "Compatible calibration exists; select one explicitly.";
    case LARDON3D_TUI_OPTICS_SELECTED:
        return manual_lens
            ? "Manual/no-EXIF configuration and exact calibration are selected."
            : "Exact optical configuration and calibration are selected.";
    case LARDON3D_TUI_OPTICS_INCOMPATIBLE:
        return "Stored selection is incompatible with the Capture configuration.";
    case LARDON3D_TUI_OPTICS_CORRUPT:
        return "Malformed durable optical state; no mutation was attempted.";
    }
    return "Unknown optical status.";
}
