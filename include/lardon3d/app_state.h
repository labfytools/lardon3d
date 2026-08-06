#ifndef LARDON3D_APP_STATE_H
#define LARDON3D_APP_STATE_H

#include <stdbool.h>
#include <limits.h>

typedef enum {
    LARDON3D_SCREEN_HOME = 0,
    LARDON3D_SCREEN_PROJECTS,
    LARDON3D_SCREEN_IMPORT,
    LARDON3D_SCREEN_VIEWER,
    LARDON3D_SCREEN_HELP
} Lardon3DScreen;

typedef struct {
    Lardon3DScreen screen;
    bool running;
    bool project_loaded;
    char project_name[128];
    char project_path[PATH_MAX];
    char status_message[256];
} Lardon3DAppState;

void lardon3d_app_state_init(Lardon3DAppState *state);

#endif
