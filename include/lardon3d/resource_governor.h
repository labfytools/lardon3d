#ifndef LARDON3D_RESOURCE_GOVERNOR_H
#define LARDON3D_RESOURCE_GOVERNOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_snapshot.h>

enum {
    LARDON3D_RESOURCE_REASON_CAPACITY = 256,
};

typedef struct Lardon3DResourceGovernor Lardon3DResourceGovernor;
typedef struct Lardon3DResourceReservation Lardon3DResourceReservation;

typedef struct {
    uint64_t system_memory_reserve_bytes;
    uint64_t gpu_memory_reserve_bytes;
    unsigned int system_cpu_reserve;
    double maximum_cpu_load_ratio;
    double maximum_io_pressure_avg10;
    unsigned int gpu_slot_capacity;
    unsigned int io_slot_capacity;
} Lardon3DResourcePolicy;

typedef enum {
    LARDON3D_RESOURCE_TASK_GENERAL = 0,
    LARDON3D_RESOURCE_TASK_IMPORT,
    LARDON3D_RESOURCE_TASK_CPU,
    LARDON3D_RESOURCE_TASK_GPU,
    LARDON3D_RESOURCE_TASK_IO,
    LARDON3D_RESOURCE_TASK_MIXED
} Lardon3DResourceTaskClass;

typedef struct {
    uint64_t memory_fixed_bytes;
    uint64_t gpu_memory_fixed_bytes;
    uint64_t memory_bytes_per_item;
    uint64_t gpu_memory_bytes_per_item;
    size_t minimum_batch_size;
    size_t maximum_batch_size;
    unsigned int desired_cpu_threads;
    unsigned int desired_gpu_slots;
    unsigned int desired_io_slots;
    Lardon3DResourceTaskClass task_class;
} Lardon3DResourceEstimate;

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
    unsigned int gpu_slots;
    unsigned int io_slots;
    char reason[LARDON3D_RESOURCE_REASON_CAPACITY];
} Lardon3DResourceDecision;

typedef enum {
    LARDON3D_RESERVATION_ACTIVE = 0,
    LARDON3D_RESERVATION_RELEASED
} Lardon3DResourceReservationState;

typedef struct {
    uint64_t id;
    uint64_t memory_bytes;
    uint64_t gpu_memory_bytes;
    unsigned int cpu_threads;
    unsigned int gpu_slots;
    unsigned int io_slots;
    size_t batch_size;
    Lardon3DResourceTaskClass task_class;
    Lardon3DResourceReservationState state;
    struct timespec created_at;
} Lardon3DResourceReservationInfo;

typedef struct {
    uint64_t memory_budget_bytes;
    uint64_t memory_reserved_bytes;
    uint64_t memory_available_bytes;
    bool gpu_memory_known;
    uint64_t gpu_memory_budget_bytes;
    uint64_t gpu_memory_reserved_bytes;
    uint64_t gpu_memory_available_bytes;
    unsigned int cpu_budget;
    unsigned int cpu_reserved;
    unsigned int cpu_available;
    unsigned int gpu_slot_budget;
    unsigned int gpu_slots_reserved;
    unsigned int gpu_slots_available;
    unsigned int io_slot_budget;
    unsigned int io_slots_reserved;
    unsigned int io_slots_available;
    size_t active_reservations;
} Lardon3DResourceAvailability;

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
bool lardon3d_resource_governor_reserve(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
);
bool lardon3d_resource_governor_reserve_available(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
);
bool lardon3d_resource_governor_release(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation *reservation
);
bool lardon3d_resource_governor_reservation_is_valid(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation
);
bool lardon3d_resource_reservation_get(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation,
    Lardon3DResourceReservationInfo *information
);
bool lardon3d_resource_reservation_get_active(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation,
    Lardon3DResourceReservationInfo *information
);
size_t lardon3d_resource_governor_list_reservations(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservationInfo *reservations,
    size_t capacity
);
size_t lardon3d_resource_governor_reservation_count(
    Lardon3DResourceGovernor *governor
);
bool lardon3d_resource_governor_availability(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    Lardon3DResourceAvailability *availability
);
uint64_t lardon3d_resource_governor_generation(
    Lardon3DResourceGovernor *governor
);
bool lardon3d_resource_governor_wait_for_change(
    Lardon3DResourceGovernor *governor,
    uint64_t observed_generation,
    uint64_t timeout_ns
);
const char *lardon3d_resource_decision_name(
    Lardon3DResourceDecisionKind kind
);

#endif
