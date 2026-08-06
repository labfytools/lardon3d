#include <locale.h>
#include <stdlib.h>

#include <lardon3d/app.h>
#include <lardon3d/tui.h>

int
lardon3d_app_run(void)
{
    if (!setlocale(LC_ALL, "")) {
        return EXIT_FAILURE;
    }

    if (!lardon3d_tui_init()) {
        return EXIT_FAILURE;
    }

    bool success = lardon3d_tui_run();
    lardon3d_tui_shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
