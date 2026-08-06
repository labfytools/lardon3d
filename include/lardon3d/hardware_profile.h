#ifndef LARDON3D_HARDWARE_PROFILE_H
#define LARDON3D_HARDWARE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    LARDON3D_HARDWARE_NAME_CAPACITY = 128,
};

typedef struct {
    unsigned int logical_cpu_count;
    uint64_t page_size_bytes;
    uint64_t memory_total_bytes;
    bool gpu_available;
    bool gpu_memory_known;
    bool gpu_uses_shared_memory;
    uint64_t gpu_memory_total_bytes;
    char cpu_architecture[LARDON3D_HARDWARE_NAME_CAPACITY];
    char gpu_name[LARDON3D_HARDWARE_NAME_CAPACITY];
} Lardon3DHardwareProfile;

bool lardon3d_hardware_profile_detect(
    Lardon3DHardwareProfile *profile,
    char *error_message,
    size_t error_message_size
);

#endif
