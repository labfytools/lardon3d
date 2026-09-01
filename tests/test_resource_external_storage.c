#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/resource_governor.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/tui_ssd_async.h>

#include "../src/resource_governor_internal.h"
#include "../src/ssd_controller_internal.h"
#include "../src/task_queue_internal.h"

#define GIB (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "external storage failure line %d: %s\n", \
                __LINE__, #condition);                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    pthread_mutex_t mutex;
    uint64_t now_ns;
    Lardon3DSsdProviderSnapshot observation;
    bool destroyed;
} FakeStorage;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    Lardon3DTaskQueue *queue;
    bool closing;
} QueueClosingProbe;

static _Atomic(QueueClosingProbe *) active_closing_probe;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    Lardon3DResourceGovernor *governor;
    Lardon3DSsdController *controller;
    bool engaged;
    bool release;
} ExternalUpdateBarrier;

static _Atomic(ExternalUpdateBarrier *) active_update_barrier;

/* Strong test definition for the private test-only seam. The callback runs
 * after update() owns the Governor mutex, so the physical drain below cannot
 * merely win a scheduler race before the updater is actually engaged. */
void
lardon3d_resource_governor_external_update_engaged_for_test(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller
)
{
    ExternalUpdateBarrier *barrier = atomic_load_explicit(
        &active_update_barrier, memory_order_acquire);
    if (!barrier || barrier->governor != governor
        || barrier->controller != controller) {
        return;
    }
    (void)pthread_mutex_lock(&barrier->mutex);
    barrier->engaged = true;
    (void)pthread_cond_broadcast(&barrier->condition);
    while (!barrier->release) {
        (void)pthread_cond_wait(&barrier->condition, &barrier->mutex);
    }
    (void)pthread_mutex_unlock(&barrier->mutex);
}

/* Strong definition for the private Queue seam compiled into this test
 * binary. It proves teardown crossed the exact closing linearization point
 * before the callback may release its scratch capability. */
void
lardon3d_task_queue_internal_test_event(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskQueueTestEvent event
)
{
    QueueClosingProbe *probe = atomic_load_explicit(
        &active_closing_probe, memory_order_acquire);
    if (!probe || probe->queue != queue
        || event != LARDON3D_TASK_QUEUE_TEST_CLOSING) {
        return;
    }
    (void)pthread_mutex_lock(&probe->mutex);
    probe->closing = true;
    (void)pthread_cond_broadcast(&probe->condition);
    (void)pthread_mutex_unlock(&probe->mutex);
}

static void
copy_text(char *destination, size_t capacity, const char *source)
{
    (void)snprintf(destination, capacity, "%s", source ? source : "");
}

static bool
fake_now(void *context, uint64_t *now_ns)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    *now_ns = fake->now_ns;
    (void)pthread_mutex_unlock(&fake->mutex);
    return true;
}

static bool
fake_refresh(
    void *context,
    Lardon3DSsdProviderSnapshot *snapshot,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    *snapshot = fake->observation;
    (void)pthread_mutex_unlock(&fake->mutex);
    reason[0] = '\0';
    return true;
}

static bool
fake_start_swap(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    bool valid = strcmp(object_path,
        fake->observation.swap.object_path) == 0;
    if (valid) {
        fake->observation.swap.active = true;
    } else {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY,
            "wrong fake swap object");
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return valid;
}

static bool
fake_stop_swap(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    bool valid = strcmp(object_path,
        fake->observation.swap.object_path) == 0;
    if (valid) {
        fake->observation.swap.active = false;
        fake->observation.swap.used_bytes = 0;
    } else {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY,
            "wrong fake swap object");
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return valid;
}

static bool
fake_mount(
    void *context,
    const char *object_path,
    char mount_path[LARDON3D_SSD_PATH_CAPACITY],
    char reason[LARDON3D_SSD_REASON_CAPACITY]
)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    bool valid = strcmp(object_path,
        fake->observation.scratch.object_path) == 0;
    if (valid) {
        fake->observation.scratch.mounted = true;
        fake->observation.scratch.free_known = true;
        fake->observation.scratch.free_bytes = 300 * GIB;
        copy_text(fake->observation.scratch.mount_path,
            sizeof(fake->observation.scratch.mount_path),
            LARDON3D_SSD_SCRATCH_MOUNT_PATH);
        copy_text(mount_path, LARDON3D_SSD_PATH_CAPACITY,
            LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    } else {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY,
            "wrong fake scratch object");
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return valid;
}

static bool
fake_unmount(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    bool valid = strcmp(object_path,
        fake->observation.scratch.object_path) == 0;
    if (valid) {
        fake->observation.scratch.mounted = false;
        fake->observation.scratch.free_known = false;
        fake->observation.scratch.free_bytes = 0;
        fake->observation.scratch.mount_path[0] = '\0';
    } else {
        copy_text(reason, LARDON3D_SSD_REASON_CAPACITY,
            "wrong fake scratch object");
    }
    (void)pthread_mutex_unlock(&fake->mutex);
    return valid;
}

static void
fake_destroy(void *context)
{
    FakeStorage *fake = context;
    (void)pthread_mutex_lock(&fake->mutex);
    fake->destroyed = true;
    (void)pthread_mutex_unlock(&fake->mutex);
}

static const Lardon3DSsdProviderOps fake_ops = {
    .monotonic_now_ns = fake_now,
    .refresh = fake_refresh,
    .start_swap = fake_start_swap,
    .stop_swap = fake_stop_swap,
    .mount_scratch = fake_mount,
    .unmount_scratch = fake_unmount,
    .destroy = fake_destroy,
};

static bool
fake_storage_init(FakeStorage *fake)
{
    memset(fake, 0, sizeof(*fake));
    if (pthread_mutex_init(&fake->mutex, NULL) != 0) {
        return false;
    }
    fake->now_ns = UINT64_C(10000000000);
    Lardon3DSsdProviderSnapshot *source = &fake->observation;
    source->model_known = true;
    copy_text(source->model, sizeof(source->model), "Fake external SSD");
    source->swap.present = true;
    source->swap.unit_ready = true;
    source->swap.interface_available = true;
    source->swap.active_known = true;
    source->swap.active = true;
    source->swap.size_bytes = 8 * GIB;
    source->swap.total_known = true;
    source->swap.used_known = true;
    source->swap.total_bytes = 8 * GIB;
    copy_text(source->swap.label, sizeof(source->swap.label),
        LARDON3D_SSD_SWAP_LABEL);
    copy_text(source->swap.uuid, sizeof(source->swap.uuid), "fake-swap-uuid");
    copy_text(source->swap.drive_identity,
        sizeof(source->swap.drive_identity), "fake-drive-identity");
    copy_text(source->swap.object_path, sizeof(source->swap.object_path),
        "/org/fake/swap");
    copy_text(source->swap.device, sizeof(source->swap.device),
        "/dev/fake-swap");

    source->scratch.present = true;
    source->scratch.unit_ready = true;
    source->scratch.interface_available = true;
    source->scratch.mounted = true;
    source->scratch.size_bytes = 400 * GIB;
    source->scratch.total_known = true;
    source->scratch.free_known = true;
    source->scratch.total_bytes = 400 * GIB;
    source->scratch.free_bytes = 350 * GIB;
    copy_text(source->scratch.label, sizeof(source->scratch.label),
        LARDON3D_SSD_SCRATCH_LABEL);
    copy_text(source->scratch.uuid, sizeof(source->scratch.uuid),
        "fake-scratch-uuid");
    copy_text(source->scratch.drive_identity,
        sizeof(source->scratch.drive_identity), "fake-drive-identity");
    copy_text(source->scratch.object_path,
        sizeof(source->scratch.object_path), "/org/fake/scratch");
    copy_text(source->scratch.device, sizeof(source->scratch.device),
        "/dev/fake-scratch");
    copy_text(source->scratch.mount_path,
        sizeof(source->scratch.mount_path),
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);

    source->memory_available_known = true;
    source->memory_available_bytes = 16 * GIB;
    source->memory_pressure_known = true;
    source->io_pressure_known = true;
    source->swap_activity_known = true;
    return true;
}

static Lardon3DSsdController *
create_controller(FakeStorage *fake)
{
    return lardon3d_ssd_controller_create_with_provider(
        (Lardon3DSsdProvider) {
            .ops = &fake_ops,
            .context = fake,
        },
        UINT64_C(1000000000));
}

static Lardon3DResourceGovernor *
create_governor(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 8,
        .page_size_bytes = 4096,
        .memory_total_bytes = 16 * GIB,
        .cpu_architecture = "synthetic",
    };
    Lardon3DResourcePolicy policy;
    if (!lardon3d_resource_policy_default(&profile, &policy)) {
        return NULL;
    }
    return lardon3d_resource_governor_create(&profile, &policy);
}

static bool
register_controller(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller
)
{
    Lardon3DSsdSnapshot snapshot;
    Lardon3DResourceExternalStorage storage;
    return lardon3d_ssd_controller_copy_snapshot(controller, &snapshot)
        && lardon3d_resource_external_storage_from_ssd_snapshot(
            &snapshot, &storage)
        && lardon3d_resource_governor_register_external_storage(
            governor, controller, &storage);
}

typedef struct {
    Lardon3DResourceGovernor *governor;
    uint64_t generation;
    atomic_bool started;
    atomic_bool changed;
} WaitContext;

static void *
wait_for_generation(void *userdata)
{
    WaitContext *context = userdata;
    atomic_store(&context->started, true);
    atomic_store(&context->changed,
        lardon3d_resource_governor_wait_for_change(
            context->governor, context->generation,
            UINT64_C(2000000000)));
    return NULL;
}

typedef struct {
    Lardon3DResourceGovernor *governor;
    Lardon3DSsdController *controller;
    Lardon3DResourceExternalStorage storage;
    bool updated;
} UpdateCall;

static void *
update_registry_once(void *userdata)
{
    UpdateCall *call = userdata;
    call->updated = lardon3d_resource_governor_update_external_storage(
        call->governor, call->controller, &call->storage);
    return NULL;
}

static bool
test_registry_and_wrappers(void)
{
    FakeStorage fake;
    CHECK(fake_storage_init(&fake));
    Lardon3DSsdController *controller = create_controller(&fake);
    Lardon3DResourceGovernor *governor = create_governor();
    CHECK(controller && governor);

    Lardon3DSsdSnapshot physical;
    CHECK(lardon3d_ssd_controller_copy_snapshot(controller, &physical));
    CHECK(physical.state == LARDON3D_SSD_ENABLED);
    CHECK(physical.scratch_allocations_allowed);

    Lardon3DResourceSnapshot host = {.memory_available_bytes = 8 * GIB};
    Lardon3DResourceAvailability before;
    Lardon3DResourceAvailability after;
    CHECK(lardon3d_resource_governor_availability(
        governor, &host, &before));
    CHECK(register_controller(governor, controller));
    CHECK(!lardon3d_resource_governor_register_external_storage(
        governor, controller, &(Lardon3DResourceExternalStorage) {0}));

    Lardon3DResourceExternalStorage external;
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE);
    CHECK(external.new_scratch_allocations_allowed);
    CHECK(external.active_scratch_leases == 0);
    CHECK(strcmp(external.stable_identity, "fake-drive-identity") == 0);

    /* Hundreds of GiB of registered scratch plus external swap is not host
     * RAM and cannot change any admission counter or budget. */
    CHECK(lardon3d_resource_governor_availability(
        governor, &host, &after));
    CHECK(before.memory_budget_bytes == after.memory_budget_bytes);
    CHECK(before.memory_reserved_bytes == after.memory_reserved_bytes);
    CHECK(before.memory_available_bytes == after.memory_available_bytes);
    CHECK(before.cpu_budget == after.cpu_budget);
    CHECK(before.cpu_available == after.cpu_available);
    CHECK(before.active_reservations == after.active_reservations);

    Lardon3DSsdScratchLease lease = {0};
    CHECK(lardon3d_resource_governor_acquire_scratch(
        governor, controller, &lease));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE);
    CHECK(external.active_scratch_leases == 1);
    CHECK(!lardon3d_resource_governor_acquire_scratch(
        governor, controller, &lease));
    Lardon3DSsdScratchLease copied_lease = lease;
    CHECK(!lardon3d_resource_governor_release_scratch(
        governor, controller, &copied_lease));
    CHECK(!lardon3d_resource_governor_unregister_external_storage(
        governor, controller));
    CHECK(!lardon3d_ssd_controller_destroy(controller));

    CHECK(lardon3d_ssd_controller_copy_snapshot(controller, &physical));
    UpdateCall update = {
        .governor = governor,
        .controller = controller,
    };
    CHECK(lardon3d_resource_external_storage_from_ssd_snapshot(
        &physical, &update.storage));
    ExternalUpdateBarrier update_barrier = {
        .governor = governor,
        .controller = controller,
    };
    CHECK(pthread_mutex_init(&update_barrier.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&update_barrier.condition, NULL) == 0);
    atomic_store_explicit(
        &active_update_barrier, &update_barrier, memory_order_release);
    pthread_t update_thread;
    CHECK(pthread_create(&update_thread, NULL,
        update_registry_once, &update) == 0);
    (void)pthread_mutex_lock(&update_barrier.mutex);
    while (!update_barrier.engaged) {
        (void)pthread_cond_wait(
            &update_barrier.condition, &update_barrier.mutex);
    }
    (void)pthread_mutex_unlock(&update_barrier.mutex);

    CHECK(lardon3d_ssd_controller_disable(controller)
        == LARDON3D_SSD_CONTROL_PENDING);
    (void)pthread_mutex_lock(&update_barrier.mutex);
    update_barrier.release = true;
    (void)pthread_cond_broadcast(&update_barrier.condition);
    (void)pthread_mutex_unlock(&update_barrier.mutex);
    CHECK(pthread_join(update_thread, NULL) == 0);
    atomic_store_explicit(
        &active_update_barrier, NULL, memory_order_release);
    CHECK(update.updated);
    CHECK(pthread_cond_destroy(&update_barrier.condition) == 0);
    CHECK(pthread_mutex_destroy(&update_barrier.mutex) == 0);

    CHECK(lardon3d_ssd_controller_copy_snapshot(controller, &physical));
    CHECK(lardon3d_resource_external_storage_from_ssd_snapshot(
        &physical, &external));
    CHECK(lardon3d_resource_governor_update_external_storage(
        governor, controller, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING);
    CHECK(!external.new_scratch_allocations_allowed);
    Lardon3DSsdScratchLease rejected = {0};
    CHECK(!lardon3d_resource_governor_acquire_scratch(
        governor, controller, &rejected));

    CHECK(lardon3d_resource_governor_release_scratch(
        governor, controller, &lease));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE);
    CHECK(external.active_scratch_leases == 0);
    CHECK(lease.opaque_controller == 0 && lease.opaque_lease_id == 0);

    uint64_t aggregate_generation =
        lardon3d_resource_governor_generation(governor);
    ++external.generation;
    CHECK(lardon3d_resource_governor_update_external_storage(
        governor, controller, &external));
    CHECK(lardon3d_resource_governor_generation(governor)
        == aggregate_generation);

    WaitContext waiter = {
        .governor = governor,
        .generation = aggregate_generation,
    };
    pthread_t wait_thread;
    CHECK(pthread_create(&wait_thread, NULL,
        wait_for_generation, &waiter) == 0);
    while (!atomic_load(&waiter.started)) {
        sched_yield();
    }
    ++external.generation;
    copy_text(external.reason, sizeof(external.reason),
        "material external storage change");
    CHECK(lardon3d_resource_governor_update_external_storage(
        governor, controller, &external));
    CHECK(pthread_join(wait_thread, NULL) == 0);
    CHECK(atomic_load(&waiter.changed));

    Lardon3DResourceExternalStorage stale = external;
    ++external.generation;
    external.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT;
    external.new_scratch_allocations_allowed = false;
    external.active_scratch_leases = 0;
    CHECK(lardon3d_resource_governor_update_external_storage(
        governor, controller, &external));
    CHECK(!lardon3d_resource_governor_acquire_scratch(
        governor, controller, &rejected));
    stale.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE;
    stale.new_scratch_allocations_allowed = true;
    copy_text(stale.reason, sizeof(stale.reason),
        "stale availability must not restore allocation");
    CHECK(!lardon3d_resource_governor_update_external_storage(
        governor, controller, &stale));
    Lardon3DResourceExternalStorage retained;
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &retained));
    CHECK(retained.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT);
    ++external.generation;
    external.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR;
    copy_text(external.stable_identity, sizeof(external.stable_identity),
        "replacement-drive");
    copy_text(external.reason, sizeof(external.reason),
        "replacement or malformed state blocks allocation");
    CHECK(lardon3d_resource_governor_update_external_storage(
        governor, controller, &external));
    CHECK(!lardon3d_resource_governor_acquire_scratch(
        governor, controller, &rejected));

    CHECK(lardon3d_resource_governor_unregister_external_storage(
        governor, controller));
    external = (Lardon3DResourceExternalStorage) {
        .generation = UINT64_MAX,
        .status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR,
        .active_scratch_leases = LARDON3D_SSD_MAX_SCRATCH_LEASES,
    };
    CHECK(!lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.generation == 0 && external.active_scratch_leases == 0);
    CHECK(!lardon3d_resource_governor_update_external_storage(
        governor, controller, &external));

    CHECK(lardon3d_ssd_controller_destroy(controller));
    (void)pthread_mutex_lock(&fake.mutex);
    bool destroyed = fake.destroyed;
    (void)pthread_mutex_unlock(&fake.mutex);
    CHECK(destroyed);
    lardon3d_resource_governor_destroy(governor);
    CHECK(pthread_mutex_destroy(&fake.mutex) == 0);
    return true;
}

static bool
complete_enabled_snapshot(Lardon3DSsdSnapshot *snapshot)
{
    if (!snapshot) {
        return false;
    }
    *snapshot = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ENABLED,
        .generation = 19,
        .device_detected = true,
        .pairing_valid = true,
        .swap_detected = true,
        .scratch_detected = true,
        .swap_partition_size_known = true,
        .scratch_partition_size_known = true,
        .swap_partition_size_bytes = 8 * GIB,
        .scratch_partition_size_bytes = 400 * GIB,
        .swap_active = true,
        .scratch_mounted = true,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        .scratch_allocations_allowed = true,
        .can_disable = true,
    };
    copy_text(snapshot->drive_identity, sizeof(snapshot->drive_identity),
        "synthetic-drive");
    copy_text(snapshot->swap_uuid, sizeof(snapshot->swap_uuid),
        "synthetic-swap");
    copy_text(snapshot->scratch_uuid, sizeof(snapshot->scratch_uuid),
        "synthetic-scratch");
    copy_text(snapshot->swap_device, sizeof(snapshot->swap_device),
        "/dev/synthetic-swap");
    copy_text(snapshot->scratch_device, sizeof(snapshot->scratch_device),
        "/dev/synthetic-scratch");
    copy_text(snapshot->scratch_mount_path,
        sizeof(snapshot->scratch_mount_path),
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    copy_text(snapshot->reason, sizeof(snapshot->reason),
        "complete synthetic pair");
    return true;
}

static bool
test_malformed_conversion(void)
{
    Lardon3DSsdSnapshot malformed = {
        .state = LARDON3D_SSD_ENABLED,
        .pairing_valid = true,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        .scratch_allocations_allowed = true,
    };
    memset(malformed.reason, 'x', sizeof(malformed.reason));
    Lardon3DResourceExternalStorage storage = {
        .generation = UINT64_MAX,
        .status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE,
        .active_scratch_leases = 9,
    };
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0 && storage.active_scratch_leases == 0);

    malformed = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ABSENT,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        .swap_total_bytes = 1,
    };
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0 && storage.swap_total_bytes == 0);

    malformed = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ENABLED,
        .pairing_valid = true,
        .swap_active = true,
        .scratch_mounted = true,
        .scratch_allocations_allowed = true,
        .can_disable = true,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
    };
    (void)snprintf(malformed.scratch_mount_path,
        sizeof(malformed.scratch_mount_path), "%s",
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0);

    Lardon3DSsdSnapshot complete;
    CHECK(complete_enabled_snapshot(&complete));
    CHECK(lardon3d_resource_external_storage_from_ssd_snapshot(
        &complete, &storage));
    CHECK(storage.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE);
    CHECK(storage.new_scratch_allocations_allowed);

    /* Exact tuple strings cannot substitute for present partitions and their
     * positive UDisks extents. */
    malformed = complete;
    malformed.device_detected = false;
    malformed.swap_detected = false;
    malformed.scratch_detected = false;
    malformed.swap_partition_size_known = false;
    malformed.scratch_partition_size_known = false;
    malformed.swap_partition_size_bytes = 0;
    malformed.scratch_partition_size_bytes = 0;
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0 && storage.active_scratch_leases == 0);

    malformed = complete;
    malformed.swap_partition_size_known = false;
    malformed.scratch_partition_size_known = false;
    malformed.swap_partition_size_bytes = 0;
    malformed.scratch_partition_size_bytes = 0;
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0);

    malformed = complete;
    malformed.swap_partition_size_bytes = 0;
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0);

    malformed = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ABSENT,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        .swap_active = true,
        .scratch_mounted = true,
    };
    copy_text(malformed.scratch_mount_path,
        sizeof(malformed.scratch_mount_path),
        LARDON3D_SSD_SCRATCH_MOUNT_PATH);
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0);

    malformed = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ABSENT,
        .device_detected = true,
        .swap_detected = true,
        .swap_partition_size_known = true,
        .swap_partition_size_bytes = 8 * GIB,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
    };
    CHECK(!lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.generation == 0);

    /* A disconnected sticky hazard is ERROR, never ABSENT. It may retain its
     * exact tuple and lease count, but grants no allocation/control authority. */
    malformed = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_ERROR,
        .generation = 23,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
        .scratch_lease_count = 1,
    };
    copy_text(malformed.drive_identity, sizeof(malformed.drive_identity),
        "retained-hazard-drive");
    copy_text(malformed.swap_uuid, sizeof(malformed.swap_uuid),
        "retained-hazard-swap");
    copy_text(malformed.scratch_uuid, sizeof(malformed.scratch_uuid),
        "retained-hazard-scratch");
    copy_text(malformed.reason, sizeof(malformed.reason),
        "original pair is disconnected");
    CHECK(lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(storage.active_scratch_leases == 1);
    CHECK(!storage.new_scratch_allocations_allowed);

    malformed = (Lardon3DSsdSnapshot) {
        .state = LARDON3D_SSD_DETECTED,
        .generation = 24,
        .device_detected = true,
        .swap_detected = true,
        .swap_partition_size_known = true,
        .swap_partition_size_bytes = 8 * GIB,
        .scratch_lease_capacity = LARDON3D_SSD_MAX_SCRATCH_LEASES,
    };
    copy_text(malformed.swap_uuid, sizeof(malformed.swap_uuid),
        "partial-swap");
    copy_text(malformed.reason, sizeof(malformed.reason),
        "scratch partition is not detected");
    CHECK(lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED);
    CHECK(!storage.new_scratch_allocations_allowed);

    /* Drain-only recovery is legitimate only with the complete reconnected
     * pair evidence, even when the exact pair is already inactive. */
    malformed = complete;
    malformed.state = LARDON3D_SSD_ERROR;
    malformed.swap_active = false;
    malformed.scratch_mounted = false;
    malformed.scratch_allocations_allowed = false;
    malformed.can_disable = true;
    copy_text(malformed.scratch_mount_path,
        sizeof(malformed.scratch_mount_path), "UNKNOWN");
    CHECK(lardon3d_resource_external_storage_from_ssd_snapshot(
        &malformed, &storage));
    CHECK(storage.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(strcmp(lardon3d_resource_external_storage_status_name(
        LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING), "DRAINING") == 0);
    return true;
}

static bool
test_checked_async_teardown_with_live_lease(void)
{
    FakeStorage fake;
    CHECK(fake_storage_init(&fake));
    Lardon3DSsdController *controller = create_controller(&fake);
    Lardon3DResourceGovernor *governor = create_governor();
    CHECK(controller && governor);
    Lardon3DTuiSsdAsync *operation =
        lardon3d_tui_ssd_async_create_with_governor(controller, governor);
    CHECK(operation);
    CHECK(lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));

    Lardon3DSsdScratchLease lease = {0};
    CHECK(lardon3d_resource_governor_acquire_scratch(
        governor, controller, &lease));
    Lardon3DTuiSsdAsync *retained = operation;
    CHECK(!lardon3d_tui_ssd_async_destroy_checked(&retained));
    CHECK(retained == operation);
    Lardon3DResourceExternalStorage external;
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.active_scratch_leases == 1);

    CHECK(lardon3d_resource_governor_release_scratch(
        governor, controller, &lease));
    CHECK(lardon3d_tui_ssd_async_destroy_checked(&retained));
    CHECK(retained == NULL);
    CHECK(!lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    lardon3d_resource_governor_destroy(governor);
    CHECK(pthread_mutex_destroy(&fake.mutex) == 0);
    return true;
}

static bool
test_saturated_generation_wrapper_reconciliation(void)
{
    FakeStorage fake;
    CHECK(fake_storage_init(&fake));
    Lardon3DSsdController *controller = create_controller(&fake);
    Lardon3DResourceGovernor *governor = create_governor();
    CHECK(controller && governor);
    CHECK(lardon3d_ssd_controller_set_generation_for_test(
        controller, UINT64_MAX));

    Lardon3DTuiSsdAsync *operation =
        lardon3d_tui_ssd_async_create_with_governor(controller, governor);
    CHECK(operation && lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));

    Lardon3DResourceExternalStorage external;
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.generation == UINT64_MAX);
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE);
    CHECK(external.active_scratch_leases == 0);

    /* Public equal-watermark telemetry remains stale even at saturation. It
     * cannot fabricate a lease or grant materially different authority. */
    Lardon3DResourceExternalStorage stale_public = external;
    stale_public.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE;
    stale_public.active_scratch_leases = 1;
    copy_text(stale_public.reason, sizeof(stale_public.reason),
        "same-generation public authority must be rejected");
    CHECK(!lardon3d_resource_governor_update_external_storage(
        governor, controller, &stale_public));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE);
    CHECK(external.active_scratch_leases == 0);

    Lardon3DSsdScratchLease lease = {0};
    CHECK(lardon3d_resource_governor_acquire_scratch(
        governor, controller, &lease));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.generation == UINT64_MAX);
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE);
    CHECK(external.active_scratch_leases == 1);

    CHECK(lardon3d_resource_governor_release_scratch(
        governor, controller, &lease));
    CHECK(lease.opaque_controller == 0 && lease.opaque_lease_id == 0);
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.generation == UINT64_MAX);
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE);
    CHECK(external.active_scratch_leases == 0);

    /* Even when the post-release cached copy is malformed, the successful
     * physical release and exact address registry are authoritative for zero.
     * Conservative ERROR must not resurrect the previous count of one. */
    CHECK(lardon3d_resource_governor_acquire_scratch(
        governor, controller, &lease));
    CHECK(lardon3d_ssd_controller_corrupt_cached_snapshot_for_test(
        controller));
    CHECK(lardon3d_resource_governor_release_scratch(
        governor, controller, &lease));
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.generation == UINT64_MAX);
    CHECK(external.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR);
    CHECK(external.active_scratch_leases == 0);
    CHECK(!external.new_scratch_allocations_allowed);

    CHECK(lardon3d_tui_ssd_async_destroy_checked(&operation));
    CHECK(operation == NULL);
    CHECK(!lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    lardon3d_resource_governor_destroy(governor);
    CHECK(pthread_mutex_destroy(&fake.mutex) == 0);
    return true;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    Lardon3DResourceGovernor *governor;
    Lardon3DSsdController *controller;
    Lardon3DSsdScratchLease lease;
    bool acquired;
    bool release;
    bool released;
} TaskLeaseLifecycle;

static bool
wait_flag(
    pthread_mutex_t *mutex,
    pthread_cond_t *condition,
    const bool *flag
)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += 10;
    (void)pthread_mutex_lock(mutex);
    int result = 0;
    while (!*flag && result == 0) {
        result = pthread_cond_timedwait(condition, mutex, &deadline);
    }
    bool observed = *flag;
    (void)pthread_mutex_unlock(mutex);
    return result == 0 && observed;
}

static bool
task_lease_callback(Lardon3DTask *task, void *userdata)
{
    (void)task;
    TaskLeaseLifecycle *lifecycle = userdata;
    if (!lardon3d_resource_governor_acquire_scratch(
            lifecycle->governor, lifecycle->controller,
            &lifecycle->lease)) {
        return false;
    }
    (void)pthread_mutex_lock(&lifecycle->mutex);
    lifecycle->acquired = true;
    (void)pthread_cond_broadcast(&lifecycle->condition);
    while (!lifecycle->release) {
        (void)pthread_cond_wait(&lifecycle->condition, &lifecycle->mutex);
    }
    (void)pthread_mutex_unlock(&lifecycle->mutex);

    bool released = lardon3d_resource_governor_release_scratch(
        lifecycle->governor, lifecycle->controller, &lifecycle->lease);
    (void)pthread_mutex_lock(&lifecycle->mutex);
    lifecycle->released = released;
    (void)pthread_cond_broadcast(&lifecycle->condition);
    (void)pthread_mutex_unlock(&lifecycle->mutex);
    return released;
}

typedef struct {
    Lardon3DTaskQueue *queue;
    atomic_bool returned;
} QueueDestroyCall;

static void *
destroy_queue(void *userdata)
{
    QueueDestroyCall *call = userdata;
    lardon3d_task_queue_destroy(call->queue);
    atomic_store_explicit(&call->returned, true, memory_order_release);
    return NULL;
}

static bool
test_queue_before_bound_storage_teardown(void)
{
    FakeStorage fake;
    CHECK(fake_storage_init(&fake));
    Lardon3DSsdController *controller = create_controller(&fake);
    Lardon3DResourceGovernor *governor = create_governor();
    CHECK(controller && governor);
    Lardon3DTuiSsdAsync *operation =
        lardon3d_tui_ssd_async_create_with_governor(controller, governor);
    CHECK(operation && lardon3d_tui_ssd_async_refresh(operation));
    CHECK(lardon3d_tui_ssd_async_wait_idle(
        operation, UINT64_C(1000000000)));

    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    TaskLeaseLifecycle lifecycle = {
        .governor = governor,
        .controller = controller,
    };
    CHECK(queue && pthread_mutex_init(&lifecycle.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&lifecycle.condition, NULL) == 0);
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DTask *task = lardon3d_task_create(
        "Task holding external scratch", &estimate,
        task_lease_callback, &lifecycle);
    CHECK(task && lardon3d_task_queue_add(queue, task, NULL));
    CHECK(wait_flag(&lifecycle.mutex, &lifecycle.condition,
        &lifecycle.acquired));

    QueueClosingProbe probe = {.queue = queue};
    CHECK(pthread_mutex_init(&probe.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&probe.condition, NULL) == 0);
    atomic_store_explicit(
        &active_closing_probe, &probe, memory_order_release);
    QueueDestroyCall destroy_call = {.queue = queue};
    pthread_t destroy_thread;
    CHECK(pthread_create(&destroy_thread, NULL,
        destroy_queue, &destroy_call) == 0);
    CHECK(wait_flag(&probe.mutex, &probe.condition, &probe.closing));
    CHECK(!atomic_load_explicit(
        &destroy_call.returned, memory_order_acquire));

    Lardon3DResourceExternalStorage external;
    CHECK(lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(external.active_scratch_leases == 1);
    (void)pthread_mutex_lock(&lifecycle.mutex);
    lifecycle.release = true;
    (void)pthread_cond_broadcast(&lifecycle.condition);
    (void)pthread_mutex_unlock(&lifecycle.mutex);
    CHECK(wait_flag(&lifecycle.mutex, &lifecycle.condition,
        &lifecycle.released));
    CHECK(pthread_join(destroy_thread, NULL) == 0);
    CHECK(atomic_load_explicit(
        &destroy_call.returned, memory_order_acquire));
    atomic_store_explicit(
        &active_closing_probe, NULL, memory_order_release);

    /* This is the application ownership boundary: Queue returned only after
     * the Task released its lease, so checked teardown can now unregister
     * before either borrowed physical owner is destroyed. */
    CHECK(lardon3d_tui_ssd_async_destroy_checked(&operation));
    CHECK(operation == NULL);
    CHECK(!lardon3d_resource_governor_get_external_storage(
        governor, &external));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    lardon3d_resource_governor_destroy(governor);

    CHECK(pthread_cond_destroy(&probe.condition) == 0);
    CHECK(pthread_mutex_destroy(&probe.mutex) == 0);
    CHECK(pthread_cond_destroy(&lifecycle.condition) == 0);
    CHECK(pthread_mutex_destroy(&lifecycle.mutex) == 0);
    CHECK(pthread_mutex_destroy(&fake.mutex) == 0);
    return true;
}

int
main(void)
{
    return test_registry_and_wrappers() && test_malformed_conversion()
        && test_checked_async_teardown_with_live_lease()
        && test_saturated_generation_wrapper_reconciliation()
        && test_queue_before_bound_storage_teardown()
        ? 0 : 1;
}
