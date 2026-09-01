#ifndef LARDON3D_TUI_MODEL_H
#define LARDON3D_TUI_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/resource_governor.h>
#include <lardon3d/ssd_controller.h>
#include <lardon3d/task.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LARDON3D_TUI_STAGE_COUNT = 11,
    LARDON3D_TUI_TEXT_CAPACITY = 256,
    /* Production Queue capacity is 64 pending. This bound observes every
     * possible live Task plus the complete bounded recent history. */
    LARDON3D_TUI_TASK_CAPACITY = 129,
};

/* The viewport decision is pure and terminal-independent so resize behavior is
 * testable without entering ncurses. Compact is a supported layout, not an
 * error path; only dimensions below 60x15 display the bounded fallback text. */
typedef enum {
    LARDON3D_TUI_VIEWPORT_TOO_SMALL = 0,
    LARDON3D_TUI_VIEWPORT_COMPACT,
    LARDON3D_TUI_VIEWPORT_FULL,
} Lardon3DTuiViewport;

typedef enum {
    LARDON3D_TUI_SEMANTIC_NORMAL = 0,
    LARDON3D_TUI_SEMANTIC_HEALTHY,
    LARDON3D_TUI_SEMANTIC_WARNING,
    LARDON3D_TUI_SEMANTIC_ERROR,
    LARDON3D_TUI_SEMANTIC_GPU,
    LARDON3D_TUI_SEMANTIC_CPU,
    LARDON3D_TUI_SEMANTIC_SSD,
    LARDON3D_TUI_SEMANTIC_DIM,
    LARDON3D_TUI_SEMANTIC_COUNT,
} Lardon3DTuiSemantic;

enum {
    LARDON3D_TUI_STYLE_BOLD = 1U << 0,
    LARDON3D_TUI_STYLE_DIM = 1U << 1,
};

typedef struct {
    bool color_enabled;
    short color_pair[LARDON3D_TUI_SEMANTIC_COUNT];
    unsigned int attributes[LARDON3D_TUI_SEMANTIC_COUNT];
} Lardon3DTuiPalette;

Lardon3DTuiViewport lardon3d_tui_viewport_classify(int rows, int columns);

/* `color_pairs` is the terminal's total pair capacity, including pair zero.
 * Missing/limited color never removes semantic text labels: styles fall back
 * deterministically to bold/dim attributes and pair zero. */
void lardon3d_tui_palette_plan(
    bool colors_supported,
    int color_pairs,
    Lardon3DTuiPalette *palette
);

typedef enum {
    LARDON3D_TUI_STAGE_ACQUISITION = 0,
    LARDON3D_TUI_STAGE_RAW,
    LARDON3D_TUI_STAGE_QUALITY,
    LARDON3D_TUI_STAGE_FEATURES,
    LARDON3D_TUI_STAGE_VISUAL_INDEX,
    LARDON3D_TUI_STAGE_CANDIDATE,
    LARDON3D_TUI_STAGE_MATCHER,
    LARDON3D_TUI_STAGE_GV,
    LARDON3D_TUI_STAGE_TRACKS,
    LARDON3D_TUI_STAGE_SPARSE_SFM,
    LARDON3D_TUI_STAGE_DENSE,
} Lardon3DTuiStage;

typedef enum {
    LARDON3D_TUI_STAGE_NOT_READY = 0,
    LARDON3D_TUI_STAGE_READY,
    LARDON3D_TUI_STAGE_QUEUED,
    LARDON3D_TUI_STAGE_RUNNING,
    LARDON3D_TUI_STAGE_THROTTLED,
    LARDON3D_TUI_STAGE_BLOCKED,
    LARDON3D_TUI_STAGE_COMPLETE,
    LARDON3D_TUI_STAGE_FAILED,
    LARDON3D_TUI_STAGE_NOT_APPLICABLE,
} Lardon3DTuiStageState;

typedef struct {
    Lardon3DTuiStage stage;
    Lardon3DTuiStageState state;
    char reason[LARDON3D_TUI_TEXT_CAPACITY];
} Lardon3DTuiStageView;

const char *lardon3d_tui_stage_name(Lardon3DTuiStage stage);
const char *lardon3d_tui_stage_state_name(Lardon3DTuiStageState state);
Lardon3DTuiSemantic lardon3d_tui_stage_semantic(
    Lardon3DTuiStageState state
);

/* Builds a runtime observation only from typed Task snapshots. Absence of a
 * Task is never interpreted as scientific completion. Dense is deliberately
 * NOT_APPLICABLE until a later production Task kind exists, so future science
 * can never be rendered as active by a name/message coincidence. */
void lardon3d_tui_stage_views_build(
    bool project_loaded,
    const Lardon3DTaskObservation *tasks,
    size_t task_count,
    Lardon3DResourcePressure pressure,
    Lardon3DTuiStageView stages[LARDON3D_TUI_STAGE_COUNT]
);

typedef enum {
    LARDON3D_TUI_ETA_INDETERMINATE = 0,
    LARDON3D_TUI_ETA_CALCULATING,
    LARDON3D_TUI_ETA_KNOWN,
    LARDON3D_TUI_ETA_STALLED,
    LARDON3D_TUI_ETA_THROTTLED,
    LARDON3D_TUI_ETA_COMPLETE,
} Lardon3DTuiEtaState;

typedef struct {
    uint64_t task_id;
    uint64_t monotonic_ns;
    Lardon3DTaskState task_state;
    bool typed_task;
    unsigned int progress_percent;
    bool durable_counts_known;
    uint64_t durable_completed;
    uint64_t durable_total;
    bool pressure_limited;
} Lardon3DTuiProgressSample;

typedef struct {
    uint64_t task_id;
    uint64_t prefix_completed;
    uint64_t previous_completed;
    uint64_t previous_sample_ns;
    uint64_t last_positive_ns;
    double ewma_units_per_second;
    unsigned int positive_sample_count;
    Lardon3DTaskState previous_state;
    bool initialized;
    bool using_durable_counts;
} Lardon3DTuiProgressTracker;

typedef struct {
    bool present;
    uint64_t task_id;
    bool durable_counts_known;
    bool runtime_percentage;
    bool integrity_error;
    uint64_t completed;
    uint64_t total;
    bool percentage_known;
    unsigned int percentage;
    bool elapsed_known;
    uint64_t elapsed_seconds;
    bool throughput_known;
    double units_per_second;
    Lardon3DTuiEtaState eta_state;
    uint64_t eta_seconds;
    bool resumed_prefix_excluded;
} Lardon3DTuiProgressView;

typedef enum {
    LARDON3D_TUI_INTERACTION_IDLE = 0,
    LARDON3D_TUI_INTERACTION_TEXT_INPUT,
    LARDON3D_TUI_INTERACTION_IMPORT_RUNNING,
} Lardon3DTuiInteractionMode;

typedef struct {
    bool enter;
    bool escape;
    bool f10;
    bool cancel_import;
    bool quit;
    bool navigate;
} Lardon3DTuiKeyContract;

/* Pure key/footer contract shared by the actual handler mode and renderer.
 * A displayed action is therefore never broader than the active input owner. */
Lardon3DTuiKeyContract lardon3d_tui_key_contract(
    Lardon3DTuiInteractionMode mode
);

/* Updates one fixed-size tracker. The first observation of a Task is a resume
 * baseline and contributes no throughput. A transition into RUNNING also
 * resets rate timing, excluding pending/pause time. ETA becomes known only
 * after two positive intervals, and rate remains UNKNOWN until that same
 * evidence exists. Five seconds without progress is a stall, regressions reset
 * the baseline, and pressure is explicit. Only coherent durable completion or
 * an untyped runtime completion is shown as exactly 100% with ETA zero; a typed
 * terminal lifecycle with incomplete/unknown durable science is never filled
 * in. */
bool lardon3d_tui_progress_update(
    Lardon3DTuiProgressTracker *tracker,
    const Lardon3DTuiProgressSample *sample,
    Lardon3DTuiProgressView *view
);

const char *lardon3d_tui_eta_state_name(Lardon3DTuiEtaState state);

typedef enum {
    LARDON3D_TUI_GPU_BACKEND_UNKNOWN = 0,
    LARDON3D_TUI_GPU_BACKEND_UNINITIALIZED,
    LARDON3D_TUI_GPU_BACKEND_AVAILABLE,
    LARDON3D_TUI_GPU_BACKEND_UNAVAILABLE,
    LARDON3D_TUI_GPU_BACKEND_CPU,
    LARDON3D_TUI_GPU_BACKEND_ORB_VULKAN,
    LARDON3D_TUI_GPU_BACKEND_MIXED,
} Lardon3DTuiGpuBackendStatus;

typedef struct {
    bool valid;
    uint64_t captured_monotonic_ns;
    Lardon3DResourcePressure governor_pressure;
    char governor_reason[LARDON3D_TUI_TEXT_CAPACITY];

    unsigned int cpu_logical_total;
    bool cpu_admitted_known;
    unsigned int cpu_admitted;
    unsigned int cpu_available;
    unsigned int cpu_active;
    bool cpu_utilization_known;
    uint32_t cpu_utilization_basis_points;
    char cpu_reason[LARDON3D_TUI_TEXT_CAPACITY];

    bool gpu_present;
    bool gpu_uses_shared_memory;
    bool gpu_memory_known;
    uint64_t gpu_memory_reserved_bytes;
    uint64_t gpu_memory_available_bytes;
    bool gpu_busy_known;
    uint32_t gpu_busy_basis_points;
    unsigned int gpu_slots_active;
    unsigned int gpu_slots_available;
    Lardon3DTuiGpuBackendStatus gpu_backend;
    char gpu_backend_reason[LARDON3D_TUI_TEXT_CAPACITY];

    uint64_t ram_total_bytes;
    uint64_t ram_available_bytes;
    uint64_t ram_reserve_bytes;
    uint64_t ram_reserved_bytes;
    bool swap_total_known;
    uint64_t swap_total_bytes;
    bool swap_used_known;
    uint64_t swap_used_bytes;
    bool swap_delta_known;
    uint64_t swap_pages_in_delta;
    uint64_t swap_pages_out_delta;

    bool batch_known;
    size_t batch_size;
    bool inflight_known;
    size_t inflight_limit;
    bool helpers_known;
    unsigned int helper_limit;
    unsigned int io_active;
    unsigned int io_available;

    bool scratch_known;
    bool scratch_mounted;
    bool scratch_total_known;
    bool scratch_free_known;
    uint64_t scratch_total_bytes;
    uint64_t scratch_free_bytes;
    size_t scratch_leases;

    /* Governor-owned external usage. Physical device paths and F10 authority
     * remain in the separate controller snapshot; this view cannot grant a
     * lease or infer availability from a mount bit. */
    bool external_storage_registered;
    Lardon3DResourceExternalStorageStatus external_storage_status;
    bool scratch_new_allocations_allowed;
    bool external_swap_total_known;
    bool external_swap_used_known;
    uint64_t external_swap_total_bytes;
    uint64_t external_swap_used_bytes;
    char external_storage_identity[LARDON3D_TUI_TEXT_CAPACITY];
    char external_storage_reason[LARDON3D_TUI_TEXT_CAPACITY];
} Lardon3DTuiResourceView;

const char *lardon3d_tui_gpu_backend_name(
    Lardon3DTuiGpuBackendStatus status
);
const char *lardon3d_tui_pressure_name(Lardon3DResourcePressure pressure);

typedef enum {
    LARDON3D_TUI_OPTICS_NO_PROJECT = 0,
    LARDON3D_TUI_OPTICS_UNRESOLVED,
    LARDON3D_TUI_OPTICS_CONFIGURATION_ONLY,
    LARDON3D_TUI_OPTICS_SELECTION_REQUIRED,
    LARDON3D_TUI_OPTICS_SELECTED,
    LARDON3D_TUI_OPTICS_INCOMPATIBLE,
    LARDON3D_TUI_OPTICS_CORRUPT,
} Lardon3DTuiOpticsStatus;

/* This presentation classifier never creates an "unknown" profile or guesses
 * identity. A manual/no-electronics lens is ordinary configuration data and
 * does not change compatibility or selection rules. */
Lardon3DTuiOpticsStatus lardon3d_tui_optics_classify(
    bool project_loaded,
    bool assignment_found,
    bool assignment_valid,
    bool manual_lens,
    size_t compatible_calibration_count,
    bool selection_found,
    bool selection_compatible
);

const char *lardon3d_tui_optics_status_name(Lardon3DTuiOpticsStatus status);
const char *lardon3d_tui_optics_status_explanation(
    Lardon3DTuiOpticsStatus status,
    bool manual_lens
);

#ifdef __cplusplus
}
#endif

#endif
