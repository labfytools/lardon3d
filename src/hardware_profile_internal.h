#ifndef LARDON3D_HARDWARE_PROFILE_INTERNAL_H
#define LARDON3D_HARDWARE_PROFILE_INTERNAL_H

#include <lardon3d/hardware_profile.h>

void lardon3d_hardware_profile_detect_gpu_at_root(
    Lardon3DHardwareProfile *profile,
    const char *drm_root
);

#endif
