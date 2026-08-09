#ifndef LARDON3D_RESOURCE_SNAPSHOT_H
#define LARDON3D_RESOURCE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/hardware_profile.h>

typedef struct {
    struct timespec captured_at;
    uint64_t memory_available_bytes;
    uint64_t memory_free_bytes;
    uint64_t swap_available_bytes;
    bool gpu_memory_available_known;
    uint64_t gpu_memory_available_bytes;
    double cpu_load_1m;
    double cpu_load_5m;
    double cpu_load_15m;
    bool cpu_pressure_known;
    double cpu_pressure_avg10;
    bool memory_pressure_known;
    double memory_pressure_avg10;
    bool io_pressure_known;
    double io_pressure_avg10;
    bool swap_activity_known;
    uint64_t swap_pages_in;
    uint64_t swap_pages_out;
} Lardon3DResourceSnapshot;

bool lardon3d_resource_snapshot_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    char *error_message,
    size_t error_message_size
);

#endif
