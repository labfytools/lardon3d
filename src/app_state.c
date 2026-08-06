#include <lardon3d/app_state.h>

void
lardon3d_app_state_init(Lardon3DAppState *state)
{
    if (!state) {
        return;
    }

    *state = (Lardon3DAppState) {
        .screen = LARDON3D_SCREEN_HOME,
        .running = true,
        .project_loaded = false,
        .project_name = "",
        .project_path = "",
        .status_message = "Bienvenue dans Lardon3D",
    };
}
