#include "../src/ssd_controller_internal.h"

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KIB UINT64_C(1024)
#define MIB (UINT64_C(1024) * KIB)
#define GIB (UINT64_C(1024) * MIB)

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                  \
            return false;                                                         \
        }                                                                         \
    } while (0)

typedef struct {
    uint64_t now_ns;
    Lardon3DSsdProviderSnapshot observation;
    size_t refresh_calls;
    size_t start_calls;
    size_t stop_calls;
    size_t mount_calls;
    size_t unmount_calls;
    bool fail_refresh;
    bool fail_next_refresh;
    bool fail_start;
    bool fail_stop;
    bool fail_mount;
    bool fail_unmount;
    bool fail_refresh_after_start;
    bool fail_refresh_after_mount;
    bool start_effect_on_failure;
    bool mount_effect_on_failure;
    bool start_without_effect;
    bool stop_without_effect;
    bool mount_without_effect;
    bool unmount_without_effect;
    char failure_reason[LARDON3D_SSD_REASON_CAPACITY];
    char mount_result[LARDON3D_SSD_PATH_CAPACITY];
    bool destroyed;
} FakeProvider;

static void text_copy(char *destination, size_t capacity, const char *source) {
    (void)snprintf(destination, capacity, "%s", source ? source : "");
}

static void fake_make_valid(FakeProvider *fake) {
    memset(fake, 0, sizeof(*fake));
    fake->now_ns = UINT64_C(10000000000);
    text_copy(fake->failure_reason, sizeof(fake->failure_reason), "injected timeout");
    text_copy(
        fake->mount_result,
        sizeof(fake->mount_result),
        LARDON3D_SSD_SCRATCH_MOUNT_PATH
    );

    fake->observation.model_known = true;
    fake->observation.serial_known = true;
    fake->observation.connection_speed_known = true;
    text_copy(fake->observation.model, sizeof(fake->observation.model), "Test SSD");
    text_copy(fake->observation.serial, sizeof(fake->observation.serial), "TEST-SERIAL");
    fake->observation.connection_speed_mbps = 10000;

    fake->observation.swap.present = true;
    fake->observation.swap.unit_ready = true;
    fake->observation.swap.interface_available = true;
    fake->observation.swap.active_known = true;
    fake->observation.swap.size_bytes = 8 * GIB;
    text_copy(
        fake->observation.swap.label,
        sizeof(fake->observation.swap.label),
        LARDON3D_SSD_SWAP_LABEL
    );
    text_copy(
        fake->observation.swap.uuid,
        sizeof(fake->observation.swap.uuid),
        "00000000-0000-0000-0000-000000000001"
    );
    text_copy(
        fake->observation.swap.drive_identity,
        sizeof(fake->observation.swap.drive_identity),
        "/org/freedesktop/UDisks2/drives/test_drive"
    );
    text_copy(
        fake->observation.swap.object_path,
        sizeof(fake->observation.swap.object_path),
        "/org/freedesktop/UDisks2/block_devices/sdz1"
    );
    text_copy(
        fake->observation.swap.device,
        sizeof(fake->observation.swap.device),
        "/dev/sdz1"
    );
    fake->observation.swap.total_known = true;
    fake->observation.swap.used_known = true;
    fake->observation.swap.total_bytes = 8 * GIB;

    fake->observation.scratch.present = true;
    fake->observation.scratch.unit_ready = true;
    fake->observation.scratch.interface_available = true;
    fake->observation.scratch.size_bytes = 400 * GIB;
    text_copy(
        fake->observation.scratch.label,
        sizeof(fake->observation.scratch.label),
        LARDON3D_SSD_SCRATCH_LABEL
    );
    text_copy(
        fake->observation.scratch.uuid,
        sizeof(fake->observation.scratch.uuid),
        "00000000-0000-0000-0000-000000000002"
    );
    text_copy(
        fake->observation.scratch.drive_identity,
        sizeof(fake->observation.scratch.drive_identity),
        "/org/freedesktop/UDisks2/drives/test_drive"
    );
    text_copy(
        fake->observation.scratch.object_path,
        sizeof(fake->observation.scratch.object_path),
        "/org/freedesktop/UDisks2/block_devices/sdz2"
    );
    text_copy(
        fake->observation.scratch.device,
        sizeof(fake->observation.scratch.device),
        "/dev/sdz2"
    );
    fake->observation.scratch.total_known = true;
    fake->observation.scratch.total_bytes = 400 * GIB;

    fake->observation.memory_available_known = true;
    fake->observation.memory_available_bytes = 16 * GIB;
    fake->observation.memory_pressure_known = true;
    fake->observation.io_pressure_known = true;
    fake->observation.swap_activity_known = true;
}

static bool fake_now(void *context, uint64_t *now_ns) {
    FakeProvider *fake = context;
    *now_ns = fake->now_ns;
    return true;
}

static bool fake_refresh(
    void *context,
    Lardon3DSsdProviderSnapshot *snapshot,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    FakeProvider *fake = context;
    fake->refresh_calls += 1;
    if (fake->fail_next_refresh) {
        fake->fail_next_refresh = false;
        text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, fake->failure_reason);
        return false;
    }
    if (fake->fail_refresh) {
        text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, fake->failure_reason);
        return false;
    }
    *snapshot = fake->observation;
    reason[0] = '\0';
    return true;
}

static bool fake_check_object(
    const char *actual,
    const char *expected,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    if (strcmp(actual, expected) == 0) {
        return true;
    }
    text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, "wrong object path");
    return false;
}

static bool fake_start(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    FakeProvider *fake = context;
    fake->start_calls += 1;
    if (!fake_check_object(
            object_path,
            fake->observation.swap.object_path,
            reason
        )) {
        return false;
    }
    if (!fake->start_without_effect
        && (!fake->fail_start || fake->start_effect_on_failure)) {
        fake->observation.swap.active = true;
    }
    if (fake->fail_refresh_after_start) {
        fake->fail_next_refresh = true;
    }
    if (fake->fail_start) {
        text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, fake->failure_reason);
        return false;
    }
    return true;
}

static bool fake_stop(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    FakeProvider *fake = context;
    fake->stop_calls += 1;
    if (!fake_check_object(
            object_path,
            fake->observation.swap.object_path,
            reason
        )) {
        return false;
    }
    if (fake->fail_stop) {
        text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, fake->failure_reason);
        return false;
    }
    if (!fake->stop_without_effect) {
        fake->observation.swap.active = false;
        fake->observation.swap.used_bytes = 0;
    }
    return true;
}

static bool fake_mount(
    void *context,
    const char *object_path,
    char mount_path[LARDON3D_SSD_PATH_CAPACITY],
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    FakeProvider *fake = context;
    fake->mount_calls += 1;
    if (!fake_check_object(
            object_path,
            fake->observation.scratch.object_path,
            reason
        )) {
        return false;
    }
    text_copy(mount_path, LARDON3D_SSD_PATH_CAPACITY, fake->mount_result);
    if (!fake->mount_without_effect
        && (!fake->fail_mount || fake->mount_effect_on_failure)) {
        fake->observation.scratch.mounted = true;
        fake->observation.scratch.free_known = true;
        fake->observation.scratch.free_bytes = 350 * GIB;
        text_copy(
            fake->observation.scratch.mount_path,
            sizeof(fake->observation.scratch.mount_path),
            fake->mount_result
        );
    }
    if (fake->fail_refresh_after_mount) {
        fake->fail_next_refresh = true;
    }
    if (fake->fail_mount) {
        text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, fake->failure_reason);
        return false;
    }
    return true;
}

static bool fake_unmount(
    void *context,
    const char *object_path,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
) {
    FakeProvider *fake = context;
    fake->unmount_calls += 1;
    if (!fake_check_object(
            object_path,
            fake->observation.scratch.object_path,
            reason
        )) {
        return false;
    }
    if (fake->fail_unmount) {
        text_copy(reason, LARDON3D_SSD_REASON_CAPACITY, fake->failure_reason);
        return false;
    }
    if (!fake->unmount_without_effect) {
        fake->observation.scratch.mounted = false;
        fake->observation.scratch.free_known = false;
        fake->observation.scratch.mount_path[0] = '\0';
    }
    return true;
}

static void fake_destroy(void *context) {
    FakeProvider *fake = context;
    fake->destroyed = true;
}

static const Lardon3DSsdProviderOps FAKE_OPS = {
    .monotonic_now_ns = fake_now,
    .refresh = fake_refresh,
    .start_swap = fake_start,
    .stop_swap = fake_stop,
    .mount_scratch = fake_mount,
    .unmount_scratch = fake_unmount,
    .destroy = fake_destroy,
};

static Lardon3DSsdController *fake_controller(FakeProvider *fake) {
    const Lardon3DSsdProvider provider = {
        .ops = &FAKE_OPS,
        .context = fake,
    };
    return lardon3d_ssd_controller_create_with_provider(
        provider,
        UINT64_C(1000000000)
    );
}

#ifdef LARDON3D_SSD_CONTROLLER_TESTING
static GVariant *fixture_block_properties(
    const char *label,
    const char *uuid,
    const char *drive,
    const char *device,
    uint64_t size
) {
    GVariantBuilder properties;

    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&properties, "{sv}", "IdLabel", g_variant_new_string(label));
    g_variant_builder_add(&properties, "{sv}", "IdUUID", g_variant_new_string(uuid));
    g_variant_builder_add(
        &properties,
        "{sv}",
        "Drive",
        g_variant_new_object_path(drive)
    );
    g_variant_builder_add(
        &properties,
        "{sv}",
        "Device",
        g_variant_new_bytestring(device)
    );
    g_variant_builder_add(&properties, "{sv}", "Size", g_variant_new_uint64(size));
    return g_variant_builder_end(&properties);
}

static GVariant *fixture_drive_interfaces(void) {
    GVariantBuilder interfaces;
    GVariantBuilder properties;

    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(
        &properties,
        "{sv}",
        "MediaAvailable",
        g_variant_new_boolean(true)
    );
    g_variant_builder_add(
        &properties,
        "{sv}",
        "Model",
        g_variant_new_string("Fixture SSD")
    );
    g_variant_builder_add(
        &properties,
        "{sv}",
        "Serial",
        g_variant_new_string("FIXTURE-SERIAL")
    );

    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(
        &interfaces,
        "{s@a{sv}}",
        "org.freedesktop.UDisks2.Drive",
        g_variant_builder_end(&properties)
    );
    return g_variant_builder_end(&interfaces);
}

typedef enum {
    FIXTURE_SWAP_ACTIVE_BOOLEAN = 0,
    FIXTURE_SWAP_ACTIVE_MISSING,
    FIXTURE_SWAP_ACTIVE_TEXT,
} FixtureSwapActiveMode;

static GVariant *fixture_swap_interfaces(
    const char *drive,
    FixtureSwapActiveMode active_mode
) {
    GVariantBuilder interfaces;
    GVariantBuilder properties;

    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(
        &interfaces,
        "{s@a{sv}}",
        "org.freedesktop.UDisks2.Block",
        fixture_block_properties(
            LARDON3D_SSD_SWAP_LABEL,
            "fixture-swap-uuid",
            drive,
            "/dev/fixture1",
            8 * GIB
        )
    );

    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    if (active_mode == FIXTURE_SWAP_ACTIVE_BOOLEAN) {
        g_variant_builder_add(
            &properties,
            "{sv}",
            "Active",
            g_variant_new_boolean(true)
        );
    } else if (active_mode == FIXTURE_SWAP_ACTIVE_TEXT) {
        g_variant_builder_add(
            &properties,
            "{sv}",
            "Active",
            g_variant_new_string("true")
        );
    }
    g_variant_builder_add(
        &interfaces,
        "{s@a{sv}}",
        "org.freedesktop.UDisks2.Swapspace",
        g_variant_builder_end(&properties)
    );
    return g_variant_builder_end(&interfaces);
}

static GVariant *fixture_scratch_interfaces(const char *drive) {
    GVariantBuilder interfaces;
    GVariantBuilder properties;
    GVariantBuilder mount_points;

    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(
        &interfaces,
        "{s@a{sv}}",
        "org.freedesktop.UDisks2.Block",
        fixture_block_properties(
            LARDON3D_SSD_SCRATCH_LABEL,
            "fixture-scratch-uuid",
            drive,
            "/dev/fixture2",
            400 * GIB
        )
    );

    g_variant_builder_init(&mount_points, G_VARIANT_TYPE("aay"));
    g_variant_builder_add_value(
        &mount_points,
        g_variant_new_bytestring(LARDON3D_SSD_SCRATCH_MOUNT_PATH)
    );
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(
        &properties,
        "{sv}",
        "MountPoints",
        g_variant_builder_end(&mount_points)
    );
    g_variant_builder_add(
        &interfaces,
        "{s@a{sv}}",
        "org.freedesktop.UDisks2.Filesystem",
        g_variant_builder_end(&properties)
    );
    return g_variant_builder_end(&interfaces);
}

static GVariant *fixture_managed_objects(FixtureSwapActiveMode active_mode) {
    static const char drive[] = "/org/freedesktop/UDisks2/drives/fixture_drive";
    GVariantBuilder objects;

    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    g_variant_builder_add(
        &objects,
        "{o@a{sa{sv}}}",
        drive,
        fixture_drive_interfaces()
    );
    g_variant_builder_add(
        &objects,
        "{o@a{sa{sv}}}",
        "/org/freedesktop/UDisks2/block_devices/fixture1",
        fixture_swap_interfaces(drive, active_mode)
    );
    g_variant_builder_add(
        &objects,
        "{o@a{sa{sv}}}",
        "/org/freedesktop/UDisks2/block_devices/fixture2",
        fixture_scratch_interfaces(drive)
    );
    return g_variant_ref_sink(g_variant_builder_end(&objects));
}
#endif

static bool snapshot_of(
    Lardon3DSsdController *controller,
    Lardon3DSsdSnapshot *snapshot
) {
    return lardon3d_ssd_controller_get_snapshot(controller, snapshot);
}

static bool snapshot_actions_are(
    const Lardon3DSsdSnapshot *snapshot,
    bool can_enable,
    bool can_disable,
    bool can_cancel_drain
) {
    return snapshot && snapshot->can_enable == can_enable
        && snapshot->can_disable == can_disable
        && snapshot->can_cancel_drain == can_cancel_drain;
}

static bool test_absent_cache_appearance_and_node_rename(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;
    char stable_identity[LARDON3D_SSD_IDENTITY_CAPACITY];

    fake_make_valid(&fake);
    memset(&fake.observation, 0, sizeof(fake.observation));
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ABSENT);
    CHECK(snapshot_actions_are(&snapshot, false, false, false));
    CHECK(fake.refresh_calls == 1);

    fake_make_valid(&fake);
    fake.refresh_calls = 1;
    fake.now_ns += UINT64_C(500000000);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ABSENT);
    CHECK(fake.refresh_calls == 1);
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(fake.refresh_calls == 2);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_DETECTED);
    CHECK(snapshot_actions_are(&snapshot, true, false, false));
    CHECK(snapshot.swap_detected && snapshot.scratch_detected);
    CHECK(snapshot.swap_total_known && snapshot.scratch_total_known);
    CHECK(snapshot.swap_partition_size_known && snapshot.scratch_partition_size_known);
    CHECK(snapshot.swap_partition_size_bytes == 8 * GIB);
    CHECK(snapshot.scratch_partition_size_bytes == 400 * GIB);
    text_copy(stable_identity, sizeof(stable_identity), snapshot.drive_identity);

    text_copy(
        fake.observation.swap.device,
        sizeof(fake.observation.swap.device),
        "/dev/sdy1"
    );
    text_copy(
        fake.observation.scratch.device,
        sizeof(fake.observation.scratch.device),
        "/dev/sdy2"
    );
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strcmp(snapshot.drive_identity, stable_identity) == 0);
    CHECK(strcmp(snapshot.swap_device, "/dev/sdy1") == 0);
    CHECK(strcmp(snapshot.scratch_device, "/dev/sdy2") == 0);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    CHECK(fake.destroyed);
    return true;
}

static bool test_bounded_udisks_storage_types(void) {
#ifdef LARDON3D_SSD_CONTROLLER_TESTING
    GVariant *objects = fixture_managed_objects(FIXTURE_SWAP_ACTIVE_BOOLEAN);
    Lardon3DSsdProviderSnapshot observation;
    char reason[LARDON3D_SSD_REASON_CAPACITY] = {0};

    /* GLib boolean storage is gboolean, not C17 bool. This raw fixture drives
     * the production parser and guards the exact ABI-sized copy boundary. */
    CHECK(lardon3d_ssd_parse_managed_objects_for_test(objects, &observation, reason));
    CHECK(observation.swap.present && observation.scratch.present);
    CHECK(observation.swap.active_known && observation.swap.active);
    CHECK(observation.swap.unit_ready && observation.scratch.unit_ready);
    CHECK(observation.scratch.mounted);
    CHECK(!observation.scratch.total_known);
    CHECK(!observation.scratch.free_known);
    CHECK(strcmp(
        observation.scratch.mount_path,
        LARDON3D_SSD_SCRATCH_MOUNT_PATH
    ) == 0);
    CHECK(observation.model_known && observation.serial_known);
    g_variant_unref(objects);
#endif
    return true;
}

#ifdef LARDON3D_SSD_CONTROLLER_TESTING
static bool check_invalid_swap_active_fixture(FixtureSwapActiveMode active_mode) {
    GVariant *objects = fixture_managed_objects(active_mode);
    Lardon3DSsdProviderSnapshot observation;
    char reason[LARDON3D_SSD_REASON_CAPACITY] = {0};
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    CHECK(lardon3d_ssd_parse_managed_objects_for_test(objects, &observation, reason));
    CHECK(observation.invalid_observation);
    CHECK(!observation.swap.active_known);
    CHECK(strstr(observation.invalid_reason, "exact boolean Active") != NULL);
    g_variant_unref(objects);

    fake_make_valid(&fake);
    fake.observation = observation;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "exact boolean Active") != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.start_calls == 0 && fake.mount_calls == 0);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.stop_calls == 0 && fake.unmount_calls == 0);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(snapshot.state != LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}
#endif

static bool test_required_swap_active_boolean(void) {
#ifdef LARDON3D_SSD_CONTROLLER_TESTING
    CHECK(check_invalid_swap_active_fixture(FIXTURE_SWAP_ACTIVE_MISSING));
    CHECK(check_invalid_swap_active_fixture(FIXTURE_SWAP_ACTIVE_TEXT));
#endif
    return true;
}

static bool test_active_node_rename_and_identity_replacement(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);

    text_copy(
        fake.observation.swap.device,
        sizeof(fake.observation.swap.device),
        "/dev/renamed1"
    );
    text_copy(
        fake.observation.scratch.device,
        sizeof(fake.observation.scratch.device),
        "/dev/renamed2"
    );
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ENABLED);
    CHECK(snapshot.scratch_allocations_allowed);

    text_copy(
        fake.observation.swap.drive_identity,
        sizeof(fake.observation.swap.drive_identity),
        "/org/freedesktop/UDisks2/drives/replacement"
    );
    text_copy(
        fake.observation.scratch.drive_identity,
        sizeof(fake.observation.scratch.drive_identity),
        "/org/freedesktop/UDisks2/drives/replacement"
    );
    text_copy(
        fake.observation.swap.uuid,
        sizeof(fake.observation.swap.uuid),
        "replacement-swap-uuid"
    );
    text_copy(
        fake.observation.scratch.uuid,
        sizeof(fake.observation.scratch.uuid),
        "replacement-scratch-uuid"
    );
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(!snapshot.pairing_valid);
    CHECK(snapshot_actions_are(&snapshot, false, false, false));
    CHECK(strstr(snapshot.reason, "Stable Drive/UUID pair changed") != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static void fake_replace_stable_pair(FakeProvider *fake) {
    text_copy(
        fake->observation.swap.drive_identity,
        sizeof(fake->observation.swap.drive_identity),
        "/org/freedesktop/UDisks2/drives/indeterminate_replacement"
    );
    text_copy(
        fake->observation.scratch.drive_identity,
        sizeof(fake->observation.scratch.drive_identity),
        "/org/freedesktop/UDisks2/drives/indeterminate_replacement"
    );
    text_copy(
        fake->observation.swap.uuid,
        sizeof(fake->observation.swap.uuid),
        "indeterminate-replacement-swap"
    );
    text_copy(
        fake->observation.scratch.uuid,
        sizeof(fake->observation.scratch.uuid),
        "indeterminate-replacement-scratch"
    );
}

static bool test_indeterminate_start_ownership(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdProviderSnapshot original_after_action;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    fake.fail_start = true;
    fake.start_effect_on_failure = true;
    fake.fail_refresh_after_start = true;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);

    /* A timeout may follow a real side effect. The failed verification must
     * preserve authority for this tuple even when discovery next sees a
     * healthy-looking replacement. */
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.start_calls == 1 && fake.mount_calls == 0);
    CHECK(fake.observation.swap.active);
    original_after_action = fake.observation;

    fake_replace_stable_pair(&fake);
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "Stable Drive/UUID pair changed") != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.start_calls == 1 && fake.mount_calls == 0);
    CHECK(fake.stop_calls == 0 && fake.unmount_calls == 0);

    fake.observation = original_after_action;
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR && snapshot.pairing_valid);
    CHECK(snapshot_actions_are(&snapshot, false, true, false));
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(fake.stop_calls == 1 && fake.unmount_calls == 0);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(snapshot_actions_are(&snapshot, true, false, false));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_indeterminate_mount_ownership(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdProviderSnapshot original_after_action;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    fake.fail_mount = true;
    fake.mount_effect_on_failure = true;
    fake.fail_refresh_after_mount = true;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);

    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.start_calls == 1 && fake.mount_calls == 1);
    CHECK(fake.observation.swap.active && fake.observation.scratch.mounted);
    original_after_action = fake.observation;

    fake_replace_stable_pair(&fake);
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "Stable Drive/UUID pair changed") != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.start_calls == 1 && fake.mount_calls == 1);
    CHECK(fake.stop_calls == 0 && fake.unmount_calls == 0);

    fake.observation = original_after_action;
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR && snapshot.pairing_valid);
    CHECK(snapshot_actions_are(&snapshot, false, true, false));
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(fake.stop_calls == 1 && fake.unmount_calls == 1);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(snapshot_actions_are(&snapshot, true, false, false));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_enable_lease_capacity_and_exact_release(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;
    Lardon3DSsdScratchLease leases[LARDON3D_SSD_MAX_SCRATCH_LEASES] = {{0}};
    Lardon3DSsdScratchLease extra = {0};
    Lardon3DSsdScratchLease copied;
    Lardon3DSsdScratchLease constructed;
    FakeProvider foreign_fake;
    Lardon3DSsdController *foreign_controller;

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(fake.start_calls == 1 && fake.mount_calls == 1);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ENABLED);
    CHECK(snapshot_actions_are(&snapshot, false, true, false));
    CHECK(snapshot.swap_active && snapshot.scratch_mounted);
    CHECK(strcmp(snapshot.scratch_mount_path, LARDON3D_SSD_SCRATCH_MOUNT_PATH) == 0);

    for (size_t index = 0; index < LARDON3D_SSD_MAX_SCRATCH_LEASES; ++index) {
        CHECK(lardon3d_ssd_controller_acquire_scratch(controller, &leases[index]));
    }
    CHECK(!lardon3d_ssd_controller_acquire_scratch(controller, &extra));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_IN_USE);
    CHECK(snapshot_actions_are(&snapshot, false, true, false));
    CHECK(snapshot.scratch_lease_count == LARDON3D_SSD_MAX_SCRATCH_LEASES);
    CHECK(!snapshot.scratch_allocations_allowed);
    /* Destruction with live capabilities is a non-mutating refusal; the
     * original objects remain releasable through the still-valid controller. */
    CHECK(!lardon3d_ssd_controller_destroy(controller));

    copied = leases[0];
    constructed = (Lardon3DSsdScratchLease) {
        .opaque_controller = leases[0].opaque_controller,
        .opaque_lease_id = leases[0].opaque_lease_id,
    };
    /* Numeric bytes are observable for ABI purposes but are not ownership.
     * Both distinct addresses must fail while the original lease is live. */
    CHECK(!lardon3d_ssd_controller_release_scratch(controller, &copied));
    CHECK(!lardon3d_ssd_controller_release_scratch(controller, &constructed));
    fake_make_valid(&foreign_fake);
    foreign_controller = fake_controller(&foreign_fake);
    CHECK(foreign_controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(foreign_controller)
        == LARDON3D_SSD_CONTROL_OK);
    CHECK(!lardon3d_ssd_controller_release_scratch(foreign_controller, &leases[0]));
    CHECK(lardon3d_ssd_controller_destroy(foreign_controller));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.scratch_lease_count == LARDON3D_SSD_MAX_SCRATCH_LEASES);
    CHECK(lardon3d_ssd_controller_release_scratch(controller, &leases[0]));
    CHECK(!lardon3d_ssd_controller_release_scratch(controller, &leases[0]));
    CHECK(!lardon3d_ssd_controller_release_scratch(controller, &copied));
    CHECK(!lardon3d_ssd_controller_release_scratch(controller, &constructed));
    for (size_t index = 1; index < LARDON3D_SSD_MAX_SCRATCH_LEASES; ++index) {
        CHECK(lardon3d_ssd_controller_release_scratch(controller, &leases[index]));
    }
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ENABLED);
    CHECK(snapshot.scratch_lease_count == 0);
    CHECK(snapshot.scratch_allocations_allowed);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_disable_unused_and_safe_disappearance(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(fake.stop_calls == 1 && fake.unmount_calls == 1);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(!snapshot.swap_active && !snapshot.scratch_mounted);

    memset(&fake.observation, 0, sizeof(fake.observation));
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ABSENT);
    CHECK(strcmp(snapshot.swap_device, "UNKNOWN") == 0);

    /* A clean verified drain relinquishes physical ownership. Absence clears
     * the safe latch, so a different healthy tuple starts a fresh lifecycle. */
    fake_make_valid(&fake);
    text_copy(
        fake.observation.swap.drive_identity,
        sizeof(fake.observation.swap.drive_identity),
        "/org/freedesktop/UDisks2/drives/replacement_after_safe"
    );
    text_copy(
        fake.observation.scratch.drive_identity,
        sizeof(fake.observation.scratch.drive_identity),
        "/org/freedesktop/UDisks2/drives/replacement_after_safe"
    );
    text_copy(
        fake.observation.swap.uuid,
        sizeof(fake.observation.swap.uuid),
        "replacement-after-safe-swap"
    );
    text_copy(
        fake.observation.scratch.uuid,
        sizeof(fake.observation.scratch.uuid),
        "replacement-after-safe-scratch"
    );
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_DETECTED);
    CHECK(strcmp(
        snapshot.drive_identity,
        "/org/freedesktop/UDisks2/drives/replacement_after_safe"
    ) == 0);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_sticky_hazard_reconnect_and_verified_drain(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;
    Lardon3DSsdScratchLease lease = {0};
    Lardon3DSsdProviderSnapshot original;
    char original_drive[LARDON3D_SSD_IDENTITY_CAPACITY];
    char original_swap_uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char original_scratch_uuid[LARDON3D_SSD_TEXT_CAPACITY];

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(lardon3d_ssd_controller_acquire_scratch(controller, &lease));
    original = fake.observation;
    text_copy(
        original_drive,
        sizeof(original_drive),
        original.swap.drive_identity
    );
    text_copy(
        original_swap_uuid,
        sizeof(original_swap_uuid),
        original.swap.uuid
    );
    text_copy(
        original_scratch_uuid,
        sizeof(original_scratch_uuid),
        original.scratch.uuid
    );

    memset(&fake.observation, 0, sizeof(fake.observation));
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "disappeared") != NULL);
    CHECK(strcmp(snapshot.drive_identity, original_drive) == 0);
    CHECK(strcmp(snapshot.swap_uuid, original_swap_uuid) == 0);
    CHECK(strcmp(snapshot.scratch_uuid, original_scratch_uuid) == 0);
    CHECK(strcmp(snapshot.swap_device, "UNKNOWN") == 0);

    /* A second absent poll must not self-heal after the one-poll active flag
     * has disappeared from ordinary discovery state. */
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strcmp(snapshot.drive_identity, original_drive) == 0);

    fake_make_valid(&fake);
    text_copy(
        fake.observation.swap.drive_identity,
        sizeof(fake.observation.swap.drive_identity),
        "/org/freedesktop/UDisks2/drives/unsafe_replacement"
    );
    text_copy(
        fake.observation.scratch.drive_identity,
        sizeof(fake.observation.scratch.drive_identity),
        "/org/freedesktop/UDisks2/drives/unsafe_replacement"
    );
    text_copy(
        fake.observation.swap.uuid,
        sizeof(fake.observation.swap.uuid),
        "unsafe-replacement-swap"
    );
    text_copy(
        fake.observation.scratch.uuid,
        sizeof(fake.observation.scratch.uuid),
        "unsafe-replacement-scratch"
    );
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "Stable Drive/UUID pair changed") != NULL);
    CHECK(strcmp(snapshot.drive_identity, original_drive) == 0);
    CHECK(strcmp(snapshot.swap_device, "UNKNOWN") == 0);
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strcmp(snapshot.drive_identity, original_drive) == 0);

    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(lardon3d_ssd_controller_release_scratch(controller, &lease));
    CHECK(fake.stop_calls == 0 && fake.unmount_calls == 0);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(fake.stop_calls == 0 && fake.unmount_calls == 0);

    /* Reconnecting the exact original tuple restores control authority, but
     * public state stays ERROR until the verified drain endpoint is reached. */
    fake.observation = original;
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(snapshot.pairing_valid);
    CHECK(strstr(snapshot.reason, "verified drain") != NULL);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(fake.stop_calls == 1 && fake.unmount_calls == 1);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(!snapshot.swap_active && !snapshot.scratch_mounted);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_lease_blocks_drain_cancel_and_auto_resume(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;
    Lardon3DSsdScratchLease lease = {0};
    Lardon3DSsdScratchLease rejected = {0};

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(lardon3d_ssd_controller_acquire_scratch(controller, &lease));
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(fake.stop_calls == 0 && fake.unmount_calls == 0);
    CHECK(!lardon3d_ssd_controller_acquire_scratch(controller, &rejected));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_DRAINING);
    CHECK(snapshot_actions_are(&snapshot, false, false, true));
    CHECK(strstr(snapshot.reason, "lease") != NULL);

    CHECK(lardon3d_ssd_controller_cancel_drain(controller));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_IN_USE);
    CHECK(!snapshot.drain_requested);
    CHECK(snapshot_actions_are(&snapshot, false, true, false));
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(lardon3d_ssd_controller_release_scratch(controller, &lease));
    CHECK(fake.stop_calls == 1 && fake.unmount_calls == 1);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_used_swap_safety_and_pressure_blockers(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    fake.observation.memory_available_known = false;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    fake.observation.swap.used_bytes = 2 * GIB;
    fake.observation.memory_available_bytes = 6 * GIB;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    fake.observation.swap.used_bytes = 4 * GIB;
    fake.observation.memory_available_bytes = 6 * GIB;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(fake.stop_calls == 0);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_DRAINING);
    CHECK(strstr(snapshot.reason, "3 GiB") != NULL);
    CHECK(lardon3d_ssd_controller_cancel_drain(controller));

    fake.observation.swap.used_bytes = 0;
    fake.observation.memory_pressure_elevated = true;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strstr(snapshot.reason, "memory PSI") != NULL);
    CHECK(lardon3d_ssd_controller_cancel_drain(controller));

    fake.observation.memory_pressure_elevated = false;
    fake.observation.swap_pages_out_delta = 1;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strstr(snapshot.reason, "swap-in/out") != NULL);
    CHECK(lardon3d_ssd_controller_cancel_drain(controller));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_disappearance_and_invalid_discovery(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    memset(&fake.observation, 0, sizeof(fake.observation));
    CHECK(lardon3d_ssd_controller_refresh(controller, true));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "disappeared") != NULL);
    CHECK(strcmp(snapshot.swap_device, "UNKNOWN") == 0);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    fake.observation.swap.size_bytes = 0;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "zero") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    fake.observation.scratch.unit_ready = false;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "Unit Not Ready") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    fake.observation.ambiguous_labels = true;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "Multiple partitions") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    text_copy(
        fake.observation.scratch.drive_identity,
        sizeof(fake.observation.scratch.drive_identity),
        "/org/freedesktop/UDisks2/drives/other_drive"
    );
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "same UDisks Drive") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    memset(&fake.observation.scratch, 0, sizeof(fake.observation.scratch));
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_DETECTED);
    CHECK(strstr(snapshot.reason, "missing") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_operation_errors_and_mount_mismatch(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    fake.fail_start = true;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "injected timeout") != NULL);
    CHECK(!snapshot.swap_active);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    fake.fail_mount = true;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(snapshot.swap_active && !snapshot.scratch_mounted);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    text_copy(fake.mount_result, sizeof(fake.mount_result), "/run/media/test/LARDON_SCRATCH");
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(snapshot.swap_active && snapshot.scratch_mounted);
    CHECK(strcmp(snapshot.scratch_mount_path, "/run/media/test/LARDON_SCRATCH") == 0);
    CHECK(strstr(snapshot.reason, "not '/mnt/lardon-scratch'") != NULL);
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_OK);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_SAFE_TO_UNPLUG);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    fake.fail_stop = true;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(snapshot.swap_active && snapshot.scratch_mounted);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    fake.fail_unmount = true;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(!snapshot.swap_active && snapshot.scratch_mounted);
    CHECK(strstr(snapshot.reason, "Unmount failed") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_unknown_telemetry_is_explicit_and_blocks_safely(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    fake.observation.model_known = false;
    fake.observation.model[0] = '\0';
    fake.observation.serial_known = false;
    fake.observation.serial[0] = '\0';
    fake.observation.connection_speed_known = false;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(!snapshot.model_known && strcmp(snapshot.model, "UNKNOWN") == 0);
    CHECK(!snapshot.serial_known && strcmp(snapshot.serial, "UNKNOWN") == 0);
    CHECK(!snapshot.connection_speed_known);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);

    fake.observation.swap.used_known = false;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strstr(snapshot.reason, "used bytes are unknown") != NULL);
    CHECK(lardon3d_ssd_controller_cancel_drain(controller));

    fake.observation.swap.used_known = true;
    fake.observation.memory_pressure_known = false;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strstr(snapshot.reason, "PSI evidence is unknown") != NULL);
    CHECK(lardon3d_ssd_controller_cancel_drain(controller));

    fake.observation.memory_pressure_known = true;
    fake.observation.swap_activity_known = false;
    CHECK(lardon3d_ssd_controller_disable(controller) == LARDON3D_SSD_CONTROL_PENDING);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strstr(snapshot.reason, "delta interval") != NULL);
    CHECK(lardon3d_ssd_controller_cancel_drain(controller));
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_success_without_effect_and_refresh_error(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    fake.start_without_effect = true;
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_ERROR);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(strstr(snapshot.reason, "not active") != NULL);
    CHECK(lardon3d_ssd_controller_destroy(controller));

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    fake.fail_refresh = true;
    CHECK(!lardon3d_ssd_controller_refresh(controller, true));
    CHECK(!snapshot_of(controller, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ERROR);
    CHECK(strstr(snapshot.reason, "injected timeout") != NULL);
    CHECK(strcmp(snapshot.swap_device, "UNKNOWN") == 0);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

typedef struct {
    Lardon3DSsdController *controller;
    bool success;
} LeaseThread;

typedef struct {
    Lardon3DSsdController *controller;
    Lardon3DSsdScratchLease *lease;
    pthread_barrier_t *barrier;
    bool acquire;
    bool success;
} SharedLeaseThread;

static void *shared_lease_call_thread(void *userdata) {
    SharedLeaseThread *thread = userdata;

    (void)pthread_barrier_wait(thread->barrier);
    thread->success = thread->acquire
        ? lardon3d_ssd_controller_acquire_scratch(thread->controller, thread->lease)
        : lardon3d_ssd_controller_release_scratch(thread->controller, thread->lease);
    return NULL;
}

static void *lease_stress_thread(void *userdata) {
    LeaseThread *thread = userdata;
    thread->success = true;
    for (size_t iteration = 0; iteration < 250; ++iteration) {
        Lardon3DSsdScratchLease lease = {0};
        while (!lardon3d_ssd_controller_acquire_scratch(thread->controller, &lease)) {
            sched_yield();
        }
        if (!lardon3d_ssd_controller_release_scratch(thread->controller, &lease)) {
            thread->success = false;
            return NULL;
        }
    }
    return NULL;
}

static bool test_concurrent_lease_accounting(void) {
    enum { THREAD_COUNT = 8 };
    FakeProvider fake;
    Lardon3DSsdController *controller;
    pthread_t threads[THREAD_COUNT];
    LeaseThread work[THREAD_COUNT];
    Lardon3DSsdSnapshot snapshot;

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);
    for (size_t index = 0; index < THREAD_COUNT; ++index) {
        work[index].controller = controller;
        work[index].success = false;
        CHECK(pthread_create(&threads[index], NULL, lease_stress_thread, &work[index]) == 0);
    }
    for (size_t index = 0; index < THREAD_COUNT; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(work[index].success);
    }
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.scratch_lease_count == 0);
    CHECK(snapshot.state == LARDON3D_SSD_ENABLED);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_same_object_lease_serialization(void) {
    FakeProvider fake;
    Lardon3DSsdController *controller;
    Lardon3DSsdScratchLease lease = {0};
    Lardon3DSsdScratchLease saved;
    Lardon3DSsdSnapshot snapshot;
    pthread_barrier_t barrier;
    pthread_t threads[2];
    SharedLeaseThread work[2];

    fake_make_valid(&fake);
    controller = fake_controller(&fake);
    CHECK(controller != NULL);
    CHECK(lardon3d_ssd_controller_enable(controller) == LARDON3D_SSD_CONTROL_OK);

    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    for (size_t index = 0; index < 2; ++index) {
        work[index] = (SharedLeaseThread) {
            .controller = controller,
            .lease = &lease,
            .barrier = &barrier,
            .acquire = true,
            .success = false,
        };
        CHECK(pthread_create(
            &threads[index],
            NULL,
            shared_lease_call_thread,
            &work[index]
        ) == 0);
    }
    for (size_t index = 0; index < 2; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    CHECK(work[0].success != work[1].success);
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.scratch_lease_count == 1);
    CHECK(!lardon3d_ssd_controller_destroy(controller));

    saved = lease;
    memset(&lease, 0, sizeof(lease));
    /* This intentionally violates the public no-reconstruction rule. The
     * address registry must still prevent a second live capability. */
    CHECK(!lardon3d_ssd_controller_acquire_scratch(controller, &lease));
    lease = saved;
    CHECK(!lardon3d_ssd_controller_acquire_scratch(controller, &lease));

    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    for (size_t index = 0; index < 2; ++index) {
        work[index] = (SharedLeaseThread) {
            .controller = controller,
            .lease = &lease,
            .barrier = &barrier,
            .acquire = false,
            .success = false,
        };
        CHECK(pthread_create(
            &threads[index],
            NULL,
            shared_lease_call_thread,
            &work[index]
        ) == 0);
    }
    for (size_t index = 0; index < 2; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    CHECK(work[0].success != work[1].success);
    CHECK(lease.opaque_controller == 0 && lease.opaque_lease_id == 0);
    CHECK(!lardon3d_ssd_controller_release_scratch(controller, &lease));
    CHECK(snapshot_of(controller, &snapshot));
    CHECK(snapshot.scratch_lease_count == 0);
    CHECK(lardon3d_ssd_controller_destroy(controller));
    return true;
}

static bool test_public_invalid_arguments_and_names(void) {
    Lardon3DSsdScratchLease lease = {0};
    Lardon3DSsdSnapshot snapshot;

    CHECK(lardon3d_ssd_controller_destroy(NULL));
    CHECK(!lardon3d_ssd_controller_refresh(NULL, false));
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(!lardon3d_ssd_controller_get_snapshot(NULL, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ABSENT);
    CHECK(strcmp(snapshot.drive_identity, "UNKNOWN") == 0);
    CHECK(snapshot.scratch_lease_capacity == LARDON3D_SSD_MAX_SCRATCH_LEASES);
    CHECK(!lardon3d_ssd_controller_get_snapshot(NULL, NULL));
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(!lardon3d_ssd_controller_copy_snapshot(NULL, &snapshot));
    CHECK(snapshot.state == LARDON3D_SSD_ABSENT);
    CHECK(snapshot.scratch_lease_capacity == LARDON3D_SSD_MAX_SCRATCH_LEASES);
    CHECK(!lardon3d_ssd_controller_copy_snapshot(NULL, NULL));
    CHECK(!lardon3d_ssd_controller_acquire_scratch(NULL, &lease));
    CHECK(!lardon3d_ssd_controller_release_scratch(NULL, &lease));
    CHECK(strcmp(lardon3d_ssd_state_name(LARDON3D_SSD_ABSENT), "ABSENT") == 0);
    CHECK(strcmp(lardon3d_ssd_state_name(LARDON3D_SSD_ERROR), "ERROR") == 0);
    CHECK(strcmp(lardon3d_ssd_state_name((Lardon3DSsdState)99), "UNKNOWN") == 0);
    return true;
}

int main(void) {
    const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"absent/cache/appearance/node rename", test_absent_cache_appearance_and_node_rename},
        {"bounded UDisks storage types", test_bounded_udisks_storage_types},
        {"required Swapspace.Active boolean", test_required_swap_active_boolean},
        {"active rename/identity replacement", test_active_node_rename_and_identity_replacement},
        {"indeterminate Start ownership", test_indeterminate_start_ownership},
        {"indeterminate Mount ownership", test_indeterminate_mount_ownership},
        {"enable/lease capacity/exact release", test_enable_lease_capacity_and_exact_release},
        {"disable unused/safe disappearance", test_disable_unused_and_safe_disappearance},
        {"sticky hazard/reconnect/verified drain", test_sticky_hazard_reconnect_and_verified_drain},
        {"lease drain/cancel/auto resume", test_lease_blocks_drain_cancel_and_auto_resume},
        {"used swap and pressure safety", test_used_swap_safety_and_pressure_blockers},
        {"disappearance/invalid discovery", test_disappearance_and_invalid_discovery},
        {"operation errors/mount mismatch", test_operation_errors_and_mount_mismatch},
        {"explicit unknown/safe blocking", test_unknown_telemetry_is_explicit_and_blocks_safely},
        {"operation verification/refresh error", test_success_without_effect_and_refresh_error},
        {"concurrent lease accounting", test_concurrent_lease_accounting},
        {"same-object lease serialization", test_same_object_lease_serialization},
        {"public invalid arguments/names", test_public_invalid_arguments_and_names},
    };

    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (!tests[index].run()) {
            fprintf(stderr, "FAILED: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS: %s\n", tests[index].name);
    }
    return 0;
}
