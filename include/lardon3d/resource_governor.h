#ifndef LARDON3D_RESOURCE_GOVERNOR_H
#define LARDON3D_RESOURCE_GOVERNOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_snapshot.h>

enum {
    LARDON3D_RESOURCE_REASON_CAPACITY = 256,
};

typedef struct Lardon3DResourceGovernor Lardon3DResourceGovernor;

typedef struct {
    uint64_t system_memory_reserve_bytes;
    uint64_t gpu_memory_reserve_bytes;
    unsigned int system_cpu_reserve;
    double maximum_cpu_load_ratio;
    double maximum_io_pressure_avg10;
} Lardon3DResourcePolicy;

typedef struct {
    uint64_t memory_bytes_per_item;
    uint64_t gpu_memory_bytes_per_item;
    size_t minimum_batch_size;
    size_t preferred_batch_size;
    unsigned int requested_cpu_threads;
    bool io_intensive;
} Lardon3DResourceRequest;

typedef enum {
    LARDON3D_RESOURCE_START = 0,
    LARDON3D_RESOURCE_WAIT,
    LARDON3D_RESOURCE_REDUCE_BATCH,
    LARDON3D_RESOURCE_REJECT
} Lardon3DResourceDecisionKind;

typedef struct {
    Lardon3DResourceDecisionKind kind;
    size_t batch_size;
    unsigned int cpu_threads;
    char reason[LARDON3D_RESOURCE_REASON_CAPACITY];
} Lardon3DResourceDecision;

bool lardon3d_resource_policy_default(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourcePolicy *policy
);
Lardon3DResourceGovernor *lardon3d_resource_governor_create(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy
);
void lardon3d_resource_governor_destroy(
    Lardon3DResourceGovernor *governor
);
bool lardon3d_resource_governor_set_policy(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourcePolicy *policy
);
bool lardon3d_resource_governor_decide(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceRequest *request,
    Lardon3DResourceDecision *decision
);
const char *lardon3d_resource_decision_name(
    Lardon3DResourceDecisionKind kind
);

#endif
