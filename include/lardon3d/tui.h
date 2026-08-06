#ifndef LARDON3D_TUI_H
#define LARDON3D_TUI_H

#include <stdbool.h>

typedef enum {
    LARDON3D_SCREEN_HOME = 0,
    LARDON3D_SCREEN_PROJECTS,
    LARDON3D_SCREEN_IMPORT,
    LARDON3D_SCREEN_VIEWER,
    LARDON3D_SCREEN_HELP
} Lardon3DScreen;

bool lardon3d_tui_init(void);
bool lardon3d_tui_run(void);
void lardon3d_tui_shutdown(void);

#endif
