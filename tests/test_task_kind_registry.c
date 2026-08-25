#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/task_kind_registry.h>

#include "resource_snapshot_test_utils.h"

#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); return false; \
} } while (0)

typedef struct {
    int *destroyed;
    int *finished;
    uint64_t expected_id;
} TestUserdata;

typedef struct {
    int *destroyed;
    int *finished;
    bool fail;
} ReconstructContext;

static bool
test_callback(Lardon3DTask *task, void *userdata)
{
    TestUserdata *data = userdata;
    return data && lardon3d_task_id(task) == data->expected_id;
}

static void
destroy_userdata(void *userdata)
{
    TestUserdata *data = userdata;
    if (data) {
        ++*data->destroyed;
        free(data);
    }
}

static void
finished_callback(const Lardon3DTask *task, void *userdata)
{
    TestUserdata *data = userdata;
    Lardon3DTaskSnapshot terminal;
    if (data && lardon3d_task_snapshot(task, &terminal)
        && terminal.state == TASK_COMPLETED && *data->destroyed == 0) {
        ++*data->finished;
    }
}

static bool
reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot,
    void *context,
    Lardon3DTaskKindBinding *binding
)
{
    ReconstructContext *settings = context;
    TestUserdata *data = malloc(sizeof(*data));
    if (!data) {
        return false;
    }
    *data = (TestUserdata) {
        .destroyed = settings->destroyed,
        .finished = settings->finished,
        .expected_id = snapshot->id,
    };
    binding->callback = test_callback;
    binding->userdata = data;
    binding->userdata_destroy = destroy_userdata;
    binding->finished_callback = finished_callback;
    binding->finished_userdata = data;
    return !settings->fail;
}

static Lardon3DTaskDurableSnapshot
snapshot(void)
{
    Lardon3DTaskDurableSnapshot result = {
        .id = 42,
        .estimate = {
            .minimum_batch_size = 1,
            .maximum_batch_size = 1,
            .desired_cpu_threads = 1,
            .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
        },
        .progress = 23,
        .saved_state = TASK_RUNNING,
        .recovery_state = TASK_PENDING,
        .sequence_count = 4,
    };
    (void)snprintf(result.name, sizeof(result.name), "Registry test");
    return result;
}

static bool
run_test(void)
{
    static const Lardon3DTaskKindDescriptor descriptors[] = {
        {.kind = "test.recovery", .kind_version = 1, .reconstruct = reconstruct},
        {.kind = "test.other", .kind_version = 1, .reconstruct = reconstruct},
    };
    Lardon3DTaskKindRegistry registry;
    CHECK(lardon3d_task_kind_registry_init(&registry, descriptors, 2));
    CHECK(lardon3d_task_kind_is_valid("test.recovery-1"));
    CHECK(!lardon3d_task_kind_is_valid(""));
    char too_long[LARDON3D_TASK_KIND_CAPACITY + 1];
    memset(too_long, 'a', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!lardon3d_task_kind_is_valid(too_long));
    CHECK(!lardon3d_task_kind_is_valid("Test.recovery"));
    CHECK(!lardon3d_task_kind_is_valid("test/recovery"));
    CHECK(!lardon3d_task_kind_is_valid(".test"));

    const Lardon3DTaskKindDescriptor *found = NULL;
    CHECK(lardon3d_task_kind_registry_lookup(&registry, "test.recovery", 1,
        &found) == LARDON3D_TASK_KIND_OK && found == &descriptors[0]);
    found = NULL;
    CHECK(lardon3d_task_kind_registry_lookup(&registry, "test.recovery", 1,
        &found) == LARDON3D_TASK_KIND_OK && found == &descriptors[0]);
    CHECK(lardon3d_task_kind_registry_lookup(&registry, "unknown.kind", 1,
        &found) == LARDON3D_TASK_KIND_UNKNOWN);
    CHECK(lardon3d_task_kind_registry_lookup(&registry, "test.recovery", 3,
        &found) == LARDON3D_TASK_KIND_UNSUPPORTED_VERSION);

    Lardon3DTaskKindDescriptor duplicate[] = {descriptors[0], descriptors[0]};
    CHECK(!lardon3d_task_kind_registry_init(&registry, duplicate, 2));
    CHECK(lardon3d_task_kind_registry_init(&registry, descriptors, 2));

    int destroyed = 0, finished = 0;
    ReconstructContext context = {
        .destroyed = &destroyed,
        .finished = &finished,
    };
    Lardon3DTaskDurableSnapshot durable = snapshot();
    Lardon3DTask *task = NULL;
    CHECK(lardon3d_task_kind_registry_restore(&registry, "test.recovery", 1,
        &durable, &context, &task) == LARDON3D_TASK_KIND_OK);
    CHECK(task && lardon3d_task_id(task) == durable.id
        && lardon3d_task_sequence_count(task) == durable.sequence_count);
    char kind[LARDON3D_TASK_KIND_CAPACITY]; uint32_t version = 0;
    CHECK(lardon3d_task_kind(task, kind, &version));
    CHECK(strcmp(kind, "test.recovery") == 0 && version == 1);
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 2,
        .page_size_bytes = 4096,
        .memory_total_bytes = 1024 * 1024,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile, &policy);
    Lardon3DResourceSnapshot resources = {
        .memory_available_bytes = profile.memory_total_bytes,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(lardon3d_test_resource_snapshot_make_fresh(&resources));
    CHECK(governor && lardon3d_resource_governor_reserve(
        governor, &resources, &durable.estimate, &decision, &reservation));
    CHECK(lardon3d_task_start(task, governor, reservation));
    Lardon3DTaskSnapshot runtime;
    CHECK(lardon3d_task_snapshot(task, &runtime)
        && runtime.state == TASK_COMPLETED);
    CHECK(finished == 1 && destroyed == 0);
    lardon3d_task_destroy(task);
    lardon3d_resource_governor_destroy(governor);
    CHECK(destroyed == 1 && finished == 1);

    context.fail = true;
    CHECK(lardon3d_task_kind_registry_restore(&registry, "test.recovery", 1,
        &durable, &context, &task) == LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED);
    CHECK(!task && destroyed == 2);
    context.fail = false;
    durable.progress = 101;
    CHECK(lardon3d_task_kind_registry_restore(&registry, "test.recovery", 1,
        &durable, &context, &task) == LARDON3D_TASK_KIND_RESTORE_FAILED);
    CHECK(!task && destroyed == 3);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
