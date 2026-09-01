#ifndef LARDON3D_RESOURCE_GOVERNOR_H
#define LARDON3D_RESOURCE_GOVERNOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_snapshot.h>
#include <lardon3d/ssd_controller.h>

enum {
    LARDON3D_RESOURCE_REASON_CAPACITY = 256,
    LARDON3D_RESOURCE_EXTERNAL_IDENTITY_CAPACITY = 256,
};

typedef struct Lardon3DResourceGovernor Lardon3DResourceGovernor;
typedef struct Lardon3DResourceReservation Lardon3DResourceReservation;

typedef struct {
    /* Host RAM below this MemAvailable floor is never assigned to new work.
     * The default keeps approximately 3 GiB on capable hosts; smaller hosts
     * use a deterministic fractional reserve. This is an operational desktop
     * safety budget, never a scientific dataset-size limit. */
    uint64_t system_memory_reserve_bytes;
    /* Custom policies may set a lower emergency threshold for immediate RED
     * pressure. The default equals the normal reserve; its separate 4 GiB
     * caution band is private Governor policy and does not reduce capacity. */
    uint64_t emergency_memory_floor_bytes;
    uint64_t gpu_memory_reserve_bytes;
    /* Requested host reserve in logical CPUs. Complete topology may reserve a
     * minimally larger whole-core group, while at least one compute CPU remains. */
    unsigned int system_cpu_reserve;
    double maximum_cpu_load_ratio;
    double maximum_cpu_pressure_avg10;
    double maximum_memory_pressure_avg10;
    double maximum_io_pressure_avg10;
    unsigned int gpu_slot_capacity;
    unsigned int io_slot_capacity;
} Lardon3DResourcePolicy;

typedef enum {
    LARDON3D_RESOURCE_PRESSURE_GREEN = 0,
    LARDON3D_RESOURCE_PRESSURE_YELLOW,
    LARDON3D_RESOURCE_PRESSURE_RED
} Lardon3DResourcePressure;

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

/* External storage is physical operational capacity, never Task/scientific
 * identity and never an extension of host RAM. `generation` is the monotonic
 * source/controller generation; the Governor has a separate aggregate change
 * generation. Unknown byte metrics have their *_known flag clear and value
 * zero. Strings are always NUL-terminated bounded copies; AVAILABLE, IN_USE,
 * DRAINING and SAFE require a nonempty exact stable identity. Controller
 * generation UINT64_MAX is legal saturation, not an invalid sentinel. */
typedef enum {
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT = 0,
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED,
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE,
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE,
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING,
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE,
    LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR,
} Lardon3DResourceExternalStorageStatus;

typedef struct {
    uint64_t generation;
    Lardon3DResourceExternalStorageStatus status;
    bool new_scratch_allocations_allowed;
    bool scratch_total_known;
    bool scratch_free_known;
    uint64_t scratch_total_bytes;
    uint64_t scratch_free_bytes;
    bool swap_total_known;
    bool swap_used_known;
    uint64_t swap_total_bytes;
    uint64_t swap_used_bytes;
    size_t active_scratch_leases;
    char stable_identity[LARDON3D_RESOURCE_EXTERNAL_IDENTITY_CAPACITY];
    char reason[LARDON3D_RESOURCE_REASON_CAPACITY];
} Lardon3DResourceExternalStorage;

bool lardon3d_resource_policy_default(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourcePolicy *policy
);
Lardon3DResourceGovernor *lardon3d_resource_governor_create(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy
);
/* No call may race with destruction. A registered external controller must be
 * unregistered and every Governor scratch lease released first; the bound TUI
 * adapter performs this ordering during normal application shutdown. */
void lardon3d_resource_governor_destroy(
    Lardon3DResourceGovernor *governor
);
bool lardon3d_resource_governor_set_policy(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourcePolicy *policy
);
/* Copies the currently active operational policy under the Governor mutex.
 * The caller owns the output. Observation cannot mutate admission, reserve
 * resources, or turn swap/scratch into RAM capacity. */
bool lardon3d_resource_governor_get_policy(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourcePolicy *policy
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

/* Validates and converts one controller-owned bounded snapshot into the
 * Governor's physical-storage vocabulary. `storage` is initialized to zero on
 * every failure. Pairing, allocation or control authority requires current
 * detection of both partitions, exact Drive/UUID identity and positive known
 * partition extents. Contradictory ABSENT facts are rejected; a disconnected
 * sticky hazard remains representable only as non-authoritative ERROR. Missing
 * optional telemetry remains unknown. This pure conversion performs no
 * controller call and acquires no lock. */
bool lardon3d_resource_external_storage_from_ssd_snapshot(
    const Lardon3DSsdSnapshot *snapshot,
    Lardon3DResourceExternalStorage *storage
);

/* Registers one exact borrowed controller object and copies its initial state
 * under the Governor mutex. The controller must outlive the registration.
 * Registration is exclusive and fails if a controller is already registered;
 * update exact-retries are idempotent. A valid newer source generation may
 * change state, while stale or materially different equal-generation public
 * data cannot restore availability. A conservative ERROR may replace
 * same/older evidence to fail closed. The exact serialized scratch wrapper may
 * reconcile its own completion at a saturated UINT64_MAX watermark; this
 * exception is private provenance and is unavailable to update(). Material
 * changes wake Governor generation waiters; source-generation-only refreshes
 * do not.
 * Unregister is rejected during a wrapper operation or while the last exact
 * snapshot reports any active lease. Outputs from get are caller-owned and
 * zeroed on failure/unregistered state. */
bool lardon3d_resource_governor_register_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    const Lardon3DResourceExternalStorage *storage
);
bool lardon3d_resource_governor_update_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    const Lardon3DResourceExternalStorage *storage
);
bool lardon3d_resource_governor_unregister_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller
);
bool lardon3d_resource_governor_get_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceExternalStorage *storage
);

/* Production scratch ownership crosses the physical-controller boundary only
 * through these wrappers. A lease object remains caller-owned, exclusive, and
 * unmoved exactly as required by the low-level controller. Acquire requires
 * the exact registered controller and current allocation authority; draining,
 * ERROR, absent, stale, or unregistered state fails closed. Release remains
 * available for an exact wrapper-acquired lease during drain/ERROR. The
 * Governor never holds its mutex while entering the controller, and the
 * controller never calls back into the Governor. Registration/controller/
 * Governor must outlive every successful lease; unregister is rejected while
 * one remains. */
bool lardon3d_resource_governor_acquire_scratch(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
);
bool lardon3d_resource_governor_release_scratch(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
);

const char *lardon3d_resource_external_storage_status_name(
    Lardon3DResourceExternalStorageStatus status
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
Lardon3DResourcePressure lardon3d_resource_governor_pressure(
    Lardon3DResourceGovernor *governor
);
/* Enregistre les métriques d'un lot terminé pour l'adaptation dynamique
 * de la taille des lots futurs. batch_size est le nombre d'éléments dont le
 * traitement a été validé dans ce lot. peak_memory_bytes == 0 signifie que
 * la mesure est inconnue et n'alimente jamais l'adaptation mémoire. Le buffer
 * est borné (8 entrées par classe de tâche). Thread-safe. */
bool lardon3d_resource_governor_record_batch(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t batch_size,
    uint64_t duration_ns,
    size_t peak_memory_bytes
);

#endif
