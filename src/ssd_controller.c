#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "ssd_controller_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    SSD_DBUS_TIMEOUT_MS = 10000,
    SSD_MAX_UDISKS_OBJECTS = 1024,
    SSD_MAX_MOUNT_POINTS = 8,
    SSD_PROC_BUFFER_CAPACITY = 131072,
};

static const uint64_t SSD_DEFAULT_POLL_INTERVAL_NS = UINT64_C(1000000000);
static const uint64_t SSD_HOST_MEMORY_RESERVE_BYTES =
    UINT64_C(3) * UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);
static const double SSD_MEMORY_PSI_ELEVATED_PERCENT = 1.0;
static const double SSD_IO_PSI_ELEVATED_PERCENT = 80.0;

typedef enum {
    SSD_OBSERVATION_ABSENT = 0,
    SSD_OBSERVATION_INCOMPLETE,
    SSD_OBSERVATION_VALID,
    SSD_OBSERVATION_INVALID,
} SsdObservationStatus;

typedef struct {
    uint64_t id;
    Lardon3DSsdScratchLease *address;
} SsdActiveLease;

struct Lardon3DSsdController {
    pthread_mutex_t mutex;
    Lardon3DSsdProvider provider;
    uint64_t minimum_poll_interval_ns;
    bool have_poll_time;
    uint64_t last_poll_ns;
    bool last_poll_succeeded;

    bool have_observation;
    Lardon3DSsdProviderSnapshot observation;
    bool refresh_error;
    char refresh_reason[LARDON3D_SSD_REASON_CAPACITY];
    bool operation_error;
    char operation_reason[LARDON3D_SSD_REASON_CAPACITY];

    bool drain_requested;
    char drain_reason[LARDON3D_SSD_REASON_CAPACITY];
    bool safe_latched;
    char last_drive_identity[LARDON3D_SSD_IDENTITY_CAPACITY];
    char last_swap_uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char last_scratch_uuid[LARDON3D_SSD_TEXT_CAPACITY];
    /* CONTRACT: once physical ownership becomes ambiguous, the exact tuple
     * owned before disappearance/replacement remains authoritative across all
     * later polls. Only a verified inactive drain of this same tuple clears
     * the latch; a replacement must never inherit its control authority. */
    bool physical_hazard_latched;
    char hazard_drive_identity[LARDON3D_SSD_IDENTITY_CAPACITY];
    char hazard_swap_uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char hazard_scratch_uuid[LARDON3D_SSD_TEXT_CAPACITY];

    uint64_t next_lease_id;
    /* Each fixed-capacity record binds the monotonic ID to its original
     * caller-owned object address. Numeric token bytes alone are not release
     * authority and copied/constructed objects cannot decrement use. */
    SsdActiveLease active_leases[LARDON3D_SSD_MAX_SCRATCH_LEASES];
    size_t lease_count;

    uint64_t generation;
    Lardon3DSsdSnapshot snapshot;
};

static void copy_text(char *destination, size_t capacity, const char *source) {
    if (!destination || capacity == 0) {
        return;
    }
    if (!source) {
        source = "";
    }
    (void)snprintf(destination, capacity, "%s", source);
}

static void set_message(char *destination, size_t capacity, const char *format, ...) {
    va_list arguments;

    if (!destination || capacity == 0) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(destination, capacity, format, arguments);
    va_end(arguments);
}

static bool text_is_present(const char *text) {
    return text && text[0] != '\0';
}

static void bump_generation_locked(Lardon3DSsdController *controller) {
    if (controller->generation != UINT64_MAX) {
        controller->generation += 1;
    }
    controller->snapshot.generation = controller->generation;
}

static void initialize_unknown_snapshot(Lardon3DSsdSnapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = LARDON3D_SSD_ABSENT;
    snapshot->scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES;
    copy_text(snapshot->model, sizeof(snapshot->model), "UNKNOWN");
    copy_text(snapshot->serial, sizeof(snapshot->serial), "UNKNOWN");
    copy_text(snapshot->drive_identity, sizeof(snapshot->drive_identity), "UNKNOWN");
    copy_text(snapshot->swap_uuid, sizeof(snapshot->swap_uuid), "UNKNOWN");
    copy_text(snapshot->scratch_uuid, sizeof(snapshot->scratch_uuid), "UNKNOWN");
    copy_text(snapshot->swap_device, sizeof(snapshot->swap_device), "UNKNOWN");
    copy_text(snapshot->scratch_device, sizeof(snapshot->scratch_device), "UNKNOWN");
    copy_text(snapshot->scratch_mount_path, sizeof(snapshot->scratch_mount_path), "UNKNOWN");
    copy_text(snapshot->reason, sizeof(snapshot->reason), "External Lardon SSD is absent");
}

static SsdObservationStatus validate_observation(
    const Lardon3DSsdProviderSnapshot *observation,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    if (observation->invalid_observation) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            text_is_present(observation->invalid_reason)
                ? observation->invalid_reason
                : "Provider returned an invalid SSD observation"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (observation->ambiguous_labels) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Multiple partitions have a reserved Lardon SSD label"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (!observation->swap.present && !observation->scratch.present) {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "External Lardon SSD is absent");
        return SSD_OBSERVATION_ABSENT;
    }
    if (!observation->swap.present || !observation->scratch.present) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            observation->swap.present
                ? "LARDON_SCRATCH partition is missing"
                : "LARDON_SWAP partition is missing"
        );
        return SSD_OBSERVATION_INCOMPLETE;
    }
    if (strcmp(observation->swap.label, LARDON3D_SSD_SWAP_LABEL) != 0
        || strcmp(observation->scratch.label, LARDON3D_SSD_SCRATCH_LABEL) != 0) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Reserved SSD partition labels do not match exactly"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (!text_is_present(observation->swap.uuid)
        || !text_is_present(observation->scratch.uuid)) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Both reserved SSD partitions require stable nonempty UUIDs"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (!text_is_present(observation->swap.drive_identity)
        || !text_is_present(observation->scratch.drive_identity)
        || strcmp(
               observation->swap.drive_identity,
               observation->scratch.drive_identity
           ) != 0) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Reserved SSD partitions do not belong to the same UDisks Drive"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (!observation->swap.unit_ready || !observation->scratch.unit_ready) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Reserved SSD drive reports Unit Not Ready"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (observation->swap.size_bytes == 0 || observation->scratch.size_bytes == 0) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Reserved SSD partition has zero usable size"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (!observation->swap.interface_available
        || !observation->scratch.interface_available
        || !text_is_present(observation->swap.object_path)
        || !text_is_present(observation->scratch.object_path)
        || !text_is_present(observation->swap.device)
        || !text_is_present(observation->scratch.device)) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "Reserved SSD partition lacks a required UDisks interface or current path"
        );
        return SSD_OBSERVATION_INVALID;
    }
    if (!observation->swap.active_known) {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "UDisks Swapspace lacks exact boolean Active telemetry"
        );
        return SSD_OBSERVATION_INVALID;
    }

    reason[0] = '\0';
    return SSD_OBSERVATION_VALID;
}

static bool observation_matches_identity(
    const Lardon3DSsdProviderSnapshot *observation,
    const char *drive_identity,
    const char *swap_uuid,
    const char *scratch_uuid
) {
    return observation && text_is_present(drive_identity)
        && text_is_present(swap_uuid) && text_is_present(scratch_uuid)
        && strcmp(observation->swap.drive_identity, drive_identity) == 0
        && strcmp(observation->scratch.drive_identity, drive_identity) == 0
        && strcmp(observation->swap.uuid, swap_uuid) == 0
        && strcmp(observation->scratch.uuid, scratch_uuid) == 0;
}

static void remember_identity_locked(
    Lardon3DSsdController *controller,
    const Lardon3DSsdProviderSnapshot *observation
) {
    copy_text(
        controller->last_drive_identity,
        sizeof(controller->last_drive_identity),
        observation->swap.drive_identity
    );
    copy_text(
        controller->last_swap_uuid,
        sizeof(controller->last_swap_uuid),
        observation->swap.uuid
    );
    copy_text(
        controller->last_scratch_uuid,
        sizeof(controller->last_scratch_uuid),
        observation->scratch.uuid
    );
}

static void forget_identity_locked(Lardon3DSsdController *controller) {
    controller->last_drive_identity[0] = '\0';
    controller->last_swap_uuid[0] = '\0';
    controller->last_scratch_uuid[0] = '\0';
}

static void latch_physical_hazard_locked(Lardon3DSsdController *controller) {
    if (controller->physical_hazard_latched) {
        return;
    }
    /* WHY: the last accepted tuple predates the poll that made physical
     * ownership ambiguous. Copy it before any later healthy-looking device can
     * replace ordinary discovery state. */
    copy_text(
        controller->hazard_drive_identity,
        sizeof(controller->hazard_drive_identity),
        controller->last_drive_identity
    );
    copy_text(
        controller->hazard_swap_uuid,
        sizeof(controller->hazard_swap_uuid),
        controller->last_swap_uuid
    );
    copy_text(
        controller->hazard_scratch_uuid,
        sizeof(controller->hazard_scratch_uuid),
        controller->last_scratch_uuid
    );
    controller->physical_hazard_latched = true;
}

static bool observation_is_hazard_pair_locked(
    const Lardon3DSsdController *controller
) {
    char reason[LARDON3D_SSD_REASON_CAPACITY];

    return controller->physical_hazard_latched
        && controller->have_observation
        && validate_observation(&controller->observation, reason)
            == SSD_OBSERVATION_VALID
        && observation_matches_identity(
            &controller->observation,
            controller->hazard_drive_identity,
            controller->hazard_swap_uuid,
            controller->hazard_scratch_uuid
        );
}

static void clear_physical_hazard_locked(Lardon3DSsdController *controller) {
    controller->physical_hazard_latched = false;
    controller->hazard_drive_identity[0] = '\0';
    controller->hazard_swap_uuid[0] = '\0';
    controller->hazard_scratch_uuid[0] = '\0';
}

static void derive_state_locked(Lardon3DSsdController *controller);
static bool refresh_locked(Lardon3DSsdController *controller, bool force);

static void bind_possible_enable_side_effect_locked(
    Lardon3DSsdController *controller
) {
    /* CONTRACT: a synchronous D-Bus error/timeout does not prove that Start or
     * Mount had no physical effect. Bind control authority to the exact tuple
     * before entering either call, so even a failed verification cannot let a
     * replacement inherit Stop/Unmount or a later enable attempt. The marker is
     * temporary only when a subsequent valid observation proves this same pair. */
    copy_text(
        controller->hazard_drive_identity,
        sizeof(controller->hazard_drive_identity),
        controller->observation.swap.drive_identity
    );
    copy_text(
        controller->hazard_swap_uuid,
        sizeof(controller->hazard_swap_uuid),
        controller->observation.swap.uuid
    );
    copy_text(
        controller->hazard_scratch_uuid,
        sizeof(controller->hazard_scratch_uuid),
        controller->observation.scratch.uuid
    );
    controller->physical_hazard_latched = true;
    controller->safe_latched = false;
}

static bool refresh_and_resolve_enable_side_effect_locked(
    Lardon3DSsdController *controller
) {
    if (!refresh_locked(controller, true)
        || !observation_is_hazard_pair_locked(controller)) {
        return false;
    }

    /* A verified observation of the original tuple resolves the indeterminate
     * action to current truth. If active/mounted, ordinary remembered ownership
     * remains and any later replacement re-latches the hazard; if inactive and
     * unmounted, there is no physical effect to drain. */
    clear_physical_hazard_locked(controller);
    remember_identity_locked(controller, &controller->observation);
    derive_state_locked(controller);
    return true;
}

static void copy_observation_to_snapshot_locked(Lardon3DSsdController *controller) {
    const Lardon3DSsdProviderSnapshot *source = &controller->observation;
    Lardon3DSsdSnapshot next;

    initialize_unknown_snapshot(&next);
    next.generation = controller->generation;
    next.device_detected = source->swap.present || source->scratch.present;
    next.swap_detected = source->swap.present;
    next.scratch_detected = source->scratch.present;

    if (source->model_known && text_is_present(source->model)) {
        next.model_known = true;
        copy_text(next.model, sizeof(next.model), source->model);
    }
    if (source->serial_known && text_is_present(source->serial)) {
        next.serial_known = true;
        copy_text(next.serial, sizeof(next.serial), source->serial);
    }
    if (source->connection_speed_known) {
        next.connection_speed_known = true;
        next.connection_speed_mbps = source->connection_speed_mbps;
    }

    if (source->swap.present) {
        copy_text(next.swap_uuid, sizeof(next.swap_uuid), source->swap.uuid);
        copy_text(next.swap_device, sizeof(next.swap_device), source->swap.device);
        next.swap_partition_size_known = source->swap.size_bytes > 0;
        next.swap_partition_size_bytes = source->swap.size_bytes;
    }
    if (source->scratch.present) {
        copy_text(next.scratch_uuid, sizeof(next.scratch_uuid), source->scratch.uuid);
        copy_text(next.scratch_device, sizeof(next.scratch_device), source->scratch.device);
        next.scratch_partition_size_known = source->scratch.size_bytes > 0;
        next.scratch_partition_size_bytes = source->scratch.size_bytes;
    }
    if (text_is_present(source->swap.drive_identity)
        && text_is_present(source->scratch.drive_identity)
        && strcmp(source->swap.drive_identity, source->scratch.drive_identity) == 0) {
        copy_text(
            next.drive_identity,
            sizeof(next.drive_identity),
            source->swap.drive_identity
        );
    }

    next.swap_active = source->swap.active;
    next.swap_total_known = source->swap.total_known;
    next.swap_used_known = source->swap.used_known;
    next.swap_total_bytes = source->swap.total_bytes;
    next.swap_used_bytes = source->swap.used_bytes;

    next.scratch_mounted = source->scratch.mounted;
    next.scratch_total_known = source->scratch.total_known;
    next.scratch_free_known = source->scratch.free_known;
    next.scratch_total_bytes = source->scratch.total_bytes;
    next.scratch_free_bytes = source->scratch.free_bytes;
    if (source->scratch.mounted) {
        copy_text(
            next.scratch_mount_path,
            sizeof(next.scratch_mount_path),
            source->scratch.mount_path
        );
    }

    next.scratch_lease_count = controller->lease_count;
    next.scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES;
    next.drain_requested = controller->drain_requested;
    controller->snapshot = next;
}

static void derive_state_locked(Lardon3DSsdController *controller) {
    char validation_reason[LARDON3D_SSD_REASON_CAPACITY];
    const SsdObservationStatus status = controller->have_observation
        ? validate_observation(&controller->observation, validation_reason)
        : SSD_OBSERVATION_ABSENT;
    Lardon3DSsdSnapshot *snapshot = &controller->snapshot;
    const bool recovery_pair = observation_is_hazard_pair_locked(controller);

    snapshot->scratch_lease_count = controller->lease_count;
    snapshot->scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES;
    snapshot->drain_requested = controller->drain_requested;
    snapshot->pairing_valid = status == SSD_OBSERVATION_VALID
        && (!controller->physical_hazard_latched || recovery_pair);
    snapshot->generation = controller->generation;
    snapshot->can_enable = false;
    snapshot->can_disable = false;
    snapshot->can_cancel_drain = false;
    snapshot->scratch_allocations_allowed = false;

    if (controller->physical_hazard_latched) {
        /* The remembered tuple is ownership identity, while device nodes are
         * ephemeral observations. Never combine the original identity with a
         * replacement's paths or activity telemetry. */
        copy_text(
            snapshot->drive_identity,
            sizeof(snapshot->drive_identity),
            text_is_present(controller->hazard_drive_identity)
                ? controller->hazard_drive_identity : "UNKNOWN"
        );
        copy_text(
            snapshot->swap_uuid,
            sizeof(snapshot->swap_uuid),
            text_is_present(controller->hazard_swap_uuid)
                ? controller->hazard_swap_uuid : "UNKNOWN"
        );
        copy_text(
            snapshot->scratch_uuid,
            sizeof(snapshot->scratch_uuid),
            text_is_present(controller->hazard_scratch_uuid)
                ? controller->hazard_scratch_uuid : "UNKNOWN"
        );
        if (!recovery_pair) {
            snapshot->model_known = false;
            snapshot->serial_known = false;
            snapshot->connection_speed_known = false;
            copy_text(snapshot->model, sizeof(snapshot->model), "UNKNOWN");
            copy_text(snapshot->serial, sizeof(snapshot->serial), "UNKNOWN");
            snapshot->connection_speed_mbps = 0;
            copy_text(snapshot->swap_device, sizeof(snapshot->swap_device), "UNKNOWN");
            copy_text(
                snapshot->scratch_device,
                sizeof(snapshot->scratch_device),
                "UNKNOWN"
            );
            copy_text(
                snapshot->scratch_mount_path,
                sizeof(snapshot->scratch_mount_path),
                "UNKNOWN"
            );
            snapshot->swap_active = false;
            snapshot->swap_total_known = false;
            snapshot->swap_used_known = false;
            snapshot->swap_total_bytes = 0;
            snapshot->swap_used_bytes = 0;
            snapshot->scratch_mounted = false;
            snapshot->scratch_total_known = false;
            snapshot->scratch_free_known = false;
            snapshot->scratch_total_bytes = 0;
            snapshot->scratch_free_bytes = 0;
            snapshot->swap_partition_size_known = false;
            snapshot->scratch_partition_size_known = false;
            snapshot->swap_partition_size_bytes = 0;
            snapshot->scratch_partition_size_bytes = 0;
        }
        snapshot->state = LARDON3D_SSD_ERROR;
        if (controller->refresh_error) {
            set_message(
                snapshot->reason,
                sizeof(snapshot->reason),
                "Physical SSD ownership remains unresolved; %s",
                controller->refresh_reason
            );
        } else if (recovery_pair) {
            copy_text(
                snapshot->reason,
                sizeof(snapshot->reason),
                "Original Drive/UUID pair reconnected after an unsafe disappearance; "
                "a verified drain is required"
            );
        } else if (status == SSD_OBSERVATION_VALID) {
            copy_text(
                snapshot->reason,
                sizeof(snapshot->reason),
                "Stable Drive/UUID pair changed while physical ownership is unresolved; "
                "reconnect the original pair to drain it"
            );
        } else if (status == SSD_OBSERVATION_INCOMPLETE
            || status == SSD_OBSERVATION_INVALID) {
            set_message(
                snapshot->reason,
                sizeof(snapshot->reason),
                "Physical SSD ownership remains unresolved: %s",
                validation_reason
            );
        } else {
            copy_text(
                snapshot->reason,
                sizeof(snapshot->reason),
                "External SSD disappeared while active, leased, or draining; "
                "physical ownership remains unresolved"
            );
        }
        goto actions;
    }
    if (controller->refresh_error) {
        snapshot->state = LARDON3D_SSD_ERROR;
        copy_text(snapshot->reason, sizeof(snapshot->reason), controller->refresh_reason);
        goto actions;
    }
    if (status == SSD_OBSERVATION_ABSENT) {
        if (controller->drain_requested) {
            snapshot->state = LARDON3D_SSD_ERROR;
            copy_text(
                snapshot->reason,
                sizeof(snapshot->reason),
                "Cannot drain because the paired external SSD is absent"
            );
        } else {
            snapshot->state = LARDON3D_SSD_ABSENT;
            copy_text(snapshot->reason, sizeof(snapshot->reason), validation_reason);
        }
        goto actions;
    }
    if (status == SSD_OBSERVATION_INCOMPLETE) {
        snapshot->state = controller->drain_requested
            ? LARDON3D_SSD_ERROR
            : LARDON3D_SSD_DETECTED;
        copy_text(snapshot->reason, sizeof(snapshot->reason), validation_reason);
        goto actions;
    }
    if (status == SSD_OBSERVATION_INVALID) {
        snapshot->state = LARDON3D_SSD_ERROR;
        copy_text(snapshot->reason, sizeof(snapshot->reason), validation_reason);
        goto actions;
    }
    if (controller->operation_error) {
        snapshot->state = LARDON3D_SSD_ERROR;
        copy_text(snapshot->reason, sizeof(snapshot->reason), controller->operation_reason);
        goto actions;
    }
    if (controller->safe_latched && !snapshot->swap_active && !snapshot->scratch_mounted) {
        snapshot->state = LARDON3D_SSD_SAFE_TO_UNPLUG;
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            "Swap is inactive and scratch is unmounted; device is safe to unplug"
        );
        goto actions;
    }
    if (controller->drain_requested) {
        snapshot->state = LARDON3D_SSD_DRAINING;
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            text_is_present(controller->drain_reason)
                ? controller->drain_reason
                : "Drain is pending"
        );
        goto actions;
    }
    if (snapshot->swap_active && snapshot->scratch_mounted) {
        if (strcmp(snapshot->scratch_mount_path, LARDON3D_SSD_SCRATCH_MOUNT_PATH) != 0) {
            snapshot->state = LARDON3D_SSD_ERROR;
            copy_text(
                snapshot->reason,
                sizeof(snapshot->reason),
                "Scratch filesystem is mounted at an unexpected path"
            );
            goto actions;
        }
        snapshot->state = controller->lease_count > 0
            ? LARDON3D_SSD_IN_USE
            : LARDON3D_SSD_ENABLED;
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            controller->lease_count > 0
                ? "External SSD is enabled and scratch has active leases"
                : "External SSD swap and scratch are enabled"
        );
        goto actions;
    }
    if (controller->lease_count > 0) {
        snapshot->state = LARDON3D_SSD_ERROR;
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            "Scratch became unavailable while leases remain active"
        );
        goto actions;
    }

    snapshot->state = LARDON3D_SSD_DETECTED;
    if (snapshot->swap_active) {
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            "External SSD is partially active: swap is active but scratch is unmounted"
        );
    } else if (snapshot->scratch_mounted) {
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            "External SSD is partially active: scratch is mounted but swap is inactive"
        );
    } else {
        copy_text(
            snapshot->reason,
            sizeof(snapshot->reason),
            "Healthy paired external SSD is detected but disabled"
        );
    }

actions:
    /* INVARIANT: this is the controller's single lease-authority decision.
     * Governor/TUI code must not approximate it from a friendly-looking state
     * because refresh errors, sticky replacement ownership, token exhaustion,
     * or a pending drain all revoke new allocation without releasing existing
     * lease ownership. */
    snapshot->scratch_allocations_allowed =
        !controller->refresh_error
        && !controller->operation_error
        && !controller->physical_hazard_latched
        && status == SSD_OBSERVATION_VALID
        && snapshot->pairing_valid
        && !controller->drain_requested
        && snapshot->swap_active
        && snapshot->scratch_mounted
        && strcmp(snapshot->scratch_mount_path,
               LARDON3D_SSD_SCRATCH_MOUNT_PATH) == 0
        && controller->lease_count < LARDON3D_SSD_MAX_SCRATCH_LEASES
        && controller->next_lease_id != 0
        && (snapshot->state == LARDON3D_SSD_ENABLED
            || snapshot->state == LARDON3D_SSD_IN_USE);
    /* CONTRACT: presentation never reverse-engineers physical control
     * authority. A failed refresh or replacement exposes no action. The one
     * exception is the freshly observed exact original tuple after a sticky
     * disappearance: it may only be drained, even if already inactive, so
     * that the controller verifies and clears ownership at the endpoint. */
    if (controller->refresh_error) {
        return;
    }
    if (controller->physical_hazard_latched) {
        snapshot->can_disable = recovery_pair;
        return;
    }
    if (status != SSD_OBSERVATION_VALID || !snapshot->pairing_valid) {
        return;
    }
    if (snapshot->state == LARDON3D_SSD_DRAINING) {
        snapshot->can_cancel_drain = controller->drain_requested;
    } else if (snapshot->state == LARDON3D_SSD_ENABLED
        || snapshot->state == LARDON3D_SSD_IN_USE
        || (snapshot->state == LARDON3D_SSD_DETECTED
            && (snapshot->swap_active || snapshot->scratch_mounted))
        || (snapshot->state == LARDON3D_SSD_ERROR
            && (snapshot->swap_active || snapshot->scratch_mounted
                || controller->drain_requested))) {
        snapshot->can_disable = true;
    } else if ((snapshot->state == LARDON3D_SSD_DETECTED
            && !snapshot->swap_active && !snapshot->scratch_mounted)
        || snapshot->state == LARDON3D_SSD_SAFE_TO_UNPLUG) {
        snapshot->can_enable = true;
    }
}

/* The controller owns the only provider instance and serializes every provider
 * callback with this mutex. A normal observation cannot cause a 75 ms UI loop
 * to enumerate D-Bus or /proc repeatedly: monotonic cache age is checked before
 * the provider is entered. User actions intentionally force one bounded poll. */
static bool refresh_locked(Lardon3DSsdController *controller, bool force) {
    uint64_t now_ns = 0;
    char reason[LARDON3D_SSD_REASON_CAPACITY] = {0};
    Lardon3DSsdProviderSnapshot next;
    SsdObservationStatus next_status;
    bool previously_owned;

    if (!controller->provider.ops->monotonic_now_ns(
            controller->provider.context,
            &now_ns
        )) {
        controller->refresh_error = true;
        copy_text(
            controller->refresh_reason,
            sizeof(controller->refresh_reason),
            "Cannot read the monotonic clock for SSD telemetry"
        );
        controller->last_poll_succeeded = false;
        bump_generation_locked(controller);
        derive_state_locked(controller);
        return false;
    }

    if (!force && controller->have_poll_time && now_ns >= controller->last_poll_ns
        && now_ns - controller->last_poll_ns < controller->minimum_poll_interval_ns) {
        return controller->last_poll_succeeded;
    }

    memset(&next, 0, sizeof(next));
    controller->have_poll_time = true;
    controller->last_poll_ns = now_ns;
    if (!controller->provider.ops->refresh(
            controller->provider.context,
            &next,
            reason
        )) {
        controller->refresh_error = true;
        set_message(
            controller->refresh_reason,
            sizeof(controller->refresh_reason),
            "SSD discovery/telemetry failed: %s",
            text_is_present(reason) ? reason : "provider error"
        );
        controller->last_poll_succeeded = false;
        /* Device-node observations are not identity and must never be retained
         * across a failed/disappearance poll. Active booleans stay visible as
         * last-known truth, but their paths become explicitly unknown. */
        copy_text(
            controller->snapshot.swap_device,
            sizeof(controller->snapshot.swap_device),
            "UNKNOWN"
        );
        copy_text(
            controller->snapshot.scratch_device,
            sizeof(controller->snapshot.scratch_device),
            "UNKNOWN"
        );
        bump_generation_locked(controller);
        derive_state_locked(controller);
        return false;
    }

    previously_owned = controller->physical_hazard_latched
        || controller->snapshot.swap_active
        || controller->snapshot.scratch_mounted
        || controller->lease_count > 0
        || (controller->drain_requested
            && text_is_present(controller->last_drive_identity)
            && !controller->safe_latched);
    next_status = validate_observation(&next, reason);

    const bool have_remembered_identity =
        text_is_present(controller->last_drive_identity)
        && text_is_present(controller->last_swap_uuid)
        && text_is_present(controller->last_scratch_uuid);
    const bool same_remembered_pair = next_status == SSD_OBSERVATION_VALID
        && have_remembered_identity
        && observation_matches_identity(
            &next,
            controller->last_drive_identity,
            controller->last_swap_uuid,
            controller->last_scratch_uuid
        );

    /* INVARIANT: successful discovery is not authority transfer. If an owned
     * pair vanishes, becomes invalid, or is replaced, capture its tuple once
     * and keep the hazard sticky. Later polling may only demonstrate that the
     * original tuple has returned; it cannot clear the required drain. */
    if (!controller->physical_hazard_latched && previously_owned
        && have_remembered_identity
        && (next_status != SSD_OBSERVATION_VALID || !same_remembered_pair)) {
        latch_physical_hazard_locked(controller);
        controller->safe_latched = false;
    }

    if (!controller->physical_hazard_latched) {
        if (next_status == SSD_OBSERVATION_VALID) {
            if (!same_remembered_pair) {
                remember_identity_locked(controller, &next);
                controller->safe_latched = false;
            }
        } else if (next_status == SSD_OBSERVATION_ABSENT && !previously_owned) {
            /* A verified safe/inactive disappearance relinquishes ownership;
             * a later healthy pair starts a fresh lifecycle. */
            controller->safe_latched = false;
            forget_identity_locked(controller);
        }
    }
    controller->observation = next;
    controller->have_observation = true;
    controller->refresh_error = false;
    controller->refresh_reason[0] = '\0';
    controller->last_poll_succeeded = true;

    copy_observation_to_snapshot_locked(controller);
    if (controller->snapshot.swap_active || controller->snapshot.scratch_mounted) {
        controller->safe_latched = false;
    }
    bump_generation_locked(controller);
    derive_state_locked(controller);
    return true;
}

static void latch_operation_error_locked(
    Lardon3DSsdController *controller,
    const char *format,
    ...
) {
    va_list arguments;

    controller->operation_error = true;
    va_start(arguments, format);
    (void)vsnprintf(
        controller->operation_reason,
        sizeof(controller->operation_reason),
        format,
        arguments
    );
    va_end(arguments);
    bump_generation_locked(controller);
    derive_state_locked(controller);
}

static bool current_pair_is_valid_locked(Lardon3DSsdController *controller) {
    char reason[LARDON3D_SSD_REASON_CAPACITY];

    return controller->have_observation
        && !controller->physical_hazard_latched
        && validate_observation(&controller->observation, reason) == SSD_OBSERVATION_VALID;
}

static bool current_pair_is_authorized_for_drain_locked(
    Lardon3DSsdController *controller
) {
    char reason[LARDON3D_SSD_REASON_CAPACITY];

    if (!controller->have_observation
        || validate_observation(&controller->observation, reason)
            != SSD_OBSERVATION_VALID) {
        return false;
    }
    return !controller->physical_hazard_latched
        || observation_is_hazard_pair_locked(controller);
}

static Lardon3DSsdControlResult continue_drain_locked(
    Lardon3DSsdController *controller
) {
    char reason[LARDON3D_SSD_REASON_CAPACITY] = {0};

    if (controller->lease_count > 0) {
        set_message(
            controller->drain_reason,
            sizeof(controller->drain_reason),
            "Waiting for %zu scratch lease%s to release",
            controller->lease_count,
            controller->lease_count == 1 ? "" : "s"
        );
        bump_generation_locked(controller);
        derive_state_locked(controller);
        return LARDON3D_SSD_CONTROL_PENDING;
    }

    if (!refresh_locked(controller, true)
        || !current_pair_is_authorized_for_drain_locked(controller)) {
        return LARDON3D_SSD_CONTROL_ERROR;
    }

    if (controller->snapshot.swap_active) {
        const Lardon3DSsdProviderSnapshot *telemetry = &controller->observation;

        if (!controller->snapshot.swap_used_known) {
            copy_text(
                controller->drain_reason,
                sizeof(controller->drain_reason),
                "Cannot stop swap: external swap used bytes are unknown"
            );
            bump_generation_locked(controller);
            derive_state_locked(controller);
            return LARDON3D_SSD_CONTROL_PENDING;
        }
        if (!telemetry->memory_pressure_known || !telemetry->io_pressure_known) {
            copy_text(
                controller->drain_reason,
                sizeof(controller->drain_reason),
                "Cannot stop swap: current memory/I/O PSI evidence is unknown"
            );
            bump_generation_locked(controller);
            derive_state_locked(controller);
            return LARDON3D_SSD_CONTROL_PENDING;
        }
        if (telemetry->memory_pressure_elevated || telemetry->io_pressure_elevated) {
            copy_text(
                controller->drain_reason,
                sizeof(controller->drain_reason),
                telemetry->memory_pressure_elevated
                    ? "Cannot stop swap while memory PSI is elevated"
                    : "Cannot stop swap while I/O PSI is elevated"
            );
            bump_generation_locked(controller);
            derive_state_locked(controller);
            return LARDON3D_SSD_CONTROL_PENDING;
        }
        if (!telemetry->swap_activity_known) {
            copy_text(
                controller->drain_reason,
                sizeof(controller->drain_reason),
                "Cannot stop swap until a swap-in/out delta interval is observed"
            );
            bump_generation_locked(controller);
            derive_state_locked(controller);
            return LARDON3D_SSD_CONTROL_PENDING;
        }
        if (telemetry->swap_pages_in_delta != 0
            || telemetry->swap_pages_out_delta != 0) {
            copy_text(
                controller->drain_reason,
                sizeof(controller->drain_reason),
                "Cannot stop swap while swap-in/out activity is current"
            );
            bump_generation_locked(controller);
            derive_state_locked(controller);
            return LARDON3D_SSD_CONTROL_PENDING;
        }
        if (controller->snapshot.swap_used_bytes > 0) {
            const uint64_t used = controller->snapshot.swap_used_bytes;
            const uint64_t available = telemetry->memory_available_bytes;

            if (!telemetry->memory_available_known) {
                copy_text(
                    controller->drain_reason,
                    sizeof(controller->drain_reason),
                    "Cannot stop used swap: MemAvailable is unknown"
                );
                bump_generation_locked(controller);
                derive_state_locked(controller);
                return LARDON3D_SSD_CONTROL_PENDING;
            }
            /* Subtraction after the reserve comparison avoids reserve+used
             * overflow. The 3 GiB floor is host safety policy only; it never
             * rejects scientific data or becomes Task memory capacity. */
            if (available < SSD_HOST_MEMORY_RESERVE_BYTES
                || used > available - SSD_HOST_MEMORY_RESERVE_BYTES) {
                copy_text(
                    controller->drain_reason,
                    sizeof(controller->drain_reason),
                    "Cannot absorb external swap while retaining 3 GiB MemAvailable"
                );
                bump_generation_locked(controller);
                derive_state_locked(controller);
                return LARDON3D_SSD_CONTROL_PENDING;
            }
        }

        if (!controller->provider.ops->stop_swap(
                controller->provider.context,
                controller->observation.swap.object_path,
                reason
            )) {
            (void)refresh_locked(controller, true);
            latch_operation_error_locked(
                controller,
                "UDisks Swapspace.Stop failed: %s",
                text_is_present(reason) ? reason : "provider error"
            );
            return LARDON3D_SSD_CONTROL_ERROR;
        }
        if (!refresh_locked(controller, true) || controller->snapshot.swap_active) {
            latch_operation_error_locked(
                controller,
                "UDisks reported success but external swap remains active"
            );
            return LARDON3D_SSD_CONTROL_ERROR;
        }
    }

    if (controller->snapshot.scratch_mounted) {
        reason[0] = '\0';
        /* A wrong mount path forbids scratch use but must not make safe cleanup
         * impossible: this still unmounts the exact UUID/Drive-selected Block
         * object. Only controller-owned leases can authorize use. A foreign
         * open file remains protected because normal (non-force) UDisks
         * Unmount reports DeviceBusy instead of forcing. */
        if (!controller->provider.ops->unmount_scratch(
                controller->provider.context,
                controller->observation.scratch.object_path,
                reason
            )) {
            (void)refresh_locked(controller, true);
            latch_operation_error_locked(
                controller,
                "UDisks Filesystem.Unmount failed: %s",
                text_is_present(reason) ? reason : "provider error"
            );
            return LARDON3D_SSD_CONTROL_ERROR;
        }
        if (!refresh_locked(controller, true) || controller->snapshot.scratch_mounted) {
            latch_operation_error_locked(
                controller,
                "UDisks reported success but scratch remains mounted"
            );
            return LARDON3D_SSD_CONTROL_ERROR;
        }
    }

    if (!current_pair_is_authorized_for_drain_locked(controller)
        || controller->snapshot.swap_active
        || controller->snapshot.scratch_mounted) {
        latch_operation_error_locked(
            controller,
            "Safe-to-unplug verification did not observe the stable inactive pair"
        );
        return LARDON3D_SSD_CONTROL_ERROR;
    }

    controller->safe_latched = true;
    controller->drain_requested = false;
    controller->drain_reason[0] = '\0';
    /* Only this verified inactive endpoint releases sticky physical ownership.
     * The ordinary refresh path deliberately has no equivalent transition. */
    clear_physical_hazard_locked(controller);
    bump_generation_locked(controller);
    derive_state_locked(controller);
    return LARDON3D_SSD_CONTROL_OK;
}

Lardon3DSsdController *lardon3d_ssd_controller_create_with_provider(
    Lardon3DSsdProvider provider,
    uint64_t minimum_poll_interval_ns
) {
    Lardon3DSsdController *controller;

    if (!provider.ops || !provider.ops->monotonic_now_ns || !provider.ops->refresh
        || !provider.ops->start_swap || !provider.ops->stop_swap
        || !provider.ops->mount_scratch || !provider.ops->unmount_scratch
        || !provider.ops->destroy
        || minimum_poll_interval_ns < SSD_DEFAULT_POLL_INTERVAL_NS) {
        return NULL;
    }

    controller = calloc(1, sizeof(*controller));
    if (!controller) {
        return NULL;
    }
    if (pthread_mutex_init(&controller->mutex, NULL) != 0) {
        free(controller);
        return NULL;
    }
    controller->provider = provider;
    controller->minimum_poll_interval_ns = minimum_poll_interval_ns;
    controller->next_lease_id = 1;
    initialize_unknown_snapshot(&controller->snapshot);

    (void)pthread_mutex_lock(&controller->mutex);
    (void)refresh_locked(controller, true);
    (void)pthread_mutex_unlock(&controller->mutex);
    return controller;
}

Lardon3DSsdController *lardon3d_ssd_controller_create(void) {
    Lardon3DSsdProvider provider = {0};

    if (!lardon3d_ssd_production_provider_create(&provider)) {
        return NULL;
    }
    Lardon3DSsdController *controller = lardon3d_ssd_controller_create_with_provider(
        provider,
        SSD_DEFAULT_POLL_INTERVAL_NS
    );
    if (!controller) {
        provider.ops->destroy(provider.context);
    }
    return controller;
}

bool lardon3d_ssd_controller_destroy(Lardon3DSsdController *controller) {
    if (!controller) {
        return true;
    }

    (void)pthread_mutex_lock(&controller->mutex);
    if (controller->lease_count != 0) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }
    (void)pthread_mutex_unlock(&controller->mutex);

    /* Public callers must prevent new ingress once successful destruction
     * begins. The mutex serializes calls already inside the controller; unlike
     * a hidden worker, the synchronous provider has no callback to join. */
    controller->provider.ops->destroy(controller->provider.context);
    (void)pthread_mutex_destroy(&controller->mutex);
    free(controller);
    return true;
}

bool lardon3d_ssd_controller_refresh(
    Lardon3DSsdController *controller,
    bool force
) {
    bool result;

    if (!controller) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    result = refresh_locked(controller, force);
    (void)pthread_mutex_unlock(&controller->mutex);
    return result;
}

bool lardon3d_ssd_controller_get_snapshot(
    Lardon3DSsdController *controller,
    Lardon3DSsdSnapshot *snapshot
) {
    bool result;

    if (!snapshot) {
        return false;
    }
    /* CONTRACT: a writable output is always initialized, including the
     * invalid-controller path, so callers never observe stale/unbounded bytes
     * after a failed snapshot request. */
    initialize_unknown_snapshot(snapshot);
    if (!controller) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    result = refresh_locked(controller, false);
    *snapshot = controller->snapshot;
    (void)pthread_mutex_unlock(&controller->mutex);
    return result;
}

bool lardon3d_ssd_controller_copy_snapshot(
    Lardon3DSsdController *controller,
    Lardon3DSsdSnapshot *snapshot
)
{
    if (!snapshot) {
        return false;
    }
    initialize_unknown_snapshot(snapshot);
    if (!controller) {
        return false;
    }
    /* WHY: Governor lease reconciliation already follows a physical call.
     * Re-entering the provider here would add a second potentially blocking
     * observation and create an unnecessary race window. */
    (void)pthread_mutex_lock(&controller->mutex);
    *snapshot = controller->snapshot;
    (void)pthread_mutex_unlock(&controller->mutex);
    return true;
}

#ifdef LARDON3D_SSD_CONTROLLER_TESTING
bool
lardon3d_ssd_controller_set_generation_for_test(
    Lardon3DSsdController *controller,
    uint64_t generation
)
{
    if (!controller) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    controller->generation = generation;
    controller->snapshot.generation = generation;
    (void)pthread_mutex_unlock(&controller->mutex);
    return true;
}

bool
lardon3d_ssd_controller_corrupt_cached_snapshot_for_test(
    Lardon3DSsdController *controller
)
{
    if (!controller) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    memset(controller->snapshot.model, 'X',
        sizeof(controller->snapshot.model));
    (void)pthread_mutex_unlock(&controller->mutex);
    return true;
}
#endif

Lardon3DSsdControlResult lardon3d_ssd_controller_enable(
    Lardon3DSsdController *controller
) {
    char reason[LARDON3D_SSD_REASON_CAPACITY] = {0};
    char mount_path[LARDON3D_SSD_PATH_CAPACITY] = {0};
    Lardon3DSsdControlResult result = LARDON3D_SSD_CONTROL_ERROR;

    if (!controller) {
        return result;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    controller->operation_error = false;
    controller->operation_reason[0] = '\0';
    controller->drain_requested = false;
    controller->drain_reason[0] = '\0';
    controller->safe_latched = false;

    if (!refresh_locked(controller, true) || !current_pair_is_valid_locked(controller)) {
        goto done;
    }
    if (controller->snapshot.scratch_mounted
        && strcmp(
               controller->snapshot.scratch_mount_path,
               LARDON3D_SSD_SCRATCH_MOUNT_PATH
           ) != 0) {
        latch_operation_error_locked(
            controller,
            "Scratch is already mounted at '%s', not '%s'",
            controller->snapshot.scratch_mount_path,
            LARDON3D_SSD_SCRATCH_MOUNT_PATH
        );
        goto done;
    }

    controller->snapshot.state = LARDON3D_SSD_ENABLING;
    copy_text(
        controller->snapshot.reason,
        sizeof(controller->snapshot.reason),
        "Enabling external SSD swap and scratch through UDisks"
    );
    bump_generation_locked(controller);

    if (!controller->snapshot.swap_active) {
        bool action_succeeded;
        bool verification_succeeded;

        bind_possible_enable_side_effect_locked(controller);
        action_succeeded = controller->provider.ops->start_swap(
                controller->provider.context,
                controller->observation.swap.object_path,
                reason
            );
        verification_succeeded =
            refresh_and_resolve_enable_side_effect_locked(controller);
        if (!action_succeeded) {
            latch_operation_error_locked(
                controller,
                "UDisks Swapspace.Start failed: %s",
                text_is_present(reason) ? reason : "provider error"
            );
            goto done;
        }
        if (!verification_succeeded || !controller->snapshot.swap_active) {
            latch_operation_error_locked(
                controller,
                "UDisks reported success but external swap is not active"
            );
            goto done;
        }
    }

    if (!controller->snapshot.scratch_mounted) {
        bool action_succeeded;
        bool verification_succeeded;

        reason[0] = '\0';
        bind_possible_enable_side_effect_locked(controller);
        action_succeeded = controller->provider.ops->mount_scratch(
                controller->provider.context,
                controller->observation.scratch.object_path,
                mount_path,
                reason
            );
        verification_succeeded =
            refresh_and_resolve_enable_side_effect_locked(controller);
        if (!action_succeeded) {
            latch_operation_error_locked(
                controller,
                "UDisks Filesystem.Mount failed: %s",
                text_is_present(reason) ? reason : "provider error"
            );
            goto done;
        }
        if (!verification_succeeded || !controller->snapshot.scratch_mounted) {
            latch_operation_error_locked(
                controller,
                "UDisks reported success but scratch is not mounted"
            );
            goto done;
        }
        if (strcmp(mount_path, LARDON3D_SSD_SCRATCH_MOUNT_PATH) != 0
            || strcmp(
                   controller->snapshot.scratch_mount_path,
                   LARDON3D_SSD_SCRATCH_MOUNT_PATH
               ) != 0) {
            latch_operation_error_locked(
                controller,
                "UDisks mounted scratch at '%s', not '%s'",
                strcmp(
                    controller->snapshot.scratch_mount_path,
                    LARDON3D_SSD_SCRATCH_MOUNT_PATH
                ) != 0
                    ? controller->snapshot.scratch_mount_path
                    : (text_is_present(mount_path) ? mount_path : "UNKNOWN"),
                LARDON3D_SSD_SCRATCH_MOUNT_PATH
            );
            goto done;
        }
    }

    if (!refresh_locked(controller, true)
        || !current_pair_is_valid_locked(controller)
        || !controller->snapshot.swap_active
        || !controller->snapshot.scratch_mounted
        || strcmp(
               controller->snapshot.scratch_mount_path,
               LARDON3D_SSD_SCRATCH_MOUNT_PATH
           ) != 0) {
        latch_operation_error_locked(
            controller,
            "Enable verification did not observe active swap at the exact scratch path"
        );
        goto done;
    }

    controller->operation_error = false;
    controller->operation_reason[0] = '\0';
    bump_generation_locked(controller);
    derive_state_locked(controller);
    result = LARDON3D_SSD_CONTROL_OK;

done:
    (void)pthread_mutex_unlock(&controller->mutex);
    return result;
}

Lardon3DSsdControlResult lardon3d_ssd_controller_disable(
    Lardon3DSsdController *controller
) {
    Lardon3DSsdControlResult result;

    if (!controller) {
        return LARDON3D_SSD_CONTROL_ERROR;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    controller->drain_requested = true;
    controller->safe_latched = false;
    controller->operation_error = false;
    controller->operation_reason[0] = '\0';
    controller->drain_reason[0] = '\0';
    bump_generation_locked(controller);
    derive_state_locked(controller);
    result = continue_drain_locked(controller);
    (void)pthread_mutex_unlock(&controller->mutex);
    return result;
}

bool lardon3d_ssd_controller_cancel_drain(
    Lardon3DSsdController *controller
) {
    if (!controller) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    if (!controller->drain_requested) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }
    controller->drain_requested = false;
    controller->drain_reason[0] = '\0';
    controller->operation_error = false;
    controller->operation_reason[0] = '\0';
    controller->safe_latched = false;
    bump_generation_locked(controller);
    derive_state_locked(controller);
    (void)pthread_mutex_unlock(&controller->mutex);
    return true;
}

bool lardon3d_ssd_controller_acquire_scratch(
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
) {
    size_t slot;
    uint64_t lease_id;

    if (!controller || !lease) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    /* INVARIANT: the controller mutex serializes both token bytes and the
     * address registry. Check the registry first so reconstructing or zeroing
     * an active object cannot acquire a second capability at that address. */
    for (slot = 0; slot < LARDON3D_SSD_MAX_SCRATCH_LEASES; ++slot) {
        if (controller->active_leases[slot].id != 0
            && controller->active_leases[slot].address == lease) {
            (void)pthread_mutex_unlock(&controller->mutex);
            return false;
        }
    }
    if (lease->opaque_controller != 0 || lease->opaque_lease_id != 0) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }
    if (!refresh_locked(controller, false)
        || controller->drain_requested
        || controller->operation_error
        || !current_pair_is_valid_locked(controller)
        || !controller->snapshot.swap_active
        || !controller->snapshot.scratch_mounted
        || strcmp(
               controller->snapshot.scratch_mount_path,
               LARDON3D_SSD_SCRATCH_MOUNT_PATH
           ) != 0
        || controller->lease_count >= LARDON3D_SSD_MAX_SCRATCH_LEASES
        || controller->next_lease_id == 0) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    for (slot = 0; slot < LARDON3D_SSD_MAX_SCRATCH_LEASES; ++slot) {
        if (controller->active_leases[slot].id == 0) {
            break;
        }
    }
    if (slot == LARDON3D_SSD_MAX_SCRATCH_LEASES) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    lease_id = controller->next_lease_id;
    controller->next_lease_id += 1;
    controller->active_leases[slot].id = lease_id;
    controller->active_leases[slot].address = lease;
    controller->lease_count += 1;
    lease->opaque_controller = (uintptr_t)controller;
    lease->opaque_lease_id = lease_id;
    bump_generation_locked(controller);
    derive_state_locked(controller);
    (void)pthread_mutex_unlock(&controller->mutex);
    return true;
}

bool lardon3d_ssd_controller_release_scratch(
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
) {
    size_t slot;
    bool found = false;

    if (!controller || !lease) {
        return false;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    /* CONTRACT: all reads and writes of caller token storage occur under the
     * same mutex as the fixed address/ID registry. Two same-controller calls
     * therefore cannot race the lease fields or decrement one use twice. */
    if (lease->opaque_controller != (uintptr_t)controller
        || lease->opaque_lease_id == 0) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }
    for (slot = 0; slot < LARDON3D_SSD_MAX_SCRATCH_LEASES; ++slot) {
        if (controller->active_leases[slot].id == lease->opaque_lease_id
            && controller->active_leases[slot].address == lease) {
            controller->active_leases[slot].id = 0;
            controller->active_leases[slot].address = NULL;
            found = true;
            break;
        }
    }
    if (!found || controller->lease_count == 0) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    controller->lease_count -= 1;
    lease->opaque_controller = 0;
    lease->opaque_lease_id = 0;
    bump_generation_locked(controller);
    derive_state_locked(controller);
    if (controller->drain_requested && controller->lease_count == 0) {
        (void)continue_drain_locked(controller);
    }
    (void)pthread_mutex_unlock(&controller->mutex);
    return true;
}

const char *lardon3d_ssd_state_name(Lardon3DSsdState state) {
    switch (state) {
        case LARDON3D_SSD_ABSENT:
            return "ABSENT";
        case LARDON3D_SSD_DETECTED:
            return "DETECTED";
        case LARDON3D_SSD_ENABLING:
            return "ENABLING";
        case LARDON3D_SSD_ENABLED:
            return "ENABLED";
        case LARDON3D_SSD_IN_USE:
            return "IN_USE";
        case LARDON3D_SSD_DRAINING:
            return "DRAINING";
        case LARDON3D_SSD_SAFE_TO_UNPLUG:
            return "SAFE_TO_UNPLUG";
        case LARDON3D_SSD_ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

typedef struct {
    GDBusConnection *connection;
    bool have_vmstat_baseline;
    uint64_t previous_pages_in;
    uint64_t previous_pages_out;
    uint64_t previous_vmstat_ns;
} SsdProductionProvider;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool completed;
    struct timespec deadline;
    GCancellable *cancellable;
} SsdBusConnectDeadline;

static const char *const SSD_UDISKS_SERVICE = "org.freedesktop.UDisks2";
static const char *const SSD_UDISKS_ROOT = "/org/freedesktop/UDisks2";
static const char *const SSD_OBJECT_MANAGER_INTERFACE =
    "org.freedesktop.DBus.ObjectManager";
static const char *const SSD_BLOCK_INTERFACE = "org.freedesktop.UDisks2.Block";
static const char *const SSD_DRIVE_INTERFACE = "org.freedesktop.UDisks2.Drive";
static const char *const SSD_SWAP_INTERFACE = "org.freedesktop.UDisks2.Swapspace";
static const char *const SSD_FILESYSTEM_INTERFACE =
    "org.freedesktop.UDisks2.Filesystem";

static void *cancel_bus_connect_at_deadline(void *userdata) {
    SsdBusConnectDeadline *deadline = userdata;

    (void)pthread_mutex_lock(&deadline->mutex);
    while (!deadline->completed) {
        const int result = pthread_cond_timedwait(
            &deadline->condition,
            &deadline->mutex,
            &deadline->deadline
        );
        if (result == ETIMEDOUT) {
            if (!deadline->completed) {
                g_cancellable_cancel(deadline->cancellable);
            }
            break;
        }
        if (result != 0) {
            g_cancellable_cancel(deadline->cancellable);
            break;
        }
    }
    (void)pthread_mutex_unlock(&deadline->mutex);
    return NULL;
}

static GDBusConnection *connect_system_bus_bounded(GError **error) {
    SsdBusConnectDeadline deadline;
    pthread_t timeout_thread;
    pthread_condattr_t condition_attributes;
    GDBusConnection *connection;
    const time_t timeout_seconds = (time_t)(SSD_DBUS_TIMEOUT_MS / 1000);

    memset(&deadline, 0, sizeof(deadline));
    deadline.cancellable = g_cancellable_new();
    if (!deadline.cancellable || clock_gettime(CLOCK_MONOTONIC, &deadline.deadline) != 0
        || deadline.deadline.tv_sec > (time_t)(INT64_MAX - timeout_seconds)) {
        if (deadline.cancellable) {
            g_object_unref(deadline.cancellable);
        }
        return NULL;
    }
    deadline.deadline.tv_sec += timeout_seconds;
    if (pthread_mutex_init(&deadline.mutex, NULL) != 0) {
        g_object_unref(deadline.cancellable);
        return NULL;
    }
    if (pthread_condattr_init(&condition_attributes) != 0) {
        (void)pthread_mutex_destroy(&deadline.mutex);
        g_object_unref(deadline.cancellable);
        return NULL;
    }
    if (pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC) != 0
        || pthread_cond_init(&deadline.condition, &condition_attributes) != 0) {
        (void)pthread_condattr_destroy(&condition_attributes);
        (void)pthread_mutex_destroy(&deadline.mutex);
        g_object_unref(deadline.cancellable);
        return NULL;
    }
    (void)pthread_condattr_destroy(&condition_attributes);
    if (pthread_create(
            &timeout_thread,
            NULL,
            cancel_bus_connect_at_deadline,
            &deadline
        ) != 0) {
        (void)pthread_cond_destroy(&deadline.condition);
        (void)pthread_mutex_destroy(&deadline.mutex);
        g_object_unref(deadline.cancellable);
        return NULL;
    }

    /* GIO honors GCancellable while opening the local system bus. The helper's
     * condition uses a monotonic deadline, exists only for this synchronous
     * constructor call, and is joined before return; no callback/thread
     * lifetime escapes into controller state. */
    connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, deadline.cancellable, error);
    (void)pthread_mutex_lock(&deadline.mutex);
    deadline.completed = true;
    (void)pthread_cond_broadcast(&deadline.condition);
    (void)pthread_mutex_unlock(&deadline.mutex);
    (void)pthread_join(timeout_thread, NULL);

    (void)pthread_cond_destroy(&deadline.condition);
    (void)pthread_mutex_destroy(&deadline.mutex);
    g_object_unref(deadline.cancellable);
    return connection;
}

static bool production_monotonic_now_ns(void *context, uint64_t *now_ns) {
    struct timespec now;

    (void)context;
    if (!now_ns || clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0
        || (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    *now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}

static void format_gerror(
    char reason[LARDON3D_SSD_REASON_CAPACITY],
    const GError *error
) {
    char *remote_name;

    if (!error) {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "unknown D-Bus error");
        return;
    }
    remote_name = g_dbus_error_get_remote_error(error);
    if (remote_name) {
        set_message(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            "%s: %s",
            remote_name,
            error->message ? error->message : "D-Bus call failed"
        );
        g_free(remote_name);
    } else {
        copy_text(
            reason,
            LARDON3D_SSD_REASON_CAPACITY,
            error->message ? error->message : "D-Bus call failed"
        );
    }
    for (size_t index = 0; reason[index] != '\0'; ++index) {
        if (reason[index] == '\n' || reason[index] == '\r') {
            reason[index] = ' ';
        }
    }
}

static bool copy_variant_string(
    GVariant *properties,
    const char *key,
    char *destination,
    size_t capacity
) {
    const char *value = NULL;

    if (!properties || !g_variant_lookup(properties, key, "&s", &value)
        || !value || strlen(value) >= capacity) {
        return false;
    }
    copy_text(destination, capacity, value);
    return true;
}

static bool copy_variant_object_path(
    GVariant *properties,
    const char *key,
    char *destination,
    size_t capacity
) {
    const char *value = NULL;

    if (!properties || !g_variant_lookup(properties, key, "&o", &value)
        || !value || strlen(value) >= capacity) {
        return false;
    }
    copy_text(destination, capacity, value);
    return true;
}

static bool copy_variant_bytestring(
    GVariant *properties,
    const char *key,
    char *destination,
    size_t capacity
) {
    GVariant *value;
    gsize length = 0;
    const guint8 *bytes;

    if (!properties) {
        return false;
    }
    value = g_variant_lookup_value(properties, key, G_VARIANT_TYPE_BYTESTRING);
    if (!value) {
        return false;
    }
    bytes = g_variant_get_fixed_array(value, &length, sizeof(*bytes));
    if (!bytes || length < 2 || bytes[length - 1] != 0 || length > capacity
        || memchr(bytes, 0, length - 1) != NULL) {
        g_variant_unref(value);
        return false;
    }
    memcpy(destination, bytes, length);
    g_variant_unref(value);
    return true;
}

static bool read_file_bounded(
    const char *path,
    char *buffer,
    size_t capacity
) {
    int descriptor;
    size_t used = 0;

    if (!path || !buffer || capacity < 2) {
        return false;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    while (used < capacity - 1) {
        const ssize_t count = read(descriptor, buffer + used, capacity - 1 - used);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno != EINTR) {
            (void)close(descriptor);
            return false;
        }
    }
    if (used == capacity - 1) {
        char extra;
        ssize_t count;
        do {
            count = read(descriptor, &extra, 1);
        } while (count < 0 && errno == EINTR);
        if (count != 0) {
            (void)close(descriptor);
            return false;
        }
    }
    if (close(descriptor) != 0) {
        return false;
    }
    buffer[used] = '\0';
    return true;
}

static bool parse_positive_u64_text(const char *text, uint64_t *value) {
    char *end = NULL;
    uintmax_t parsed;

    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || parsed == 0 || parsed > UINT64_MAX) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static void populate_connection_speed_from_sysfs(
    Lardon3DSsdProviderSnapshot *snapshot
) {
    char class_path[PATH_MAX];
    char resolved[PATH_MAX];
    char speed_path[PATH_MAX];
    char speed_text[128];
    const char *device;
    const char *name;
    int class_path_length;

    if (snapshot->connection_speed_known || !snapshot->scratch.present) {
        return;
    }
    device = snapshot->scratch.device;
    name = strrchr(device, '/');
    name = name ? name + 1 : device;
    if (!text_is_present(name) || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    class_path_length = snprintf(
        class_path,
        sizeof(class_path),
        "/sys/class/block/%s",
        name
    );
    if (class_path_length < 0 || (size_t)class_path_length >= sizeof(class_path)
        || !realpath(class_path, resolved)) {
        return;
    }

    /* Standard UDisks Drive metadata exposes the connection bus but not the
     * negotiated rate. That rate is optional telemetry, not product identity:
     * walk only the bounded resolved sysfs ancestry of the current UDisks
     * device. A future sdX rename therefore changes neither pairing nor
     * leases. Linux USB `speed` files are expressed in Mb/s. */
    for (size_t depth = 0; depth < 32; ++depth) {
        const int length = snprintf(speed_path, sizeof(speed_path), "%s/speed", resolved);
        if (length > 0 && (size_t)length < sizeof(speed_path)
            && read_file_bounded(speed_path, speed_text, sizeof(speed_text))
            && parse_positive_u64_text(
                speed_text,
                &snapshot->connection_speed_mbps
            )) {
            snapshot->connection_speed_known = true;
            return;
        }

        char *slash = strrchr(resolved, '/');
        if (!slash || slash == resolved || strcmp(resolved, "/sys") == 0) {
            return;
        }
        *slash = '\0';
    }
}

static bool parse_named_u64(const char *text, const char *name, uint64_t *value) {
    const size_t name_length = strlen(name);
    const char *cursor = text;

    while (cursor && *cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        const size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length > name_length && strncmp(cursor, name, name_length) == 0
            && (cursor[name_length] == ' ' || cursor[name_length] == '\t')) {
            char *parse_end = NULL;
            errno = 0;
            const uintmax_t parsed = strtoumax(cursor + name_length, &parse_end, 10);
            if (errno != 0 || parse_end == cursor + name_length || parsed > UINT64_MAX) {
                return false;
            }
            *value = (uint64_t)parsed;
            return true;
        }
        cursor = end ? end + 1 : NULL;
    }
    return false;
}

static bool parse_mem_available(const char *text, uint64_t *bytes) {
    const char *line = strstr(text, "MemAvailable:");
    char *end = NULL;
    uintmax_t kib;

    if (!line) {
        return false;
    }
    line += strlen("MemAvailable:");
    errno = 0;
    kib = strtoumax(line, &end, 10);
    if (errno != 0 || end == line || kib > UINT64_MAX / UINT64_C(1024)) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    if (strncmp(end, "kB", 2) != 0) {
        return false;
    }
    *bytes = (uint64_t)kib * UINT64_C(1024);
    return true;
}

static bool parse_psi_elevated(
    const char *text,
    double elevated_threshold,
    bool *elevated
) {
    const char *some = strstr(text, "some ");
    const char *average;
    char *end = NULL;
    double value;

    if (!some || !(average = strstr(some, "avg10="))) {
        return false;
    }
    average += strlen("avg10=");
    errno = 0;
    value = strtod(average, &end);
    if (errno != 0 || end == average || value < 0.0) {
        return false;
    }
    /* These are the established default Governor pressure boundaries (1%
     * memory, 80% I/O avg10), reused as physical-removal safety evidence
     * without adding a second scheduler or redefining admission. Historical
     * swap occupancy is deliberately irrelevant here. */
    *elevated = value >= elevated_threshold;
    return true;
}

static void populate_host_telemetry(
    SsdProductionProvider *provider,
    Lardon3DSsdProviderSnapshot *snapshot
) {
    char buffer[SSD_PROC_BUFFER_CAPACITY];
    uint64_t pages_in;
    uint64_t pages_out;
    uint64_t now_ns;

    if (read_file_bounded("/proc/meminfo", buffer, sizeof(buffer))) {
        snapshot->memory_available_known = parse_mem_available(
            buffer,
            &snapshot->memory_available_bytes
        );
    }
    if (read_file_bounded("/proc/pressure/memory", buffer, sizeof(buffer))) {
        snapshot->memory_pressure_known = parse_psi_elevated(
            buffer,
            SSD_MEMORY_PSI_ELEVATED_PERCENT,
            &snapshot->memory_pressure_elevated
        );
    }
    if (read_file_bounded("/proc/pressure/io", buffer, sizeof(buffer))) {
        snapshot->io_pressure_known = parse_psi_elevated(
            buffer,
            SSD_IO_PSI_ELEVATED_PERCENT,
            &snapshot->io_pressure_elevated
        );
    }
    if (!read_file_bounded("/proc/vmstat", buffer, sizeof(buffer))
        || !parse_named_u64(buffer, "pswpin", &pages_in)
        || !parse_named_u64(buffer, "pswpout", &pages_out)
        || !production_monotonic_now_ns(provider, &now_ns)) {
        return;
    }
    if (!provider->have_vmstat_baseline || now_ns < provider->previous_vmstat_ns
        || pages_in < provider->previous_pages_in
        || pages_out < provider->previous_pages_out) {
        provider->previous_pages_in = pages_in;
        provider->previous_pages_out = pages_out;
        provider->previous_vmstat_ns = now_ns;
        provider->have_vmstat_baseline = true;
        return;
    }
    /* A force-refresh performed immediately after construction must not turn
     * a near-zero sampling window into false proof of quiet swap. Accumulate
     * against the existing baseline until at least the normal poll interval
     * has elapsed. */
    if (now_ns - provider->previous_vmstat_ns >= SSD_DEFAULT_POLL_INTERVAL_NS) {
        snapshot->swap_activity_known = true;
        snapshot->swap_pages_in_delta = pages_in - provider->previous_pages_in;
        snapshot->swap_pages_out_delta = pages_out - provider->previous_pages_out;
        provider->previous_pages_in = pages_in;
        provider->previous_pages_out = pages_out;
        provider->previous_vmstat_ns = now_ns;
    }
}

static void populate_swap_usage(Lardon3DSsdProviderSnapshot *snapshot) {
    char buffer[SSD_PROC_BUFFER_CAPACITY];
    char *save = NULL;
    char *line;

    /* Block.Size validates a nonzero partition but is not exact usable swap
     * capacity. Publish total/used only from the kernel's active swap table. */
    if (!snapshot->swap.present) {
        return;
    }
    if (!read_file_bounded("/proc/swaps", buffer, sizeof(buffer))) {
        if (!snapshot->swap.active) {
            snapshot->swap.used_known = true;
            snapshot->swap.used_bytes = 0;
        }
        return;
    }
    (void)strtok_r(buffer, "\n", &save); /* Skip the fixed /proc/swaps header. */
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        char device[LARDON3D_SSD_PATH_CAPACITY];
        char type[32];
        uintmax_t total_kib;
        uintmax_t used_kib;
        int priority;

        if (sscanf(
                line,
                "%255s %31s %" SCNuMAX " %" SCNuMAX " %d",
                device,
                type,
                &total_kib,
                &used_kib,
                &priority
            ) != 5
            || strcmp(device, snapshot->swap.device) != 0) {
            continue;
        }
        (void)type;
        (void)priority;
        /* /proc/swaps is kernel truth and may be newer than the D-Bus property
         * cache. A matching row can only strengthen Active; a stale false must
         * never lead to SAFE_TO_UNPLUG. */
        snapshot->swap.active = true;
        if (total_kib <= UINT64_MAX / UINT64_C(1024)
            && used_kib <= UINT64_MAX / UINT64_C(1024)) {
            snapshot->swap.total_known = true;
            snapshot->swap.total_bytes = (uint64_t)total_kib * UINT64_C(1024);
            snapshot->swap.used_known = true;
            snapshot->swap.used_bytes = (uint64_t)used_kib * UINT64_C(1024);
        }
        return;
    }
    if (!snapshot->swap.active) {
        snapshot->swap.used_known = true;
        snapshot->swap.used_bytes = 0;
    }
}

static bool copy_mount_points(
    GVariant *filesystem,
    Lardon3DSsdProviderScratch *scratch,
    Lardon3DSsdProviderSnapshot *snapshot
) {
    GVariant *mounts;
    GVariantIter iterator;
    GVariant *encoded;
    size_t count = 0;

    mounts = g_variant_lookup_value(
        filesystem,
        "MountPoints",
        G_VARIANT_TYPE("aay")
    );
    if (!mounts) {
        snapshot->invalid_observation = true;
        copy_text(
            snapshot->invalid_reason,
            sizeof(snapshot->invalid_reason),
            "UDisks Filesystem lacks MountPoints telemetry"
        );
        return false;
    }
    g_variant_iter_init(&iterator, mounts);
    while ((encoded = g_variant_iter_next_value(&iterator)) != NULL) {
        gsize length = 0;
        const guint8 *mount_path = g_variant_get_fixed_array(
            encoded,
            &length,
            sizeof(*mount_path)
        );
        count += 1;
        if (count > SSD_MAX_MOUNT_POINTS || !mount_path || length < 2
            || mount_path[length - 1] != 0 || length > sizeof(scratch->mount_path)
            || memchr(mount_path, 0, length - 1) != NULL) {
            snapshot->invalid_observation = true;
            copy_text(
                snapshot->invalid_reason,
                sizeof(snapshot->invalid_reason),
                "UDisks returned too many or overlong scratch mount points"
            );
            g_variant_unref(encoded);
            g_variant_unref(mounts);
            return false;
        }
        if (count == 1) {
            memcpy(scratch->mount_path, mount_path, length);
        }
        g_variant_unref(encoded);
    }
    g_variant_unref(mounts);
    if (count > 1) {
        snapshot->invalid_observation = true;
        copy_text(
            snapshot->invalid_reason,
            sizeof(snapshot->invalid_reason),
            "Scratch filesystem has multiple mount points"
        );
        return false;
    }
    scratch->mounted = count == 1 && text_is_present(scratch->mount_path);
    return true;
}

static void populate_volume_from_block(
    const char *object_path,
    GVariant *interfaces,
    GVariant *block,
    bool is_swap,
    Lardon3DSsdProviderSnapshot *snapshot
) {
    uint64_t size = 0;
    char label[LARDON3D_SSD_TEXT_CAPACITY] = {0};
    char uuid[LARDON3D_SSD_TEXT_CAPACITY] = {0};
    char drive[LARDON3D_SSD_IDENTITY_CAPACITY] = {0};
    char device[LARDON3D_SSD_PATH_CAPACITY] = {0};

    if (!copy_variant_string(block, "IdLabel", label, sizeof(label))) {
        return;
    }
    if ((is_swap && strcmp(label, LARDON3D_SSD_SWAP_LABEL) != 0)
        || (!is_swap && strcmp(label, LARDON3D_SSD_SCRATCH_LABEL) != 0)) {
        return;
    }

    if (!g_variant_lookup(block, "Size", "t", &size)
        || !copy_variant_string(block, "IdUUID", uuid, sizeof(uuid))
        || !copy_variant_object_path(block, "Drive", drive, sizeof(drive))
        || !copy_variant_bytestring(block, "Device", device, sizeof(device))
        || strlen(object_path) >= LARDON3D_SSD_OBJECT_PATH_CAPACITY) {
        snapshot->invalid_observation = true;
        copy_text(
            snapshot->invalid_reason,
            sizeof(snapshot->invalid_reason),
            "Reserved UDisks Block properties are missing or overlong"
        );
        return;
    }

    if (is_swap) {
        GVariant *swap = g_variant_lookup_value(
            interfaces,
            SSD_SWAP_INTERFACE,
            G_VARIANT_TYPE_VARDICT
        );
        if (snapshot->swap.present) {
            snapshot->ambiguous_labels = true;
            if (swap) {
                g_variant_unref(swap);
            }
            return;
        }
        snapshot->swap.present = true;
        snapshot->swap.size_bytes = size;
        copy_text(snapshot->swap.label, sizeof(snapshot->swap.label), label);
        copy_text(snapshot->swap.uuid, sizeof(snapshot->swap.uuid), uuid);
        copy_text(
            snapshot->swap.drive_identity,
            sizeof(snapshot->swap.drive_identity),
            drive
        );
        copy_text(snapshot->swap.object_path, sizeof(snapshot->swap.object_path), object_path);
        copy_text(snapshot->swap.device, sizeof(snapshot->swap.device), device);
        snapshot->swap.interface_available = swap != NULL;
        if (swap) {
            GVariant *active = g_variant_lookup_value(
                swap,
                "Active",
                G_VARIANT_TYPE_BOOLEAN
            );

            /* CONTRACT: Swapspace.Active is required control telemetry, not an
             * optional display value. Missing or differently typed storage is
             * corruption at the provider boundary and must never become the
             * unsafe default "inactive" used by drain/Safe-to-Unplug logic. */
            if (!active) {
                snapshot->invalid_observation = true;
                copy_text(
                    snapshot->invalid_reason,
                    sizeof(snapshot->invalid_reason),
                    "UDisks Swapspace lacks exact boolean Active telemetry"
                );
            } else {
                snapshot->swap.active_known = true;
                snapshot->swap.active = g_variant_get_boolean(active) != FALSE;
                g_variant_unref(active);
            }
            g_variant_unref(swap);
        }
    } else {
        GVariant *filesystem = g_variant_lookup_value(
            interfaces,
            SSD_FILESYSTEM_INTERFACE,
            G_VARIANT_TYPE_VARDICT
        );
        if (snapshot->scratch.present) {
            snapshot->ambiguous_labels = true;
            if (filesystem) {
                g_variant_unref(filesystem);
            }
            return;
        }
        snapshot->scratch.present = true;
        snapshot->scratch.size_bytes = size;
        copy_text(snapshot->scratch.label, sizeof(snapshot->scratch.label), label);
        copy_text(snapshot->scratch.uuid, sizeof(snapshot->scratch.uuid), uuid);
        copy_text(
            snapshot->scratch.drive_identity,
            sizeof(snapshot->scratch.drive_identity),
            drive
        );
        copy_text(
            snapshot->scratch.object_path,
            sizeof(snapshot->scratch.object_path),
            object_path
        );
        copy_text(snapshot->scratch.device, sizeof(snapshot->scratch.device), device);
        snapshot->scratch.interface_available = filesystem != NULL;
        if (filesystem) {
            (void)copy_mount_points(filesystem, &snapshot->scratch, snapshot);
            g_variant_unref(filesystem);
        }
    }
}

static void populate_drive_properties(
    GVariant *objects,
    const char *drive_identity,
    Lardon3DSsdProviderSnapshot *snapshot,
    bool *unit_ready
) {
    GVariant *interfaces;
    GVariant *drive;
    /* GLib's boolean ABI is wider than C17 bool and must not write directly
     * into controller storage. Missing MediaAvailable is already conservative:
     * it leaves Unit Not Ready and cannot authorize an action. */
    gboolean media_available = FALSE;

    *unit_ready = false;
    interfaces = g_variant_lookup_value(
        objects,
        drive_identity,
        G_VARIANT_TYPE("a{sa{sv}}")
    );
    if (!interfaces) {
        return;
    }
    drive = g_variant_lookup_value(
        interfaces,
        SSD_DRIVE_INTERFACE,
        G_VARIANT_TYPE_VARDICT
    );
    if (!drive) {
        g_variant_unref(interfaces);
        return;
    }

    if (g_variant_lookup(drive, "MediaAvailable", "b", &media_available)) {
        *unit_ready = media_available != FALSE;
    }
    if (!snapshot->model_known
        && copy_variant_string(drive, "Model", snapshot->model, sizeof(snapshot->model))
        && text_is_present(snapshot->model)) {
        snapshot->model_known = true;
    }
    if (!snapshot->serial_known
        && copy_variant_string(drive, "Serial", snapshot->serial, sizeof(snapshot->serial))
        && text_is_present(snapshot->serial)) {
        snapshot->serial_known = true;
    }
    g_variant_unref(drive);
    g_variant_unref(interfaces);
}

static bool parse_managed_objects(
    GVariant *objects,
    Lardon3DSsdProviderSnapshot *snapshot,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    GVariantIter iterator;
    const char *object_path;
    GVariant *interfaces;
    size_t object_count = 0;

    if (!objects || !snapshot
        || !g_variant_is_of_type(objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"))) {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "invalid UDisks object map");
        return false;
    }

    g_variant_iter_init(&iterator, objects);
    while (g_variant_iter_next(
        &iterator,
        "{&o@a{sa{sv}}}",
        &object_path,
        &interfaces
    )) {
        GVariant *block;

        object_count += 1;
        if (object_count > SSD_MAX_UDISKS_OBJECTS) {
            g_variant_unref(interfaces);
            copy_text(
                reason,
                LARDON3D_SSD_REASON_CAPACITY,
                "UDisks object count exceeds the controller bound"
            );
            return false;
        }
        block = g_variant_lookup_value(
            interfaces,
            SSD_BLOCK_INTERFACE,
            G_VARIANT_TYPE_VARDICT
        );
        if (block) {
            populate_volume_from_block(object_path, interfaces, block, true, snapshot);
            populate_volume_from_block(object_path, interfaces, block, false, snapshot);
            g_variant_unref(block);
        }
        g_variant_unref(interfaces);
    }

    if (snapshot->swap.present) {
        populate_drive_properties(
            objects,
            snapshot->swap.drive_identity,
            snapshot,
            &snapshot->swap.unit_ready
        );
    }
    if (snapshot->scratch.present) {
        populate_drive_properties(
            objects,
            snapshot->scratch.drive_identity,
            snapshot,
            &snapshot->scratch.unit_ready
        );
    }
    reason[0] = '\0';
    return true;
}

#ifdef LARDON3D_SSD_CONTROLLER_TESTING
bool lardon3d_ssd_parse_managed_objects_for_test(
    GVariant *objects,
    Lardon3DSsdProviderSnapshot *snapshot,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    if (!snapshot || !reason) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return parse_managed_objects(objects, snapshot, reason);
}
#endif

static bool production_refresh(
    void *context,
    Lardon3DSsdProviderSnapshot *snapshot,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    SsdProductionProvider *provider = context;
    GError *error = NULL;
    GVariant *reply;
    GVariant *objects;
    bool parsed;

    if (!provider || !snapshot) {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "invalid production provider");
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));

    /* GetManagedObjects and every mutating call have the same finite timeout.
     * GDBus owns and bounds the complete D-Bus message; parsing additionally
     * caps accepted object count before retaining any repository state. */
    reply = g_dbus_connection_call_sync(
        provider->connection,
        SSD_UDISKS_SERVICE,
        SSD_UDISKS_ROOT,
        SSD_OBJECT_MANAGER_INTERFACE,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        SSD_DBUS_TIMEOUT_MS,
        NULL,
        &error
    );
    if (!reply) {
        format_gerror(reason, error);
        g_clear_error(&error);
        return false;
    }
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    g_variant_unref(reply);
    parsed = parse_managed_objects(objects, snapshot, reason);
    g_variant_unref(objects);
    if (!parsed) {
        return false;
    }

    populate_connection_speed_from_sysfs(snapshot);
    populate_swap_usage(snapshot);
    /* CONTRACT: Block.Size is partition extent, not usable filesystem space.
     * A synchronous UI refresh must not issue an unbounded path-based statvfs;
     * production therefore leaves scratch total/free explicitly UNKNOWN until
     * a future bounded telemetry source exists. */
    populate_host_telemetry(provider, snapshot);
    reason[0] = '\0';
    return true;
}

static GVariant *empty_options_tuple(void) {
    GVariant *dictionary = g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
    return g_variant_new("(@a{sv})", dictionary);
}

static bool production_call_no_result(
    SsdProductionProvider *provider,
    const char *object_path,
    const char *interface_name,
    const char *method,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    GError *error = NULL;
    GVariant *reply;

    if (!provider || !object_path || !g_variant_is_object_path(object_path)) {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "invalid UDisks object path");
        return false;
    }
    reply = g_dbus_connection_call_sync(
        provider->connection,
        SSD_UDISKS_SERVICE,
        object_path,
        interface_name,
        method,
        empty_options_tuple(),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        SSD_DBUS_TIMEOUT_MS,
        NULL,
        &error
    );
    if (!reply) {
        format_gerror(reason, error);
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    reason[0] = '\0';
    return true;
}

static bool production_start_swap(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    return production_call_no_result(
        context,
        object_path,
        SSD_SWAP_INTERFACE,
        "Start",
        reason
    );
}

static bool production_stop_swap(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    return production_call_no_result(
        context,
        object_path,
        SSD_SWAP_INTERFACE,
        "Stop",
        reason
    );
}

static bool production_mount_scratch(
    void *context,
    const char *object_path,
    char mount_path[LARDON3D_SSD_PATH_CAPACITY],
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    SsdProductionProvider *provider = context;
    GError *error = NULL;
    GVariant *reply;
    const char *returned_path = NULL;

    if (!provider || !object_path || !g_variant_is_object_path(object_path)) {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "invalid UDisks object path");
        return false;
    }
    reply = g_dbus_connection_call_sync(
        provider->connection,
        SSD_UDISKS_SERVICE,
        object_path,
        SSD_FILESYSTEM_INTERFACE,
        "Mount",
        empty_options_tuple(),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        SSD_DBUS_TIMEOUT_MS,
        NULL,
        &error
    );
    if (!reply) {
        format_gerror(reason, error);
        g_clear_error(&error);
        return false;
    }
    g_variant_get(reply, "(&s)", &returned_path);
    if (!returned_path || strlen(returned_path) >= LARDON3D_SSD_PATH_CAPACITY) {
        g_variant_unref(reply);
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY, "UDisks mount path is overlong");
        return false;
    }
    copy_text(mount_path, LARDON3D_SSD_PATH_CAPACITY, returned_path);
    g_variant_unref(reply);
    reason[0] = '\0';
    return true;
}

static bool production_unmount_scratch(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    return production_call_no_result(
        context,
        object_path,
        SSD_FILESYSTEM_INTERFACE,
        "Unmount",
        reason
    );
}

static void production_destroy(void *context) {
    SsdProductionProvider *provider = context;

    if (!provider) {
        return;
    }
    if (provider->connection) {
        g_object_unref(provider->connection);
    }
    free(provider);
}

static const Lardon3DSsdProviderOps SSD_PRODUCTION_PROVIDER_OPS = {
    .monotonic_now_ns = production_monotonic_now_ns,
    .refresh = production_refresh,
    .start_swap = production_start_swap,
    .stop_swap = production_stop_swap,
    .mount_scratch = production_mount_scratch,
    .unmount_scratch = production_unmount_scratch,
    .destroy = production_destroy,
};

bool lardon3d_ssd_production_provider_create(Lardon3DSsdProvider *provider) {
    SsdProductionProvider *context;
    GError *error = NULL;

    if (!provider) {
        return false;
    }
    *provider = (Lardon3DSsdProvider){0};
    context = calloc(1, sizeof(*context));
    if (!context) {
        return false;
    }
    context->connection = connect_system_bus_bounded(&error);
    if (!context->connection) {
        g_clear_error(&error);
        free(context);
        return false;
    }
    provider->ops = &SSD_PRODUCTION_PROVIDER_OPS;
    provider->context = context;
    return true;
}
