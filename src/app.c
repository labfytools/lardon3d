#include <locale.h>
#include <stdlib.h>

#include <lardon3d/app.h>
#include <lardon3d/app_state.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/resource_governor.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/tui.h>

int
lardon3d_app_run(void)
{
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
    if (!state.resource_governor) {
        return EXIT_FAILURE;
    }
    state.task_queue = lardon3d_task_queue_create();
    if (!state.task_queue) {
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }

    if (!lardon3d_tui_init()) {
        lardon3d_task_queue_destroy(state.task_queue);
        lardon3d_resource_governor_destroy(state.resource_governor);
        return EXIT_FAILURE;
    }

    bool success = lardon3d_tui_run(&state);
    lardon3d_image_view_destroy(state.image_view);
    lardon3d_image_catalog_destroy(state.image_catalog);
    lardon3d_tui_shutdown();
    lardon3d_task_queue_destroy(state.task_queue);
    lardon3d_resource_governor_destroy(state.resource_governor);

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
