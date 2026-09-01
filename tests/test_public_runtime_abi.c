#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/layout.h>
#include <lardon3d/resource_snapshot.h>
#include <lardon3d/task.h>
#include <lardon3d/tui.h>

/* ABI fixture captured from baseline 96d0791 on the supported x86_64 target.
 * CONTRACT: additive observability uses separately named objects/functions;
 * old callers must never receive a larger write or a changed symbol type. */
_Static_assert(sizeof(Lardon3DTaskSnapshot) == 432,
    "baseline TaskSnapshot size changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, id) == 0,
    "baseline TaskSnapshot.id offset changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, name) == 8,
    "baseline TaskSnapshot.name offset changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, progress) == 136,
    "baseline TaskSnapshot.progress offset changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, state) == 140,
    "baseline TaskSnapshot.state offset changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, message) == 144,
    "baseline TaskSnapshot.message offset changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, started_at) == 400,
    "baseline TaskSnapshot.started_at offset changed");
_Static_assert(offsetof(Lardon3DTaskSnapshot, finished_at) == 416,
    "baseline TaskSnapshot.finished_at offset changed");

_Static_assert(sizeof(Lardon3DResourceSnapshot) == 152,
    "baseline ResourceSnapshot size changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, captured_at) == 0,
    "baseline ResourceSnapshot.captured_at offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot,
    memory_available_bytes) == 16,
    "baseline ResourceSnapshot.memory_available_bytes offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, memory_free_bytes) == 24,
    "baseline ResourceSnapshot.memory_free_bytes offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, swap_available_bytes) == 32,
    "baseline ResourceSnapshot.swap_available_bytes offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot,
    gpu_memory_available_known) == 40,
    "baseline ResourceSnapshot GPU-known offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot,
    gpu_memory_available_bytes) == 48,
    "baseline ResourceSnapshot GPU-bytes offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, cpu_load_1m) == 56,
    "baseline ResourceSnapshot cpu_load_1m offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, cpu_load_5m) == 64,
    "baseline ResourceSnapshot cpu_load_5m offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, cpu_load_15m) == 72,
    "baseline ResourceSnapshot cpu_load_15m offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, cpu_pressure_known) == 80,
    "baseline ResourceSnapshot CPU PSI-known offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, cpu_pressure_avg10) == 88,
    "baseline ResourceSnapshot CPU PSI offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot,
    memory_pressure_known) == 96,
    "baseline ResourceSnapshot memory PSI-known offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot,
    memory_pressure_avg10) == 104,
    "baseline ResourceSnapshot memory PSI offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, io_pressure_known) == 112,
    "baseline ResourceSnapshot I/O PSI-known offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, io_pressure_avg10) == 120,
    "baseline ResourceSnapshot I/O PSI offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, swap_activity_known) == 128,
    "baseline ResourceSnapshot swap-known offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, swap_pages_in) == 136,
    "baseline ResourceSnapshot swap-in offset changed");
_Static_assert(offsetof(Lardon3DResourceSnapshot, swap_pages_out) == 144,
    "baseline ResourceSnapshot swap-out offset changed");

_Static_assert(sizeof(Lardon3DAppState) == 4952,
    "baseline AppState size changed");
_Static_assert(offsetof(Lardon3DAppState, screen) == 0,
    "baseline AppState.screen offset changed");
_Static_assert(offsetof(Lardon3DAppState, running) == 4,
    "baseline AppState.running offset changed");
_Static_assert(offsetof(Lardon3DAppState, project_loaded) == 5,
    "baseline AppState.project_loaded offset changed");
_Static_assert(offsetof(Lardon3DAppState, project_name) == 6,
    "baseline AppState.project_name offset changed");
_Static_assert(offsetof(Lardon3DAppState, project_path) == 134,
    "baseline AppState.project_path offset changed");
_Static_assert(offsetof(Lardon3DAppState, project_stable_id) == 4230,
    "baseline AppState.project_stable_id offset changed");
_Static_assert(offsetof(Lardon3DAppState, status_message) == 4295,
    "baseline AppState.status_message offset changed");
_Static_assert(offsetof(Lardon3DAppState, image_catalog) == 4552,
    "baseline AppState.image_catalog offset changed");
_Static_assert(offsetof(Lardon3DAppState, image_view) == 4560,
    "baseline AppState.image_view offset changed");
_Static_assert(offsetof(Lardon3DAppState, task_queue) == 4568,
    "baseline AppState.task_queue offset changed");
_Static_assert(offsetof(Lardon3DAppState, hardware_profile) == 4576,
    "baseline AppState.hardware_profile offset changed");
_Static_assert(offsetof(Lardon3DAppState, resource_governor) == 4880,
    "baseline AppState.resource_governor offset changed");
_Static_assert(offsetof(Lardon3DAppState, project_db) == 4888,
    "baseline AppState.project_db offset changed");
_Static_assert(offsetof(Lardon3DAppState, orb_vulkan_backend) == 4896,
    "baseline AppState.orb_vulkan_backend offset changed");
_Static_assert(offsetof(Lardon3DAppState, recovery_inspected) == 4904,
    "baseline AppState recovery offset changed");
_Static_assert(offsetof(Lardon3DAppState,
    recovery_published_not_durable) == 4936,
    "baseline AppState final count offset changed");
_Static_assert(offsetof(Lardon3DAppState, recovery_queue_full) == 4944,
    "baseline AppState final flag offset changed");
_Static_assert(LARDON3D_SCREEN_RESOURCES == 6,
    "baseline screen enum values changed");

typedef void (*LegacyLayoutFunction)(
    const Lardon3DAppState *,
    const char *,
    const char *,
    const Lardon3DImportTaskSnapshot *,
    const Lardon3DTaskSnapshot *,
    size_t,
    const Lardon3DTaskQueueSummary *,
    const Lardon3DResourceAvailability *,
    int,
    int
);
typedef bool (*LegacyTuiRunFunction)(Lardon3DAppState *);

_Static_assert(_Generic(&lardon3d_layout_draw,
        LegacyLayoutFunction: 1, default: 0),
    "legacy layout symbol type changed");
_Static_assert(_Generic(&lardon3d_tui_run,
        LegacyTuiRunFunction: 1, default: 0),
    "legacy TUI run symbol type changed");

int
main(void)
{
    return 0;
}
