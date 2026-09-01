#ifndef LARDON3D_SSD_CONTROLLER_H
#define LARDON3D_SSD_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LARDON3D_SSD_TEXT_CAPACITY = 128,
    LARDON3D_SSD_IDENTITY_CAPACITY = 256,
    LARDON3D_SSD_PATH_CAPACITY = 256,
    LARDON3D_SSD_REASON_CAPACITY = 256,
    LARDON3D_SSD_MAX_SCRATCH_LEASES = 64,
};

#define LARDON3D_SSD_SWAP_LABEL "LARDON_SWAP"
#define LARDON3D_SSD_SCRATCH_LABEL "LARDON_SCRATCH"
#define LARDON3D_SSD_SCRATCH_MOUNT_PATH "/mnt/lardon-scratch"

typedef struct Lardon3DSsdController Lardon3DSsdController;

typedef enum {
    LARDON3D_SSD_ABSENT = 0,
    LARDON3D_SSD_DETECTED,
    LARDON3D_SSD_ENABLING,
    LARDON3D_SSD_ENABLED,
    LARDON3D_SSD_IN_USE,
    LARDON3D_SSD_DRAINING,
    LARDON3D_SSD_SAFE_TO_UNPLUG,
    LARDON3D_SSD_ERROR,
} Lardon3DSsdState;

typedef enum {
    /* The requested transition was observed and verified. */
    LARDON3D_SSD_CONTROL_OK = 0,
    /* No unsafe action was attempted; retry after the snapshot blocker clears. */
    LARDON3D_SSD_CONTROL_PENDING,
    /* Provider/identity/operation failure; the snapshot retains truthful state. */
    LARDON3D_SSD_CONTROL_ERROR,
} Lardon3DSsdControlResult;

/* A scratch lease is a caller-owned, process-local capability. Its opaque
 * words are never persistent identity and must not be inspected or copied to
 * represent additional use. The exact object address is part of ownership:
 * after acquire, that object must remain alive and unmoved until its successful
 * release. The caller must provide exclusive access to that object for this
 * whole lifetime and must never present the same storage concurrently to
 * different controllers, whose distinct mutexes cannot serialize caller
 * memory. Calls using separate lease objects are thread-safe. Acquire writes a
 * fresh token; release zeros the same object. A copied, constructed, stale,
 * foreign, or already released object is rejected without changing the
 * controller's exact bounded lease count. */
typedef struct {
    uintptr_t opaque_controller;
    uint64_t opaque_lease_id;
} Lardon3DSsdScratchLease;

/* The snapshot is a bounded caller-owned copy. Every string is NUL-terminated.
 * `generation` is monotonic and legally saturates at UINT64_MAX; serialized
 * controller operations may therefore change material state at that terminal
 * watermark. Arbitrary equal-generation copies are not control authority.
 * Unknown optional telemetry has its *_known flag clear and its text, when
 * present, set to "UNKNOWN"; zero is therefore never guessed as knowledge.
 * Device nodes are current observations only. Stable product identity is the
 * paired UDisks Drive identity plus the two filesystem UUIDs. If an owned
 * device disappears or is replaced, that tuple remains reported while paths
 * become UNKNOWN; polling alone cannot clear the physical hazard. */
typedef struct {
    Lardon3DSsdState state;
    uint64_t generation;

    bool device_detected;
    bool pairing_valid;
    bool model_known;
    bool serial_known;
    bool connection_speed_known;
    char model[LARDON3D_SSD_TEXT_CAPACITY];
    char serial[LARDON3D_SSD_TEXT_CAPACITY];
    char drive_identity[LARDON3D_SSD_IDENTITY_CAPACITY];
    uint64_t connection_speed_mbps;

    bool swap_detected;
    bool scratch_detected;
    char swap_uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char scratch_uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char swap_device[LARDON3D_SSD_PATH_CAPACITY];
    char scratch_device[LARDON3D_SSD_PATH_CAPACITY];

    /* UDisks Block.Size is exact partition extent. The usable swap/filesystem
     * totals below are separate and remain unknown until a bounded telemetry
     * source can measure them; a mounted scratch filesystem may legitimately
     * report total/free UNKNOWN. The controller never substitutes one meaning
     * for another or performs an unbounded path lookup during a UI poll. */
    bool swap_partition_size_known;
    bool scratch_partition_size_known;
    uint64_t swap_partition_size_bytes;
    uint64_t scratch_partition_size_bytes;

    bool swap_active;
    bool swap_total_known;
    bool swap_used_known;
    uint64_t swap_total_bytes;
    uint64_t swap_used_bytes;

    bool scratch_mounted;
    bool scratch_total_known;
    bool scratch_free_known;
    uint64_t scratch_total_bytes;
    uint64_t scratch_free_bytes;
    char scratch_mount_path[LARDON3D_SSD_PATH_CAPACITY];

    size_t scratch_lease_count;
    size_t scratch_lease_capacity;
    bool drain_requested;
    /* Exact physical lease authority. This is true only for the validated
     * current Drive/UUID pair at the exact mount path, outside drain/error and
     * below the fixed lease/token capacity. Presentation and the Governor must
     * consume this flag rather than reconstructing authority from fields. */
    bool scratch_allocations_allowed;
    /* Sole control authority for UI/automation. These flags are derived under
     * the controller mutex from the validated current pair plus sticky
     * ownership state. Callers must not reconstruct authority from `state`,
     * paths, labels, activity bits, or an apparently clean ERROR. At most one
     * flag is true in a valid snapshot. */
    bool can_enable;
    bool can_disable;
    bool can_cancel_drain;
    char reason[LARDON3D_SSD_REASON_CAPACITY];
} Lardon3DSsdSnapshot;

/* Creates one synchronous physical-lifecycle controller. The controller owns
 * its GDBus connection/provider and serializes calls internally; it creates no
 * scheduler or background polling thread. Its bounded system-bus connection
 * helper is joined before this call returns. Discovery is cached for at least
 * one second between ordinary observations. NULL reports allocation or
 * provider-initialization failure. */
Lardon3DSsdController *lardon3d_ssd_controller_create(void);

/* Releases all controller/provider resources without mounting, unmounting, or
 * changing swap state. NULL is an idempotent success.
 * Destruction returns false and leaves the controller valid while any scratch
 * lease is outstanding; callers must release those capabilities and retry.
 * No other call may race with a successful destroy. */
bool lardon3d_ssd_controller_destroy(Lardon3DSsdController *controller);

/* Refreshes discovery and telemetry. force=false honors the monotonic
 * one-second cache interval; force=true performs one bounded provider poll and
 * is intended for explicit user actions and tests. A failed poll preserves
 * truthful known active state where possible and publishes ERROR with a
 * bounded reason. Thread-safe. */
bool lardon3d_ssd_controller_refresh(
    Lardon3DSsdController *controller,
    bool force
);

/* Copies the latest state after an ordinary cached refresh. The caller owns
 * `snapshot`; no internal pointer escapes. A non-NULL output is initialized to
 * a bounded all-UNKNOWN/ABSENT snapshot even when controller is NULL. Returns
 * false for a NULL output, invalid controller, or refresh failure (the copied
 * ERROR snapshot is still provided for a valid controller). Thread-safe. */
bool lardon3d_ssd_controller_get_snapshot(
    Lardon3DSsdController *controller,
    Lardon3DSsdSnapshot *snapshot
);

/* Copies the already-cached bounded snapshot without polling UDisks or other
 * provider telemetry. This is the post-operation reconciliation seam used
 * after a serialized physical lease call. Output is initialized on every
 * failure and owned by the caller. Thread-safe. */
bool lardon3d_ssd_controller_copy_snapshot(
    Lardon3DSsdController *controller,
    Lardon3DSsdSnapshot *snapshot
);

/* Enables only the exact healthy paired LARDON_SWAP/LARDON_SCRATCH device.
 * UDisks Swapspace.Start precedes Filesystem.Mount, whose observed path must be
 * exactly LARDON3D_SSD_SCRATCH_MOUNT_PATH. Calls are synchronous but have a
 * bounded D-Bus timeout. Partial success remains active and visible in ERROR;
 * immediately before each potentially side-effecting call, control ownership
 * is conservatively bound to the exact Drive/UUID tuple. A timeout or failed
 * verification can then be recovered only by observing and draining that same
 * pair; a replacement receives no control authority. The controller never
 * formats, repairs, powers off, or deletes data. A verified already-enabled
 * pair returns OK without repeating either action. */
Lardon3DSsdControlResult lardon3d_ssd_controller_enable(
    Lardon3DSsdController *controller
);

/* Requests an idempotent drain. New leases are rejected immediately. Active
 * leases produce PENDING and must be released before stop/unmount can proceed.
 * Swap is stopped only when current PSI/swap-delta evidence is quiet and its
 * used bytes can be absorbed while retaining the 3 GiB MemAvailable reserve.
 * A last lease release automatically retries the pending bounded drain;
 * pressure/telemetry PENDING results are retryable by calling disable again.
 * After an unsafe disappearance, only the reconnected original Drive/UUID
 * tuple is authorized for this drain; its verified inactive endpoint clears
 * the sticky hazard and reaches SAFE_TO_UNPLUG. */
Lardon3DSsdControlResult lardon3d_ssd_controller_disable(
    Lardon3DSsdController *controller
);

/* Cancels a pending drain without changing mount or swap state. A fully active
 * pair resumes ENABLED/IN_USE according to the exact current lease count;
 * partial state remains truthfully DETECTED/ERROR. Returns false for invalid
 * arguments or when no drain is pending. */
bool lardon3d_ssd_controller_cancel_drain(
    Lardon3DSsdController *controller
);

/* Low-level physical seam used by the Governor wrapper and controller tests.
 * Production Task ownership must call
 * lardon3d_resource_governor_acquire_scratch(), not this function directly.
 * Acquires one of the fixed-capacity scratch-use capabilities. `lease` must be
 * non-NULL and zero-initialized (or previously released), and its exclusively
 * owned storage must not move or be copied until release succeeds. Acquisition
 * requires a valid mounted pair in ENABLED/IN_USE and is rejected during
 * DRAINING or ERROR. The lease is operational resource ownership, never
 * Task/scientific identity. */
bool lardon3d_ssd_controller_acquire_scratch(
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
);

/* Low-level physical seam used by the Governor wrapper and controller tests;
 * production Task ownership releases through the matching Governor wrapper.
 * Releases exactly the originally acquired object and zeros it on success.
 * Invalid, copied, constructed, stale, foreign, or double-release objects
 * return false without decrementing use. When this is the final lease of a
 * pending drain, the same call advances the bounded stop/unmount sequence. */
bool lardon3d_ssd_controller_release_scratch(
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
);

const char *lardon3d_ssd_state_name(Lardon3DSsdState state);

#ifdef __cplusplus
}
#endif

#endif
