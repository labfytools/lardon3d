#ifndef LARDON3D_PROJECT_H
#define LARDON3D_PROJECT_H

#include <stdbool.h>

#include <lardon3d/app_state.h>

bool lardon3d_project_set_name(
    Lardon3DAppState *state,
    const char *name
);

void lardon3d_project_close(Lardon3DAppState *state);

#endif
