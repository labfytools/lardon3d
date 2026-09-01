#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/task_kind_registry.h>

#include "resource_snapshot_test_utils.h"

typedef struct {
    Lardon3DTaskCallback callback;
    void *userdata;
    Lardon3DTaskUserdataDestroy userdata_destroy;
    Lardon3DTaskFinishedCallback finished_callback;
    void *finished_userdata;
} HistoricalTaskKindBinding;

/* The registry binding is public C17 ABI. Legacy estimate normalization must
 * remain private and must not append fields to this established layout. */
_Static_assert(sizeof(Lardon3DTaskKindBinding) ==
                   sizeof(HistoricalTaskKindBinding),
               "Lardon3DTaskKindBinding ABI size changed");
_Static_assert(offsetof(Lardon3DTaskKindBinding, finished_userdata) ==
                   offsetof(HistoricalTaskKindBinding, finished_userdata),
               "Lardon3DTaskKindBinding ABI layout changed");

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
same_estimate(const Lardon3DResourceEstimate *left,
              const Lardon3DResourceEstimate *right)
{
    return left->memory_fixed_bytes == right->memory_fixed_bytes
        && left->gpu_memory_fixed_bytes == right->gpu_memory_fixed_bytes
        && left->memory_bytes_per_item == right->memory_bytes_per_item
        && left->gpu_memory_bytes_per_item == right->gpu_memory_bytes_per_item
        && left->minimum_batch_size == right->minimum_batch_size
        && left->maximum_batch_size == right->maximum_batch_size
        && left->desired_cpu_threads == right->desired_cpu_threads
        && left->desired_gpu_slots == right->desired_gpu_slots
        && left->desired_io_slots == right->desired_io_slots
        && left->task_class == right->task_class;
}

static bool
run_legacy_estimate_validation_test(void)
{
    static const Lardon3DTaskKindDescriptor descriptors[] = {
        {.kind = "candidate_pair.generate", .kind_version = 1,
         .reconstruct = reconstruct},
        {.kind = "features.extract", .kind_version = 1,
         .reconstruct = reconstruct},
        {.kind = "features.extract.sift", .kind_version = 1,
         .reconstruct = reconstruct},
        {.kind = "features.extract.rootsift", .kind_version = 1,
         .reconstruct = reconstruct},
        {.kind = "visual_index.update", .kind_version = 1,
         .reconstruct = reconstruct},
        {.kind = "geometric_verifier.run", .kind_version = 1,
         .reconstruct = reconstruct},
    };
    Lardon3DTaskKindRegistry registry;
    CHECK(lardon3d_task_kind_registry_init(
        &registry, descriptors, sizeof(descriptors) / sizeof(descriptors[0])));
    int destroyed = 0;
    int finished = 0;
    ReconstructContext context = {
        .destroyed = &destroyed,
        .finished = &finished,
    };
    const char *kinds[] = {
        "candidate_pair.generate",
        "features.extract",
        "features.extract.sift",
        "features.extract.rootsift",
        "visual_index.update",
        "geometric_verifier.run",
    };
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
        Lardon3DTaskDurableSnapshot corrupt = snapshot();
        corrupt.estimate = (Lardon3DResourceEstimate) {0};
        Lardon3DTask *task = NULL;
        /* An absent legacy form must never make the all-zero estimate look
         * like an exact historical signature. Reconstruction owns and frees
         * the binding userdata on this corruption rejection. */
        CHECK(lardon3d_task_kind_registry_restore(
            &registry, kinds[index], 1, &corrupt, &context, &task
        ) == LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED);
        CHECK(!task && destroyed == (int)index + 1);
    }

    typedef struct {
        const char *kind;
        Lardon3DResourceEstimate durable;
        Lardon3DResourceEstimate effective;
    } EstimateCase;
    const Lardon3DResourceEstimate candidate_current = {
        .memory_fixed_bytes = 256 * 1024,
        .memory_bytes_per_item = 8 * 1024 * 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 64,
        .desired_cpu_threads = 64,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    const Lardon3DResourceEstimate feature_current = {
        .memory_fixed_bytes = 64ULL * 1024 * 1024,
        .memory_bytes_per_item = 512ULL * 1024 * 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = INT_MAX,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    const Lardon3DResourceEstimate sift_current = {
        .memory_fixed_bytes = 64ULL * 1024 * 1024,
        .memory_bytes_per_item = 1024ULL * 1024 * 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = INT_MAX,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    const Lardon3DResourceEstimate visual_current = {
        .memory_fixed_bytes = 8ULL * 1024 * 1024,
        .memory_bytes_per_item = 2ULL * 1024 * 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 16,
        .desired_cpu_threads = 16,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    const Lardon3DResourceEstimate geometric_current = {
        .memory_bytes_per_item = 8ULL * 1024 * 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 16,
        .desired_cpu_threads = 8,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    EstimateCase accepted[] = {
        {.kind = "candidate_pair.generate",
         .durable = candidate_current, .effective = candidate_current},
        {.kind = "candidate_pair.generate",
         .durable = candidate_current, .effective = candidate_current},
        {.kind = "candidate_pair.generate",
         .durable = candidate_current, .effective = candidate_current},
        {.kind = "features.extract",
         .durable = feature_current, .effective = feature_current},
        {.kind = "features.extract",
         .durable = feature_current, .effective = feature_current},
        {.kind = "features.extract",
         .durable = feature_current, .effective = feature_current},
        {.kind = "features.extract.sift",
         .durable = sift_current, .effective = sift_current},
        {.kind = "features.extract.sift",
         .durable = sift_current, .effective = sift_current},
        {.kind = "features.extract.sift",
         .durable = sift_current, .effective = sift_current},
        {.kind = "features.extract.rootsift",
         .durable = sift_current, .effective = sift_current},
        {.kind = "features.extract.rootsift",
         .durable = sift_current, .effective = sift_current},
        {.kind = "features.extract.rootsift",
         .durable = sift_current, .effective = sift_current},
        {.kind = "visual_index.update",
         .durable = visual_current, .effective = visual_current},
        {.kind = "visual_index.update",
         .durable = visual_current, .effective = visual_current},
        {.kind = "visual_index.update",
         .durable = visual_current, .effective = visual_current},
        {.kind = "geometric_verifier.run",
         .durable = geometric_current, .effective = geometric_current},
        {.kind = "geometric_verifier.run",
         .durable = geometric_current, .effective = geometric_current},
    };
    /* The first five groups contain current, immediately preceding, and oldest
     * exact signatures. GV has only current plus its frozen serial envelope.
     * No neighboring estimate is accepted. */
    accepted[1].durable.memory_bytes_per_item = 64 * 1024;
    accepted[1].durable.desired_cpu_threads = 12;
    accepted[2].durable = accepted[1].durable;
    accepted[2].durable.memory_fixed_bytes = 128 * 1024;
    accepted[2].durable.desired_cpu_threads = 1;
    accepted[4].durable.desired_cpu_threads = 12;
    accepted[5].durable.desired_cpu_threads = 1;
    accepted[7].durable.desired_cpu_threads = 12;
    accepted[8].durable.desired_cpu_threads = 1;
    accepted[10].durable.desired_cpu_threads = 12;
    accepted[11].durable.desired_cpu_threads = 1;
    accepted[13].durable.desired_cpu_threads = 12;
    accepted[14].durable.desired_cpu_threads = 1;
    accepted[16].durable.memory_fixed_bytes = 4ULL * 1024 * 1024;
    accepted[16].durable.memory_bytes_per_item = 0;
    accepted[16].durable.maximum_batch_size = 8;
    accepted[16].durable.desired_cpu_threads = 1;

    for (size_t index = 0;
         index < sizeof(accepted) / sizeof(accepted[0]); ++index) {
        Lardon3DTaskDurableSnapshot durable = snapshot();
        durable.estimate = accepted[index].durable;
        Lardon3DTask *task = NULL;
        CHECK(lardon3d_task_kind_registry_restore(
            &registry, accepted[index].kind, 1, &durable, &context, &task
        ) == LARDON3D_TASK_KIND_OK);
        Lardon3DResourceEstimate effective;
        CHECK(task && lardon3d_task_resource_estimate(task, &effective));
        CHECK(same_estimate(&effective, &accepted[index].effective));
        lardon3d_task_destroy(task);
    }

    size_t before_malformed = (size_t)destroyed;
    const size_t historical_case[] = {1, 4, 7, 10, 13, 16};
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
        Lardon3DTaskDurableSnapshot malformed = snapshot();
        malformed.estimate = accepted[historical_case[index]].durable;
        ++malformed.estimate.desired_cpu_threads;
        Lardon3DTask *task = NULL;
        CHECK(lardon3d_task_kind_registry_restore(
            &registry, kinds[index], 1, &malformed, &context, &task
        ) == LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED && !task);
    }
    CHECK((size_t)destroyed == before_malformed +
          sizeof(kinds) / sizeof(kinds[0]));
    return true;
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
    return (run_test() && run_legacy_estimate_validation_test())
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
