#ifndef LARDON3D_LAYOUT_H
#define LARDON3D_LAYOUT_H

#include <lardon3d/app_state.h>

void lardon3d_layout_draw(
    const Lardon3DAppState *state,
    const char *project_name_input,
    const char *project_input_label,
    int rows,
    int cols
);

#endif
