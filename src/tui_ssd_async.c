#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/tui_ssd_async.h>

#include "tui_ssd_async_internal.h"

struct Lardon3DTuiSsdAsync {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    bool thread_started;
    bool running;
    bool stopping;
    Lardon3DTuiSsdAction action;
    bool result_known;
    Lardon3DSsdControlResult result;
    uint64_t generation;
    bool refresh_attempt_known;
    uint64_t refresh_attempt_ns;
    bool controller_snapshot_known;
    bool controller_snapshot_actionable;
    Lardon3DSsdSnapshot controller_snapshot;
    char reason[LARDON3D_SSD_REASON_CAPACITY];
    Lardon3DTuiSsdAsyncProvider provider;
    Lardon3DResourceGovernor *governor;
    Lardon3DSsdController *controller_identity;
    bool external_storage_registered;
};

enum {
    SSD_TELEMETRY_INTERVAL_NS = 1000000000ULL,
};

static bool
production_now(void *context, uint64_t *now_ns)
{
    (void)context;
    struct timespec now;
    if (!now_ns || clock_gettime(CLOCK_MONOTONIC, &now) != 0
        || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L
        || (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    uint64_t seconds = (uint64_t)now.tv_sec * UINT64_C(1000000000);
    if (seconds > UINT64_MAX - (uint64_t)now.tv_nsec) {
        return false;
    }
    *now_ns = seconds + (uint64_t)now.tv_nsec;
    return true;
}

static bool
production_snapshot(void *context, Lardon3DSsdSnapshot *snapshot)
{
    return lardon3d_ssd_controller_get_snapshot(context, snapshot);
}

static Lardon3DSsdControlResult
production_enable(void *context)
{
    return lardon3d_ssd_controller_enable(context);
}

static Lardon3DSsdControlResult
production_disable(void *context)
{
    return lardon3d_ssd_controller_disable(context);
}

static bool
production_cancel(void *context)
{
    return lardon3d_ssd_controller_cancel_drain(context);
}

static const Lardon3DTuiSsdAsyncProviderOps production_ops = {
    .monotonic_now_ns = production_now,
    .snapshot = production_snapshot,
    .enable = production_enable,
    .disable = production_disable,
    .cancel_drain = production_cancel,
};

static bool
valid_controller_snapshot(const Lardon3DSsdSnapshot *snapshot)
{
    Lardon3DResourceExternalStorage storage;
    return lardon3d_resource_external_storage_from_ssd_snapshot(
        snapshot, &storage);
}

static Lardon3DSsdSnapshot
invalid_snapshot_error(void)
{
    Lardon3DSsdSnapshot snapshot = {.state = LARDON3D_SSD_ERROR};
    snapshot.scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES;
    (void)snprintf(snapshot.model, sizeof(snapshot.model), "UNKNOWN");
    (void)snprintf(snapshot.serial, sizeof(snapshot.serial), "UNKNOWN");
    (void)snprintf(snapshot.drive_identity,
        sizeof(snapshot.drive_identity), "UNKNOWN");
    (void)snprintf(snapshot.swap_uuid, sizeof(snapshot.swap_uuid), "UNKNOWN");
    (void)snprintf(snapshot.scratch_uuid,
        sizeof(snapshot.scratch_uuid), "UNKNOWN");
    (void)snprintf(snapshot.swap_device,
        sizeof(snapshot.swap_device), "UNKNOWN");
    (void)snprintf(snapshot.scratch_device,
        sizeof(snapshot.scratch_device), "UNKNOWN");
    (void)snprintf(snapshot.scratch_mount_path,
        sizeof(snapshot.scratch_mount_path), "UNKNOWN");
    (void)snprintf(snapshot.reason, sizeof(snapshot.reason),
        "Malformed SSD telemetry; control disabled");
    return snapshot;
}

static bool
publish_external_error(
    Lardon3DTuiSsdAsync *operation,
    uint64_t source_generation,
    const char *reason
)
{
    if (!operation->external_storage_registered) {
        return true;
    }
    Lardon3DResourceExternalStorage storage;
    if (!lardon3d_resource_governor_get_external_storage(
            operation->governor, &storage)) {
        return false;
    }
    if (source_generation > storage.generation) {
        storage.generation = source_generation;
    }
    storage.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR;
    storage.new_scratch_allocations_allowed = false;
    (void)snprintf(storage.reason, sizeof(storage.reason), "%s",
        reason ? reason : "Malformed SSD telemetry; scratch allocation blocked");
    return lardon3d_resource_governor_update_external_storage(
        operation->governor, operation->controller_identity, &storage);
}

static bool
publish_external_snapshot(
    Lardon3DTuiSsdAsync *operation,
    const Lardon3DSsdSnapshot *snapshot
)
{
    if (!operation->external_storage_registered) {
        return true;
    }
    Lardon3DResourceExternalStorage storage;
    if (!lardon3d_resource_external_storage_from_ssd_snapshot(
            snapshot, &storage)) {
        return publish_external_error(operation,
            snapshot ? snapshot->generation : 0,
            "Malformed SSD telemetry; scratch allocation blocked");
    }
    if (lardon3d_resource_governor_update_external_storage(
            operation->governor, operation->controller_identity, &storage)) {
        return true;
    }
    Lardon3DResourceExternalStorage current;
    if (lardon3d_resource_governor_get_external_storage(
            operation->governor, &current)
        && storage.generation < current.generation) {
        /* Another serialized controller operation already published stronger
         * evidence; a stale worker copy must not revoke or replace it. */
        return true;
    }
    return publish_external_error(operation, storage.generation,
        "Inconsistent SSD generation; scratch allocation blocked");
}

const char *
lardon3d_tui_ssd_action_name(Lardon3DTuiSsdAction action)
{
    switch (action) {
    case LARDON3D_TUI_SSD_ACTION_NONE:
        return "NONE";
    case LARDON3D_TUI_SSD_ACTION_OBSERVE:
        return "OBSERVE";
    case LARDON3D_TUI_SSD_ACTION_ENABLE:
        return "ENABLE";
    case LARDON3D_TUI_SSD_ACTION_DRAIN:
        return "DRAIN";
    case LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN:
        return "CANCEL_DRAIN";
    }
    return "UNKNOWN";
}

static bool
valid_control_action(Lardon3DTuiSsdAction action)
{
    return action >= LARDON3D_TUI_SSD_ACTION_ENABLE
        && action <= LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN;
}

static bool
snapshot_authorizes(
    const Lardon3DSsdSnapshot *snapshot,
    Lardon3DTuiSsdAction action
)
{
    return snapshot
        && ((action == LARDON3D_TUI_SSD_ACTION_ENABLE
                && snapshot->can_enable)
            || (action == LARDON3D_TUI_SSD_ACTION_DRAIN
                && snapshot->can_disable)
            || (action == LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN
                && snapshot->can_cancel_drain));
}

static void
set_result_reason(Lardon3DTuiSsdAsync *operation)
{
    const char *result = operation->result == LARDON3D_SSD_CONTROL_OK
        ? "OK"
        : (operation->result == LARDON3D_SSD_CONTROL_PENDING
            ? "PENDING" : "ERROR");
    (void)snprintf(operation->reason, sizeof(operation->reason), "%s: %s",
        lardon3d_tui_ssd_action_name(operation->action), result);
}

static void *
run_operation(void *userdata)
{
    Lardon3DTuiSsdAsync *operation = userdata;
    Lardon3DTuiSsdAction action;
    Lardon3DTuiSsdAsyncProvider provider;
    (void)pthread_mutex_lock(&operation->mutex);
    action = operation->action;
    provider = operation->provider;
    (void)pthread_mutex_unlock(&operation->mutex);

    Lardon3DSsdControlResult result = LARDON3D_SSD_CONTROL_OK;
    if (action == LARDON3D_TUI_SSD_ACTION_ENABLE) {
        result = provider.ops->enable(provider.context);
    } else if (action == LARDON3D_TUI_SSD_ACTION_DRAIN) {
        result = provider.ops->disable(provider.context);
    } else if (action == LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN) {
        result = provider.ops->cancel_drain(provider.context)
            ? LARDON3D_SSD_CONTROL_OK
            : LARDON3D_SSD_CONTROL_ERROR;
    }
    Lardon3DSsdSnapshot controller_snapshot = {0};
    bool snapshot_result = provider.ops->snapshot(
        provider.context, &controller_snapshot);
    bool snapshot_valid = valid_controller_snapshot(&controller_snapshot);
    bool snapshot_known = snapshot_valid && (snapshot_result
        || controller_snapshot.state == LARDON3D_SSD_ERROR);
    bool snapshot_actionable = snapshot_known;
    if (!snapshot_valid
        || (!snapshot_result
            && controller_snapshot.state != LARDON3D_SSD_ERROR)) {
        controller_snapshot = invalid_snapshot_error();
        snapshot_known = true;
        snapshot_actionable = false;
    }
    /* Provider/controller calls have returned and hold no lock here. Publish
     * before advertising worker completion so the next F10/render observes a
     * registry state at least as recent as this outcome. */
    if (!publish_external_snapshot(operation, &controller_snapshot)) {
        controller_snapshot = invalid_snapshot_error();
        snapshot_known = true;
        snapshot_actionable = false;
        result = LARDON3D_SSD_CONTROL_ERROR;
    }

    (void)pthread_mutex_lock(&operation->mutex);
    operation->result = result;
    operation->result_known = action != LARDON3D_TUI_SSD_ACTION_OBSERVE;
    if (snapshot_known) {
        operation->controller_snapshot = controller_snapshot;
        operation->controller_snapshot_known = true;
        operation->controller_snapshot_actionable = snapshot_actionable;
    }
    operation->running = false;
    operation->generation = operation->generation == UINT64_MAX
        ? UINT64_MAX : operation->generation + 1;
    if (action == LARDON3D_TUI_SSD_ACTION_OBSERVE) {
        if (!snapshot_known) {
            (void)snprintf(operation->reason, sizeof(operation->reason),
                "SSD telemetry refresh failed");
        } else {
            operation->reason[0] = '\0';
        }
    } else {
        set_result_reason(operation);
    }
    (void)pthread_cond_broadcast(&operation->condition);
    (void)pthread_mutex_unlock(&operation->mutex);
    return NULL;
}

static bool
join_if_complete(Lardon3DTuiSsdAsync *operation)
{
    pthread_t thread;
    bool join = false;
    (void)pthread_mutex_lock(&operation->mutex);
    if (operation->thread_started && !operation->running) {
        thread = operation->thread;
        operation->thread_started = false;
        join = true;
    }
    (void)pthread_mutex_unlock(&operation->mutex);
    return !join || pthread_join(thread, NULL) == 0;
}

static Lardon3DTuiSsdAsync *
create_with_provider_and_binding(
    Lardon3DTuiSsdAsyncProvider provider,
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller_identity
)
{
    if (!provider.ops || !provider.ops->monotonic_now_ns
        || !provider.ops->snapshot || !provider.ops->enable
        || !provider.ops->disable || !provider.ops->cancel_drain
        || ((governor == NULL) != (controller_identity == NULL))) {
        return NULL;
    }
    Lardon3DTuiSsdAsync *operation = calloc(1, sizeof(*operation));
    if (!operation || pthread_mutex_init(&operation->mutex, NULL) != 0) {
        free(operation);
        return NULL;
    }
    pthread_condattr_t attributes;
    bool attributes_ready = pthread_condattr_init(&attributes) == 0;
    bool clock_ready = attributes_ready
        && pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0;
    if (!attributes_ready || !clock_ready
        || pthread_cond_init(&operation->condition, &attributes) != 0) {
        if (attributes_ready) {
            (void)pthread_condattr_destroy(&attributes);
        }
        (void)pthread_mutex_destroy(&operation->mutex);
        free(operation);
        return NULL;
    }
    (void)pthread_condattr_destroy(&attributes);
    operation->provider = provider;
    operation->governor = governor;
    operation->controller_identity = controller_identity;
    if (governor) {
        Lardon3DResourceExternalStorage storage = {
            .status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR,
        };
        (void)snprintf(storage.stable_identity,
            sizeof(storage.stable_identity), "UNKNOWN");
        (void)snprintf(storage.reason, sizeof(storage.reason),
            "SSD telemetry has not yet been validated; scratch allocation blocked");
        if (!lardon3d_resource_governor_register_external_storage(
                governor, controller_identity, &storage)) {
            (void)pthread_cond_destroy(&operation->condition);
            (void)pthread_mutex_destroy(&operation->mutex);
            free(operation);
            return NULL;
        }
        operation->external_storage_registered = true;
    }
    (void)snprintf(operation->reason, sizeof(operation->reason),
        "No SSD control operation requested");
    return operation;
}

Lardon3DTuiSsdAsync *
lardon3d_tui_ssd_async_create_with_provider(
    Lardon3DTuiSsdAsyncProvider provider
)
{
    return create_with_provider_and_binding(provider, NULL, NULL);
}

Lardon3DTuiSsdAsync *
lardon3d_tui_ssd_async_internal_create_with_provider_and_governor(
    Lardon3DTuiSsdAsyncProvider provider,
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller_identity
)
{
    return create_with_provider_and_binding(
        provider, governor, controller_identity);
}

Lardon3DTuiSsdAsync *
lardon3d_tui_ssd_async_create(Lardon3DSsdController *controller)
{
    if (!controller) {
        return NULL;
    }
    return lardon3d_tui_ssd_async_create_with_provider(
        (Lardon3DTuiSsdAsyncProvider) {
            .ops = &production_ops,
            .context = controller,
        });
}

Lardon3DTuiSsdAsync *
lardon3d_tui_ssd_async_create_with_governor(
    Lardon3DSsdController *controller,
    Lardon3DResourceGovernor *governor
)
{
    if (!controller || !governor) {
        return NULL;
    }
    return create_with_provider_and_binding(
        (Lardon3DTuiSsdAsyncProvider) {
            .ops = &production_ops,
            .context = controller,
        },
        governor,
        controller);
}

bool
lardon3d_tui_ssd_async_destroy_checked(Lardon3DTuiSsdAsync **owned_operation)
{
    if (!owned_operation || !*owned_operation) {
        return false;
    }
    Lardon3DTuiSsdAsync *operation = *owned_operation;
    pthread_t thread;
    bool join = false;
    (void)pthread_mutex_lock(&operation->mutex);
    operation->stopping = true;
    if (operation->thread_started) {
        thread = operation->thread;
        operation->thread_started = false;
        join = true;
    }
    (void)pthread_mutex_unlock(&operation->mutex);
    if (join && pthread_join(thread, NULL) != 0) {
        return false;
    }
    if (operation->external_storage_registered) {
        /* INVARIANT: a failed unregister leaves every owner and the exact
         * controller token alive. The caller retains this stopped object and
         * can retry after releasing its Governor-managed leases. */
        if (!lardon3d_resource_governor_unregister_external_storage(
                operation->governor, operation->controller_identity)) {
            return false;
        }
        operation->external_storage_registered = false;
    }
    if (operation->provider.ops->destroy) {
        operation->provider.ops->destroy(operation->provider.context);
    }
    (void)pthread_cond_destroy(&operation->condition);
    (void)pthread_mutex_destroy(&operation->mutex);
    free(operation);
    *owned_operation = NULL;
    return true;
}

void
lardon3d_tui_ssd_async_destroy(Lardon3DTuiSsdAsync *operation)
{
    if (!operation) {
        return;
    }
    Lardon3DTuiSsdAsync *owned = operation;
    (void)lardon3d_tui_ssd_async_destroy_checked(&owned);
}

bool
lardon3d_tui_ssd_async_request(
    Lardon3DTuiSsdAsync *operation,
    Lardon3DTuiSsdAction action
)
{
    if (!operation || !valid_control_action(action)
        || !join_if_complete(operation)) {
        return false;
    }
    uint64_t now_ns;
    bool now_known = operation->provider.ops->monotonic_now_ns(
        operation->provider.context, &now_ns);
    (void)pthread_mutex_lock(&operation->mutex);
    if (operation->stopping || operation->running
        || operation->thread_started
        || !operation->controller_snapshot_known
        || !operation->controller_snapshot_actionable
        || !snapshot_authorizes(&operation->controller_snapshot, action)) {
        (void)pthread_mutex_unlock(&operation->mutex);
        return false;
    }
    operation->action = action;
    operation->running = true;
    operation->result_known = false;
    operation->reason[0] = '\0';
    if (now_known) {
        operation->refresh_attempt_known = true;
        operation->refresh_attempt_ns = now_ns;
    }
    int created = pthread_create(
        &operation->thread, NULL, run_operation, operation);
    if (created != 0) {
        operation->running = false;
        operation->action = LARDON3D_TUI_SSD_ACTION_NONE;
        (void)snprintf(operation->reason, sizeof(operation->reason),
            "Cannot start bounded SSD operation thread");
        (void)pthread_mutex_unlock(&operation->mutex);
        return false;
    }
    operation->thread_started = true;
    (void)pthread_mutex_unlock(&operation->mutex);
    return true;
}

bool
lardon3d_tui_ssd_async_refresh(Lardon3DTuiSsdAsync *operation)
{
    if (!operation || !join_if_complete(operation)) {
        return false;
    }
    (void)pthread_mutex_lock(&operation->mutex);
    if (operation->stopping) {
        (void)pthread_mutex_unlock(&operation->mutex);
        return false;
    }
    if (operation->running || operation->thread_started) {
        (void)pthread_mutex_unlock(&operation->mutex);
        return true;
    }
    (void)pthread_mutex_unlock(&operation->mutex);

    uint64_t now_ns;
    if (!operation->provider.ops->monotonic_now_ns(
            operation->provider.context, &now_ns)) {
        return false;
    }
    (void)pthread_mutex_lock(&operation->mutex);
    if (operation->stopping) {
        (void)pthread_mutex_unlock(&operation->mutex);
        return false;
    }
    if (operation->running || operation->thread_started) {
        (void)pthread_mutex_unlock(&operation->mutex);
        return true;
    }
    if (operation->refresh_attempt_known
        && now_ns >= operation->refresh_attempt_ns
        && now_ns - operation->refresh_attempt_ns
            < SSD_TELEMETRY_INTERVAL_NS) {
        (void)pthread_mutex_unlock(&operation->mutex);
        return true;
    }
    operation->refresh_attempt_known = true;
    operation->refresh_attempt_ns = now_ns;
    operation->action = LARDON3D_TUI_SSD_ACTION_OBSERVE;
    operation->running = true;
    operation->result_known = false;
    operation->reason[0] = '\0';
    int created = pthread_create(
        &operation->thread, NULL, run_operation, operation);
    if (created != 0) {
        operation->running = false;
        operation->action = LARDON3D_TUI_SSD_ACTION_NONE;
        (void)snprintf(operation->reason, sizeof(operation->reason),
            "Cannot start bounded SSD telemetry thread");
        (void)pthread_mutex_unlock(&operation->mutex);
        return false;
    }
    operation->thread_started = true;
    (void)pthread_mutex_unlock(&operation->mutex);
    return true;
}

bool
lardon3d_tui_ssd_async_poll(
    Lardon3DTuiSsdAsync *operation,
    Lardon3DTuiSsdAsyncSnapshot *snapshot
)
{
    if (snapshot) {
        *snapshot = (Lardon3DTuiSsdAsyncSnapshot) {0};
    }
    if (!operation || !snapshot || !join_if_complete(operation)) {
        return false;
    }
    (void)pthread_mutex_lock(&operation->mutex);
    *snapshot = (Lardon3DTuiSsdAsyncSnapshot) {
        .running = operation->running,
        .action = operation->action,
        .result_known = operation->result_known,
        .result = operation->result,
        .generation = operation->generation,
        .controller_snapshot_known = operation->controller_snapshot_known,
        .controller_snapshot_actionable =
            operation->controller_snapshot_actionable,
        .controller_snapshot = operation->controller_snapshot,
    };
    (void)snprintf(snapshot->reason, sizeof(snapshot->reason), "%s",
        operation->reason);
    (void)pthread_mutex_unlock(&operation->mutex);
    return true;
}

static bool
deadline_after(uint64_t timeout_ns, struct timespec *deadline)
{
    if (!deadline || clock_gettime(CLOCK_MONOTONIC, deadline) != 0
        || deadline->tv_sec < 0 || deadline->tv_nsec < 0
        || deadline->tv_nsec >= 1000000000L) {
        return false;
    }
    uint64_t seconds = timeout_ns / UINT64_C(1000000000);
    uint64_t nanoseconds = timeout_ns % UINT64_C(1000000000);
    if (seconds > (uint64_t)INT64_MAX
        || deadline->tv_sec > (time_t)(INT64_MAX - (int64_t)seconds)) {
        return false;
    }
    deadline->tv_sec += (time_t)seconds;
    deadline->tv_nsec += (long)nanoseconds;
    if (deadline->tv_nsec >= 1000000000L) {
        if (deadline->tv_sec == (time_t)INT64_MAX) {
            return false;
        }
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

bool
lardon3d_tui_ssd_async_wait_idle(
    Lardon3DTuiSsdAsync *operation,
    uint64_t timeout_ns
)
{
    if (!operation) {
        return false;
    }
    struct timespec deadline;
    if (!deadline_after(timeout_ns, &deadline)) {
        return false;
    }
    (void)pthread_mutex_lock(&operation->mutex);
    while (operation->running) {
        int result = pthread_cond_timedwait(
            &operation->condition, &operation->mutex, &deadline);
        if (result == ETIMEDOUT) {
            (void)pthread_mutex_unlock(&operation->mutex);
            return false;
        }
        if (result != 0) {
            (void)pthread_mutex_unlock(&operation->mutex);
            return false;
        }
    }
    (void)pthread_mutex_unlock(&operation->mutex);
    return join_if_complete(operation);
}

bool
lardon3d_tui_ssd_action_for_snapshot(
    const Lardon3DSsdSnapshot *snapshot,
    Lardon3DTuiSsdAction *action
)
{
    if (action) {
        *action = LARDON3D_TUI_SSD_ACTION_NONE;
    }
    if (!snapshot || !action || !valid_controller_snapshot(snapshot)) {
        return false;
    }
    unsigned int action_count = (unsigned int)snapshot->can_enable
        + (unsigned int)snapshot->can_disable
        + (unsigned int)snapshot->can_cancel_drain;
    if (action_count != 1) {
        return false;
    }
    if (snapshot->can_enable) {
        *action = LARDON3D_TUI_SSD_ACTION_ENABLE;
        return true;
    }
    if (snapshot->can_disable) {
        *action = LARDON3D_TUI_SSD_ACTION_DRAIN;
        return true;
    }
    if (snapshot->can_cancel_drain) {
        *action = LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN;
        return true;
    }
    return false;
}
