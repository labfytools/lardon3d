#ifndef LARDON3D_RESOURCE_GOVERNOR_INTERNAL_H
#define LARDON3D_RESOURCE_GOVERNOR_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/resource_governor.h>

typedef struct {
    unsigned int pressure_streak;
    unsigned int recovery_streak;
    unsigned int slow_start_streak;
} Lardon3DResourceGovernorInternalCounters;

bool lardon3d_resource_governor_internal_set_next_reservation_id(
    Lardon3DResourceGovernor *governor,
    uint64_t next_reservation_id
);
bool lardon3d_resource_governor_internal_set_counters(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceGovernorInternalCounters *counters
);
bool lardon3d_resource_governor_internal_get_counters(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceGovernorInternalCounters *counters
);
bool lardon3d_resource_governor_internal_set_monotonic_now(
    Lardon3DResourceGovernor *governor,
    const struct timespec *now
);
void lardon3d_resource_governor_internal_force_capture_failure(
    Lardon3DResourceGovernor *governor,
    bool force_failure
);

#endif
