#include <stdlib.h>

#include <scan3d/app.h>
#include <scan3d/tui.h>

int
scan3d_app_run(void)
{
    if (!scan3d_tui_init()) {
        return EXIT_FAILURE;
    }

    bool success = scan3d_tui_run();
    scan3d_tui_shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
