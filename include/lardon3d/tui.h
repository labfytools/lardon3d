#ifndef LARDON3D_TUI_H
#define LARDON3D_TUI_H

#include <stdbool.h>

#include <lardon3d/app_state.h>

bool lardon3d_tui_init(void);
bool lardon3d_tui_run(Lardon3DAppState *state);
void lardon3d_tui_shutdown(void);

#endif
