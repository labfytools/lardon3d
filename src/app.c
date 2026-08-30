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
#include <lardon3d/task_queue.h>
#include <lardon3d/tui.h>

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
    unsigned int feature_threads = state.hardware_profile.logical_cpu_count
        - resource_policy.system_cpu_reserve;
    if (feature_threads > 12) {
        feature_threads = 12;
    }
    /* OpenCV owns one process-wide CPU setting. Startup establishes the safe
     * audited baseline/ceiling before Queue exists; the sole Queue callback may
     * temporarily apply its immutable admitted count and must restore this
     * baseline on every path. Concurrent multi-worker mutation is unsupported.
     * This operational count is never FeatureSet identity or fingerprint. */
    if (!lardon3d_feature_opencv_configure_threads(feature_threads)) {
        return EXIT_FAILURE;
    }
    state.resource_governor = lardon3d_resource_governor_create(
        &state.hardware_profile,
        &resource_policy
    );
    if (!state.resource_governor) {
        return EXIT_FAILURE;
    }
    state.orb_vulkan_backend = lardon3d_orb_vulkan_backend_create();
    if (!state.orb_vulkan_backend) {
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }
    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 64);
    if (!state.task_queue) {
        lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }

    if (!lardon3d_tui_init()) {
        lardon3d_task_queue_destroy(state.task_queue);
        lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }

    bool success = lardon3d_tui_run(&state);
    lardon3d_tui_shutdown();
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    if (state.project_loaded) {
        lardon3d_project_close(&state);
    }
    lardon3d_orb_vulkan_backend_destroy(state.orb_vulkan_backend);
    lardon3d_resource_governor_destroy(state.resource_governor);

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
