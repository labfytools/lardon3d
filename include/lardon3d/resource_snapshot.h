#ifndef LARDON3D_RESOURCE_SNAPSHOT_H
#define LARDON3D_RESOURCE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/hardware_profile.h>

enum {
    LARDON3D_RESOURCE_SNAPSHOT_MAX_AGE_MILLISECONDS = 1000,
};

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

/* Additive host observation. The legacy snapshot remains ABI-exact and is
 * embedded by value; SwapTotal is telemetry only and never enlarges the
 * Governor RAM budget. A successful capture makes swap_total_known true,
 * including on a host whose exact total is zero. */
typedef struct {
    Lardon3DResourceSnapshot snapshot;
    bool swap_total_known;
    uint64_t swap_total_bytes;
} Lardon3DResourceObservation;

bool lardon3d_resource_snapshot_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    char *error_message,
    size_t error_message_size
);

/* Captures the same bounded /proc and GPU observation as the legacy entry
 * point plus exact SwapTotal telemetry. `profile` and `observation` are
 * required; a non-NULL observation is zeroed before validation and remains
 * caller-owned. `error_message` may be NULL; when supplied with positive size
 * it is always NUL-terminated. No retained pointer or reservation is
 * created, and UMA availability remains the legacy snapshot's one host-RAM
 * quantity rather than an additional budget. */
bool lardon3d_resource_observation_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceObservation *observation,
    char *error_message,
    size_t error_message_size
);

#endif
