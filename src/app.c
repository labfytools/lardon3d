#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include <lardon3d/app.h>
#include <lardon3d/app_state.h>
#include <lardon3d/feature_extractor.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/orb_vulkan_backend.h>
#include <lardon3d/project.h>
#include <lardon3d/resource_governor.h>
#include <lardon3d/ssd_controller.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/tui.h>
#include <lardon3d/tui_ssd_async.h>

#include "resource_governor_internal.h"

int
lardon3d_app_run(void)
{
    /* CONTRACT: establish the process-wide driver policy before Queue, OpenCV,
     * ncurses, or any other application pthread can exist. An explicit unsafe
     * Mesa cache value is rejected instead of silently overwritten. */
    Lardon3DResourceDriverPolicyResult driver_policy =
        lardon3d_resource_governor_internal_configure_driver_policy();
    if (driver_policy == LARDON3D_RESOURCE_DRIVER_POLICY_FAILED
        || driver_policy == LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE) {
        (void)fprintf(stderr,
            "MESA_SHADER_CACHE_DISABLE must be true for safe CPU affinity\n");
        return EXIT_FAILURE;
    }
    Lardon3DAppState state;
    lardon3d_app_state_init(&state);

    if (!setlocale(LC_ALL, "")) {
        return EXIT_FAILURE;
    }
    char error[256];
    Lardon3DResourcePolicy resource_policy;
    if (!lardon3d_hardware_profile_detect(
            &state.hardware_profile,
            error,
            sizeof(error)
        )
        || !lardon3d_resource_policy_default(
            &state.hardware_profile,
            &resource_policy
        )) {
        return EXIT_FAILURE;
    }
    state.resource_governor = lardon3d_resource_governor_create(
        &state.hardware_profile,
        &resource_policy
    );
    Lardon3DResourceCpuPolicyDiagnostic cpu_policy;
    if (!state.resource_governor
        || !lardon3d_resource_governor_internal_cpu_policy(
            state.resource_governor, &cpu_policy)
        || cpu_policy.compute_cpu_count == 0) {
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }
    /* OpenCV owns one process-wide CPU setting. Startup establishes the safe
     * derived compute-pool baseline before Queue exists; the sole Queue callback
     * may temporarily apply its immutable admitted count and must restore this
     * baseline on every path. Topology/core-group overshoot and external affinity
     * constraints are therefore honored once, rather than independently capped.
     * This operational count is never FeatureSet identity or fingerprint. */
    if (!lardon3d_feature_opencv_configure_threads(
            cpu_policy.compute_cpu_count)) {
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }
    state.orb_vulkan_backend = lardon3d_orb_vulkan_backend_create();
    if (!state.orb_vulkan_backend) {
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }
    state.task_queue = lardon3d_task_queue_create(
        state.resource_governor, LARDON3D_TASK_QUEUE_PRODUCTION_CAPACITY);
    if (!state.task_queue) {
        lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }

    /* SSD support is optional physical-resource control. Failure to connect to
     * UDisks leaves the TUI truthful but does not make scientific execution or
     * Project DB availability depend on removable hardware. */
    Lardon3DSsdController *ssd_controller =
        lardon3d_ssd_controller_create();
    Lardon3DTuiSsdAsync *ssd_operation = ssd_controller
        ? lardon3d_tui_ssd_async_create_with_governor(
            ssd_controller, state.resource_governor)
        : NULL;

    if (ssd_controller && !ssd_operation) {
        lardon3d_task_queue_destroy(state.task_queue);
        state.task_queue = NULL;
        (void)lardon3d_ssd_controller_destroy(ssd_controller);
        lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }

    if (!lardon3d_tui_init()) {
        lardon3d_task_queue_destroy(state.task_queue);
        state.task_queue = NULL;
        bool storage_released = !ssd_operation
            || lardon3d_tui_ssd_async_destroy_checked(&ssd_operation);
        if (storage_released) {
            (void)lardon3d_ssd_controller_destroy(ssd_controller);
        }
        lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
        if (storage_released) {
            lardon3d_resource_governor_destroy(state.resource_governor);
        }
        return EXIT_FAILURE;
    }

    bool success = lardon3d_tui_run_with_ssd_operation(
        &state, ssd_operation);
    lardon3d_tui_shutdown();
    /* INVARIANT: Queue destruction cancels and joins the sole Task worker, so
     * every production Task-owned scratch lease is released before the
     * Governor registration loses its exact physical-controller identity. */
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    if (state.project_loaded) {
        lardon3d_project_close(&state);
    }
    bool storage_released = !ssd_operation
        || lardon3d_tui_ssd_async_destroy_checked(&ssd_operation);
    if (!storage_released) {
        success = false;
    } else if (!lardon3d_ssd_controller_destroy(ssd_controller)) {
        success = false;
    }
    lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
    if (storage_released) {
        lardon3d_resource_governor_destroy(state.resource_governor);
    }

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
