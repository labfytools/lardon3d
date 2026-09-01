#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/tui_ssd_async.h>

#include "../src/tui_ssd_async_internal.h"

#define GIB (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "TUI SSD async failure line %d: %s\n",   \
                __LINE__, #condition);                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint64_t now_ns;
    Lardon3DSsdSnapshot snapshot;
    bool snapshot_result;
    bool block_snapshot;
    bool release_snapshot;
    bool snapshot_entered;
    size_t snapshot_calls;
    size_t enable_calls;
    size_t disable_calls;
    size_t cancel_calls;
    bool destroyed;
} FakeProvider;

static bool
fake_now(void *context, uint64_t *now_ns)
{
    FakeProvider *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    *now_ns = fake->now_ns;
    (void)pthread_mutex_unlock(&fake->mutex);
    return true;
}

static bool
fake_snapshot(void *context, Lardon3DSsdSnapshot *snapshot)
{
    FakeProvider *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    ++fake->snapshot_calls;
    fake->snapshot_entered = true;
    (void)pthread_cond_broadcast(&fake->condition);
    while (fake->block_snapshot && !fake->release_snapshot) {
        (void)pthread_cond_wait(&fake->condition, &fake->mutex);
    }
    *snapshot = fake->snapshot;
    bool result = fake->snapshot_result;
    (void)pthread_mutex_unlock(&fake->mutex);
    return result;
}

static Lardon3DSsdControlResult
fake_enable(void *context)
{
    FakeProvider *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    ++fake->enable_calls;
    fake->snapshot.state = LARDON3D_SSD_ENABLED;
    fake->snapshot.pairing_valid = true;
    fake->snapshot.swap_active = true;
    fake->snapshot.scratch_mounted = true;
    fake->snapshot.can_enable = false;
    fake->snapshot.can_disable = true;
    fake->snapshot.can_cancel_drain = false;
    fake->snapshot.scratch_allocations_allowed = true;
    fake->snapshot.scratch_lease_capacity =
        LARDON3D_SSD_MAX_SCRATCH_LEASES;
    (void)snprintf(fake->snapshot.scratch_mount_path,
        sizeof(fake->snapshot.scratch_mount_path), "%s",
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    if (fake->snapshot.generation != UINT64_MAX) {
        ++fake->snapshot.generation;
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return LARDON3D_SSD_CONTROL_OK;
}

static Lardon3DSsdControlResult
fake_disable(void *context)
{
    FakeProvider *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    ++fake->disable_calls;
    fake->snapshot.state = LARDON3D_SSD_SAFE_TO_UNPLUG;
    fake->snapshot.swap_active = false;
    fake->snapshot.scratch_mounted = false;
    fake->snapshot.drain_requested = false;
    fake->snapshot.can_enable = true;
    fake->snapshot.can_disable = false;
    fake->snapshot.can_cancel_drain = false;
    fake->snapshot.scratch_allocations_allowed = false;
    (void)snprintf(fake->snapshot.scratch_mount_path,
        sizeof(fake->snapshot.scratch_mount_path), "UNKNOWN");
    if (fake->snapshot.generation != UINT64_MAX) {
        ++fake->snapshot.generation;
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return LARDON3D_SSD_CONTROL_OK;
}

static bool
fake_cancel(void *context)
{
    FakeProvider *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    ++fake->cancel_calls;
    fake->snapshot.state = LARDON3D_SSD_ENABLED;
    fake->snapshot.drain_requested = false;
    fake->snapshot.can_enable = false;
    fake->snapshot.can_disable = true;
    fake->snapshot.can_cancel_drain = false;
    fake->snapshot.scratch_allocations_allowed = true;
    if (fake->snapshot.generation != UINT64_MAX) {
        ++fake->snapshot.generation;
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return true;
}

static void
fake_destroy(void *context)
{
    FakeProvider *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    fake->destroyed = true;
    (void)pthread_mutex_unlock(&fake->mutex);
}

static const Lardon3DTuiSsdAsyncProviderOps fake_ops = {
    .monotonic_now_ns = fake_now,
    .snapshot = fake_snapshot,
    .enable = fake_enable,
    .disable = fake_disable,
    .cancel_drain = fake_cancel,
    .destroy = fake_destroy,
};

static void
set_exact_pair_identity(Lardon3DSsdSnapshot *snapshot)
{
    snapshot->device_detected = true;
    snapshot->swap_detected = true;
    snapshot->scratch_detected = true;
    snapshot->swap_partition_size_known = true;
    snapshot->scratch_partition_size_known = true;
    snapshot->swap_partition_size_bytes = 8 * GIB;
    snapshot->scratch_partition_size_bytes = 400 * GIB;
    (void)snprintf(snapshot->drive_identity,
        sizeof(snapshot->drive_identity), "async-fake-drive");
    (void)snprintf(snapshot->swap_uuid,
        sizeof(snapshot->swap_uuid), "async-fake-swap");
    (void)snprintf(snapshot->scratch_uuid,
        sizeof(snapshot->scratch_uuid), "async-fake-scratch");
    (void)snprintf(snapshot->swap_device,
        sizeof(snapshot->swap_device), "/dev/fake-swap");
    (void)snprintf(snapshot->scratch_device,
        sizeof(snapshot->scratch_device), "/dev/fake-scratch");
}

static bool
wait_for_snapshot_entry(FakeProvider *fake)
{
    (void)pthread_mutex_lock(&fake->mutex);
    while (!fake->snapshot_entered) {
        (void)pthread_cond_wait(&fake->condition, &fake->mutex);
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return true;
}

static size_t
snapshot_calls(FakeProvider *fake)
{
    (void)pthread_mutex_lock(&fake->mutex);
    size_t calls = fake->snapshot_calls;
    (void)pthread_mutex_unlock(&fake->mutex);
    return calls;
}

static bool
test_action_matrix(void)
{
    static const struct {
        Lardon3DSsdState state;
        bool pairing_valid;
        bool can_enable;
        bool can_disable;
        bool can_cancel_drain;
        bool drain_requested;
        bool expected;
        Lardon3DTuiSsdAction action;
    } cases[] = {
        {LARDON3D_SSD_ABSENT, false, false, false, false, false,
            false, LARDON3D_TUI_SSD_ACTION_NONE},
        /* DETECTED can also mean an incomplete pair; only capability grants
         * enable authority. */
        {LARDON3D_SSD_DETECTED, false, false, false, false, false,
            false, LARDON3D_TUI_SSD_ACTION_NONE},
        {LARDON3D_SSD_DETECTED, true, true, false, false, false,
            true, LARDON3D_TUI_SSD_ACTION_ENABLE},
        {LARDON3D_SSD_ENABLING, true, false, false, false, false,
            false, LARDON3D_TUI_SSD_ACTION_NONE},
        {LARDON3D_SSD_ENABLED, true, false, true, false, false,
            true, LARDON3D_TUI_SSD_ACTION_DRAIN},
        {LARDON3D_SSD_IN_USE, true, false, true, false, false,
            true, LARDON3D_TUI_SSD_ACTION_DRAIN},
        {LARDON3D_SSD_DRAINING, true, false, false, true, true,
            true,
            LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN},
        {LARDON3D_SSD_SAFE_TO_UNPLUG, true, true, false, false, false,
            true,
            LARDON3D_TUI_SSD_ACTION_ENABLE},
        /* A clean-looking ERROR has no inferred action; an exact original
         * sticky pair may explicitly grant drain-only recovery. */
        {LARDON3D_SSD_ERROR, true, false, false, false, false,
            false, LARDON3D_TUI_SSD_ACTION_NONE},
        {LARDON3D_SSD_ERROR, true, false, true, false, false,
            true, LARDON3D_TUI_SSD_ACTION_DRAIN},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        Lardon3DSsdSnapshot snapshot = {
            .state = cases[index].state,
            .pairing_valid = cases[index].pairing_valid,
            .can_enable = cases[index].can_enable,
            .can_disable = cases[index].can_disable,
            .can_cancel_drain = cases[index].can_cancel_drain,
            .drain_requested = cases[index].drain_requested,
            .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
            .scratch_lease_count = cases[index].state == LARDON3D_SSD_IN_USE
                ? 1 : 0,
        };
        if (snapshot.pairing_valid) {
            set_exact_pair_identity(&snapshot);
        } else if (snapshot.state == LARDON3D_SSD_DETECTED) {
            /* Production DETECTED without a complete pair still represents at
             * least one exact observed reserved partition. */
            snapshot.device_detected = true;
            snapshot.swap_detected = true;
            snapshot.swap_partition_size_known = true;
            snapshot.swap_partition_size_bytes = 8 * GIB;
            (void)snprintf(snapshot.swap_uuid,
                sizeof(snapshot.swap_uuid), "partial-swap");
        }
        if (cases[index].state == LARDON3D_SSD_ENABLED
            || cases[index].state == LARDON3D_SSD_IN_USE
            || cases[index].state == LARDON3D_SSD_DRAINING) {
            snapshot.swap_active = true;
            snapshot.scratch_mounted = true;
            (void)snprintf(snapshot.scratch_mount_path,
                sizeof(snapshot.scratch_mount_path), "%s",
                LARDON3D_SSD_SCRATCH_MOUNT_PATH);
        }
        Lardon3DTuiSsdAction action = LARDON3D_TUI_SSD_ACTION_ENABLE;
        CHECK(lardon3d_tui_ssd_action_for_snapshot(&snapshot, &action)
            == cases[index].expected);
        CHECK(action == cases[index].action);
    }
    Lardon3DSsdSnapshot clean_error = {
        .state = LARDON3D_SSD_ERROR,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
    };
    Lardon3DTuiSsdAction action;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(&clean_error, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);

    Lardon3DSsdSnapshot malformed_authority = {
        .state = LARDON3D_SSD_ABSENT,
        .pairing_valid = true,
        .can_disable = true,
    };
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(
        &malformed_authority, &action));

    Lardon3DSsdSnapshot invalid = {
        .state = LARDON3D_SSD_ENABLED,
        .pairing_valid = true,
        .swap_active = true,
        .scratch_mounted = true,
        .can_disable = true,
        .scratch_allocations_allowed = true,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
    };
    set_exact_pair_identity(&invalid);
    (void)snprintf(invalid.scratch_mount_path,
        sizeof(invalid.scratch_mount_path), "%s",
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    const Lardon3DSsdSnapshot valid_enabled = invalid;

    invalid.device_detected = false;
    invalid.swap_detected = false;
    invalid.scratch_detected = false;
    invalid.swap_partition_size_known = false;
    invalid.scratch_partition_size_known = false;
    invalid.swap_partition_size_bytes = 0;
    invalid.scratch_partition_size_bytes = 0;
    action = LARDON3D_TUI_SSD_ACTION_DRAIN;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(&invalid, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);

    invalid = valid_enabled;
    invalid.swap_partition_size_known = false;
    invalid.scratch_partition_size_known = false;
    invalid.swap_partition_size_bytes = 0;
    invalid.scratch_partition_size_bytes = 0;
    action = LARDON3D_TUI_SSD_ACTION_DRAIN;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(&invalid, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);

    invalid = valid_enabled;
    invalid.swap_partition_size_bytes = 0;
    action = LARDON3D_TUI_SSD_ACTION_DRAIN;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(&invalid, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);

    invalid = valid_enabled;
    invalid.state = LARDON3D_SSD_ABSENT;
    action = LARDON3D_TUI_SSD_ACTION_DRAIN;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(&invalid, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);

    invalid = valid_enabled;
    invalid.can_enable = true;
    action = LARDON3D_TUI_SSD_ACTION_DRAIN;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(&invalid, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);
    return true;
}

static bool
test_async_owner(void)
{
    FakeProvider fake = {
        .now_ns = UINT64_C(1000000000),
        .snapshot = {
            .state = LARDON3D_SSD_DETECTED,
            .generation = 1,
            .pairing_valid = true,
            .can_enable = true,
            .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        },
        .snapshot_result = true,
        .block_snapshot = true,
    };
    set_exact_pair_identity(&fake.snapshot);
    CHECK(pthread_mutex_init(&fake.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&fake.condition, NULL) == 0);
    Lardon3DTuiSsdAsync *operation =
        lardon3d_tui_ssd_async_create_with_provider(
            (Lardon3DTuiSsdAsyncProvider) {
                .ops = &fake_ops,
                .context = &fake,
            });
    CHECK(operation);

    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(wait_for_snapshot_entry(&fake));
    Lardon3DTuiSsdAsyncSnapshot view;
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(view.running);
    CHECK(view.action == LARDON3D_TUI_SSD_ACTION_OBSERVE);
    CHECK(!view.controller_snapshot_known);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(snapshot_calls(&fake) == 1);
    CHECK(!lardon3d_tui_ssd_async_request(
        operation, LARDON3D_TUI_SSD_ACTION_ENABLE));

    (void)pthread_mutex_lock(&fake.mutex);
    fake.release_snapshot = true;
    fake.block_snapshot = false;
    (void)pthread_cond_broadcast(&fake.condition);
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(!view.running);
    CHECK(view.controller_snapshot_known);
    CHECK(view.controller_snapshot_actionable);
    CHECK(view.controller_snapshot.state == LARDON3D_SSD_DETECTED);
    CHECK(view.generation == 1);

    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(snapshot_calls(&fake) == 1);
    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    fake.snapshot_entered = false;
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(snapshot_calls(&fake) == 2);

    CHECK(lardon3d_tui_ssd_async_request(
        operation, LARDON3D_TUI_SSD_ACTION_ENABLE));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(view.result_known && view.result == LARDON3D_SSD_CONTROL_OK);
    CHECK(view.action == LARDON3D_TUI_SSD_ACTION_ENABLE);
    CHECK(view.controller_snapshot.state == LARDON3D_SSD_ENABLED);

    CHECK(lardon3d_tui_ssd_async_request(
        operation, LARDON3D_TUI_SSD_ACTION_DRAIN));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(view.controller_snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);

    (void)pthread_mutex_lock(&fake.mutex);
    fake.snapshot.state = LARDON3D_SSD_DRAINING;
    fake.snapshot.swap_active = true;
    fake.snapshot.scratch_mounted = true;
    fake.snapshot.drain_requested = true;
    fake.snapshot.can_enable = false;
    fake.snapshot.can_disable = false;
    fake.snapshot.can_cancel_drain = true;
    fake.snapshot.scratch_allocations_allowed = false;
    (void)snprintf(fake.snapshot.scratch_mount_path,
        sizeof(fake.snapshot.scratch_mount_path), "%s",
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    ++fake.snapshot.generation;
    (void)pthread_mutex_unlock(&fake.mutex);
    /* Refresh the controller-issued authority before requesting cancellation.
     */
    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_request(
        operation, LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(view.controller_snapshot.state == LARDON3D_SSD_ENABLED);

    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    fake.snapshot_result = false;
    fake.snapshot.state = LARDON3D_SSD_ERROR;
    fake.snapshot.scratch_allocations_allowed = false;
    ++fake.snapshot.generation;
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(view.controller_snapshot_known);
    CHECK(view.controller_snapshot_actionable);
    CHECK(view.controller_snapshot.state == LARDON3D_SSD_ERROR);

    /* A malformed provider copy is visible as ERROR but can never authorize
     * F10 control or retain a stale SAFE_TO_UNPLUG observation. */
    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    fake.snapshot_result = true;
    fake.snapshot.state = LARDON3D_SSD_DETECTED;
    ++fake.snapshot.generation;
    (void)memset(fake.snapshot.model, 'X', sizeof(fake.snapshot.model));
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_tui_ssd_async_poll(operation, &view));
    CHECK(view.controller_snapshot_known);
    CHECK(!view.controller_snapshot_actionable);
    CHECK(view.controller_snapshot.state == LARDON3D_SSD_ERROR);

    CHECK(lardon3d_tui_ssd_async_destroy_checked(&operation));
    CHECK(operation == NULL);
    (void)pthread_mutex_lock(&fake.mutex);
    CHECK(fake.destroyed);
    CHECK(fake.enable_calls == 1);
    CHECK(fake.disable_calls == 1);
    CHECK(fake.cancel_calls == 1);
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(pthread_cond_destroy(&fake.condition) == 0);
    CHECK(pthread_mutex_destroy(&fake.mutex) == 0);
    return true;
}

static Lardon3DResourceGovernor *
test_governor(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 8,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_C(16) * 1024 * 1024 * 1024,
        .cpu_architecture = "synthetic",
    };
    Lardon3DResourcePolicy policy;
    return lardon3d_resource_policy_default(&profile, &policy)
        ? lardon3d_resource_governor_create(&profile, &policy) : NULL;
}

static bool
test_bound_governor_publication(void)
{
    FakeProvider fake = {
        .now_ns = UINT64_C(1000000000),
        .snapshot = {
            .state = LARDON3D_SSD_DETECTED,
            .generation = 1,
            .pairing_valid = true,
            .can_enable = true,
            .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        },
        .snapshot_result = true,
    };
    set_exact_pair_identity(&fake.snapshot);
    (void)snprintf(fake.snapshot.reason, sizeof(fake.snapshot.reason),
        "healthy fake pair detected");
    CHECK(pthread_mutex_init(&fake.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&fake.condition, NULL) == 0);
    Lardon3DResourceGovernor *governor = test_governor();
    CHECK(governor);
    unsigned char identity_storage = 0;
    Lardon3DSsdController *identity =
        (Lardon3DSsdController *)(void *)&identity_storage;
    Lardon3DTuiSsdAsync *operation =
        lardon3d_tui_ssd_async_internal_create_with_provider_and_governor(
            (Lardon3DTuiSsdAsyncProvider) {
                .ops = &fake_ops,
                .context = &fake,
            },
            governor,
            identity);
    CHECK(operation);

    Lardon3DResourceExternalStorage external;
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(!external.new_scratch_allocations_allowed);

    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED);
    CHECK(strcmp(external.stable_identity, "async-fake-drive") == 0);

    CHECK(lardon3d_tui_ssd_async_request(
        operation, LARDON3D_TUI_SSD_ACTION_ENABLE));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE);
    CHECK(external.new_scratch_allocations_allowed);

    /* Exact names without current detection and positive extents are not a
     * physical pair. The async boundary must publish ERROR and F10 must remain
     * non-actionable rather than trusting the friendly ENABLED flags. */
    Lardon3DSsdSnapshot malformed_authority;
    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    ++fake.snapshot.generation;
    fake.snapshot.device_detected = false;
    fake.snapshot.swap_detected = false;
    fake.snapshot.scratch_detected = false;
    fake.snapshot.swap_partition_size_known = false;
    fake.snapshot.scratch_partition_size_known = false;
    fake.snapshot.swap_partition_size_bytes = 0;
    fake.snapshot.scratch_partition_size_bytes = 0;
    malformed_authority = fake.snapshot;
    (void)pthread_mutex_unlock(&fake.mutex);
    Lardon3DTuiSsdAction action = LARDON3D_TUI_SSD_ACTION_DRAIN;
    CHECK(!lardon3d_tui_ssd_action_for_snapshot(
        &malformed_authority, &action));
    CHECK(action == LARDON3D_TUI_SSD_ACTION_NONE);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(!external.new_scratch_allocations_allowed);

    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    ++fake.snapshot.generation;
    set_exact_pair_identity(&fake.snapshot);
    fake.snapshot.swap_partition_size_bytes = 0;
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(!external.new_scratch_allocations_allowed);

    (void)pthread_mutex_lock(&fake.mutex);
    fake.now_ns += UINT64_C(1000000000);
    ++fake.snapshot.generation;
    set_exact_pair_identity(&fake.snapshot);
    fake.snapshot.swap_partition_size_bytes = 8 * GIB;
    (void)memset(fake.snapshot.reason, 'X', sizeof(fake.snapshot.reason));
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(!external.new_scratch_allocations_allowed);
    CHECK(strstr(external.reason, "Malformed SSD telemetry") != NULL);

    CHECK(lardon3d_tui_ssd_async_destroy_checked(&operation));
    CHECK(operation == NULL);
    external = (Lardon3DResourceExternalStorage) {.generation = UINT64_MAX};
    CHECK(!lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.generation == 0);
    lardon3d_resource_governor_destroy(governor);
    CHECK(pthread_cond_destroy(&fake.condition) == 0);
    CHECK(pthread_mutex_destroy(&fake.mutex) == 0);
    return true;
}

int
main(void)
{
    return test_action_matrix() && test_async_owner()
        && test_bound_governor_publication()
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
