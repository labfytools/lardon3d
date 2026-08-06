#include <locale.h>
#include <stdlib.h>

#include <lardon3d/app.h>
#include <lardon3d/app_state.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/tui.h>

int
lardon3d_app_run(void)
{
    Lardon3DAppState state;
    lardon3d_app_state_init(&state);

    if (!setlocale(LC_ALL, "")) {
        return EXIT_FAILURE;
    }

    if (!lardon3d_tui_init()) {
        return EXIT_FAILURE;
    }

    bool success = lardon3d_tui_run(&state);
    lardon3d_image_view_destroy(state.image_view);
    lardon3d_image_catalog_destroy(state.image_catalog);
    lardon3d_tui_shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
