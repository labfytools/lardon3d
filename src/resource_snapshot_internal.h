#ifndef LARDON3D_RESOURCE_SNAPSHOT_INTERNAL_H
#define LARDON3D_RESOURCE_SNAPSHOT_INTERNAL_H

#include <lardon3d/resource_snapshot.h>

void lardon3d_resource_snapshot_capture_gpu_at_root(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    const char *drm_root
);

#endif
