#ifndef LARDON3D_PROJECT_H
#define LARDON3D_PROJECT_H

#include <stdbool.h>

#include <lardon3d/app_state.h>

bool lardon3d_project_create(
    Lardon3DAppState *state,
    const char *name
);

bool lardon3d_project_open(
    Lardon3DAppState *state,
    const char *directory_name
);

void lardon3d_project_close(Lardon3DAppState *state);

#endif
