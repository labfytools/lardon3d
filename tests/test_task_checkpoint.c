#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/task_checkpoint.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

enum {
    TEST_CHECKPOINT_SIZE = 516,
    TEST_CHECKPOINT_HEADER_SIZE = 20,
    TEST_MINIMUM_BATCH_OFFSET = 188,
    TEST_STARTED_SECONDS_OFFSET = 488,
};

static uint32_t
test_checksum(const unsigned char *data, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void
test_put_u32(unsigned char *output, uint32_t value)
{
    for (size_t index = 0; index < 4; ++index) {
        output[index] = (unsigned char)(value >> (index * 8));
    }
}

static void
test_put_u64(unsigned char *output, uint64_t value)
{
    for (size_t index = 0; index < 8; ++index) {
        output[index] = (unsigned char)(value >> (index * 8));
    }
}

static bool
rewrite_u64(const char *path, size_t offset, uint64_t value)
{
    unsigned char data[TEST_CHECKPOINT_SIZE];
    int descriptor = open(path, O_RDWR);
    if (descriptor < 0
        || read(descriptor, data, sizeof(data)) != (ssize_t)sizeof(data)) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return false;
    }
    test_put_u64(data + offset, value);
    test_put_u32(
        data + 16,
        test_checksum(
            data + TEST_CHECKPOINT_HEADER_SIZE,
            TEST_CHECKPOINT_SIZE - TEST_CHECKPOINT_HEADER_SIZE
        )
    );
    bool written = pwrite(descriptor, data, sizeof(data), 0)
        == (ssize_t)sizeof(data);
    return close(descriptor) == 0 && written;
}

static bool
unused_callback(Lardon3DTask *task, void *userdata)
{
    (void)task;
    (void)userdata;
    return true;
}

typedef struct {
    Lardon3DTaskDurableSnapshot snapshot;
    bool captured;
} CaptureContext;

static bool
capture_running_callback(Lardon3DTask *task, void *userdata)
{
    CaptureContext *context = userdata;
    context->captured = lardon3d_task_durable_snapshot(task, &context->snapshot);
    return context->captured;
}

typedef struct {
    Lardon3DResourceGovernor *governor;
    Lardon3DTaskDurableSnapshot snapshot;
    bool captured;
} SequenceContext;

static bool
capture_sequence_callback(Lardon3DTask *task, void *userdata)
{
    SequenceContext *context = userdata;
    Lardon3DResourceReservation *reservation = NULL;
    Lardon3DTaskExecutionContract contract;
    if (!lardon3d_task_sequence_break(
            task,
            context->governor,
            &reservation,
            &contract
        )) {
        return false;
    }
    context->captured = lardon3d_task_durable_snapshot(task, &context->snapshot);
    return context->captured;
}

static Lardon3DTaskDurableSnapshot
snapshot_for(Lardon3DTaskState state)
{
    Lardon3DTaskDurableSnapshot snapshot = {
        .id = 41,
        .estimate = {
            .minimum_batch_size = 1,
            .maximum_batch_size = 8,
            .desired_cpu_threads = 1,
            .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
        },
        .progress = state == TASK_COMPLETED ? 100 : 37,
        .saved_state = state,
        .recovery_state = state == TASK_RUNNING || state == TASK_PAUSED
            ? TASK_PENDING : state,
        .started_at = {.tv_sec = 10, .tv_nsec = 20},
        .finished_at = {.tv_sec = 30, .tv_nsec = 40},
        .sequence_count = 3,
    };
    (void)snprintf(snapshot.name, sizeof(snapshot.name), "Tâche durable");
    (void)snprintf(snapshot.message, sizeof(snapshot.message), "Frontière validée");
    return snapshot;
}

static bool
write_bytes(const char *path, const void *data, size_t size)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
        return false;
    }
    ssize_t written = write(descriptor, data, size);
    bool ok = written >= 0 && (size_t)written == size && close(descriptor) == 0;
    if (!ok) {
        (void)close(descriptor);
    }
    return ok;
}

static bool
check_restored_state(Lardon3DTaskState saved, Lardon3DTaskState expected)
{
    Lardon3DTaskDurableSnapshot durable = snapshot_for(saved);
    Lardon3DTask *task = lardon3d_task_restore(
        &durable,
        unused_callback,
        NULL
    );
    CHECK(task);
    Lardon3DTaskSnapshot runtime;
    CHECK(lardon3d_task_snapshot(task, &runtime));
    CHECK(runtime.state == expected);
    CHECK(lardon3d_task_sequence_count(task) == durable.sequence_count);
    Lardon3DTaskExecutionContract contract;
    CHECK(!lardon3d_task_execution_contract(task, &contract));
    lardon3d_task_destroy(task);
    return true;
}

static bool
run_test(void)
{
    char directory[] = "/tmp/lardon3d-checkpoint-XXXXXX";
    CHECK(mkdtemp(directory));
    char path[512];
    char temporary[512];
    char corrupt[512];
    char uncertain_directory[512];
    char uncertain_path[512];
    CHECK(snprintf(path, sizeof(path), "%s/task.chk", directory) > 0);
    CHECK(snprintf(temporary, sizeof(temporary), "%s/task.chk.tmp", directory) > 0);
    CHECK(snprintf(corrupt, sizeof(corrupt), "%s/corrupt.chk", directory) > 0);
    CHECK(snprintf(
        uncertain_directory,
        sizeof(uncertain_directory),
        "%s/no-read",
        directory
    ) > 0);
    CHECK(snprintf(
        uncertain_path,
        sizeof(uncertain_path),
        "%s/task.chk",
        uncertain_directory
    ) > 0);

    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 4,
        .desired_cpu_threads = 1,
        .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
    };
    Lardon3DTask *pending = lardon3d_task_create(
        "Pending",
        &estimate,
        unused_callback,
        NULL
    );
    CHECK(pending && lardon3d_task_assign_id(pending, 7));
    Lardon3DTaskDurableSnapshot durable;
    CHECK(lardon3d_task_durable_snapshot(pending, &durable));
    CHECK(durable.saved_state == TASK_PENDING);
    CHECK(durable.recovery_state == TASK_PENDING);
    CHECK(durable.sequence_count == 0);
    lardon3d_task_destroy(pending);

    CHECK(lardon3d_task_checkpoint_save(path, &durable)
        == LARDON3D_TASK_CHECKPOINT_OK);
    Lardon3DTaskDurableSnapshot loaded;
    uint32_t version = 0;
    CHECK(lardon3d_task_checkpoint_load(path, &loaded, &version)
        == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(version == LARDON3D_TASK_CHECKPOINT_VERSION);
    CHECK(loaded.id == durable.id && loaded.saved_state == TASK_PENDING);

    durable = snapshot_for(TASK_COMPLETED);
    durable.id = 99;
    CHECK(lardon3d_task_checkpoint_save(path, &durable)
        == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(lardon3d_task_checkpoint_load(path, &loaded, NULL)
        == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(loaded.id == 99 && loaded.saved_state == TASK_COMPLETED);

    CHECK(mkdir(uncertain_directory, 0700) == 0);
    Lardon3DTaskDurableSnapshot previous = durable;
    previous.id = 98;
    CHECK(lardon3d_task_checkpoint_save(uncertain_path, &previous)
        == LARDON3D_TASK_CHECKPOINT_OK);
    durable.id = 100;
    CHECK(setenv(
        "LARDON3D_TEST_CHECKPOINT_SYNC_DIRECTORY_FAILURE",
        "1",
        1
    ) == 0);
    Lardon3DTaskCheckpointResult uncertain = lardon3d_task_checkpoint_save(
        uncertain_path,
        &durable
    );
    CHECK(unsetenv("LARDON3D_TEST_CHECKPOINT_SYNC_DIRECTORY_FAILURE") == 0);
    CHECK(uncertain == LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE);
    CHECK(lardon3d_task_checkpoint_load(uncertain_path, &loaded, NULL)
        == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(loaded.id == 100);

    unsigned char short_data[12] = {'L', '3', 'D', 'T', 'A', 'S', 'K', 0};
    CHECK(write_bytes(corrupt, short_data, sizeof(short_data)));
    CHECK(lardon3d_task_checkpoint_load(corrupt, &loaded, NULL)
        == LARDON3D_TASK_CHECKPOINT_INVALID);

    int descriptor = open(path, O_RDWR);
    CHECK(descriptor >= 0);
    unsigned char unknown_version[4] = {2, 0, 0, 0};
    CHECK(pwrite(descriptor, unknown_version, sizeof(unknown_version), 8)
        == (ssize_t)sizeof(unknown_version));
    CHECK(close(descriptor) == 0);
    CHECK(lardon3d_task_checkpoint_load(path, &loaded, &version)
        == LARDON3D_TASK_CHECKPOINT_UNSUPPORTED_VERSION);
    CHECK(version == 2);

    durable = snapshot_for(TASK_PENDING);
    CHECK(lardon3d_task_checkpoint_save(path, &durable)
        == LARDON3D_TASK_CHECKPOINT_OK);
    descriptor = open(path, O_RDWR);
    CHECK(descriptor >= 0);
    unsigned char changed = 'X';
    CHECK(pwrite(descriptor, &changed, 1, 100) == 1);
    CHECK(close(descriptor) == 0);
    CHECK(lardon3d_task_checkpoint_load(path, &loaded, NULL)
        == LARDON3D_TASK_CHECKPOINT_INVALID);

    durable = snapshot_for(TASK_PENDING);
    CHECK(lardon3d_task_checkpoint_save(path, &durable)
        == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(rewrite_u64(path, TEST_STARTED_SECONDS_OFFSET, UINT64_MAX));
    CHECK(lardon3d_task_checkpoint_load(path, &loaded, NULL)
        == LARDON3D_TASK_CHECKPOINT_INVALID);
    if (sizeof(size_t) < sizeof(uint64_t)) {
        CHECK(lardon3d_task_checkpoint_save(path, &durable)
            == LARDON3D_TASK_CHECKPOINT_OK);
        CHECK(rewrite_u64(path, TEST_MINIMUM_BATCH_OFFSET, UINT64_MAX));
        CHECK(lardon3d_task_checkpoint_load(path, &loaded, NULL)
            == LARDON3D_TASK_CHECKPOINT_INVALID);
    }

    const unsigned char incomplete[] = "incomplet";
    CHECK(write_bytes(temporary, incomplete, sizeof(incomplete)));
    CHECK(unlink(path) == 0);
    CHECK(lardon3d_task_checkpoint_load(path, &loaded, NULL)
        == LARDON3D_TASK_CHECKPOINT_NOT_FOUND);

    durable = snapshot_for(TASK_PENDING);
    durable.estimate.maximum_batch_size = 0;
    CHECK(lardon3d_task_checkpoint_save(path, &durable)
        == LARDON3D_TASK_CHECKPOINT_INVALID);
    durable = snapshot_for(TASK_PENDING);
    memset(durable.name, 'x', sizeof(durable.name));
    CHECK(lardon3d_task_checkpoint_save(path, &durable)
        == LARDON3D_TASK_CHECKPOINT_INVALID);

    CHECK(check_restored_state(TASK_COMPLETED, TASK_COMPLETED));
    CHECK(check_restored_state(TASK_FAILED, TASK_FAILED));
    CHECK(check_restored_state(TASK_CANCELLED, TASK_CANCELLED));
    CHECK(check_restored_state(TASK_RUNNING, TASK_PENDING));
    CHECK(check_restored_state(TASK_PAUSED, TASK_PENDING));

    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 2,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    Lardon3DResourceSnapshot resources = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;
    CaptureContext running = {0};
    Lardon3DTask *running_task = lardon3d_task_create(
        "Running",
        &estimate,
        capture_running_callback,
        &running
    );
    CHECK(governor && running_task && lardon3d_task_assign_id(running_task, 51));
    CHECK(lardon3d_resource_governor_reserve(
        governor, &resources, &estimate, &decision, &reservation
    ));
    CHECK(lardon3d_task_start(running_task, governor, reservation));
    CHECK(running.captured && running.snapshot.saved_state == TASK_RUNNING);
    CHECK(running.snapshot.recovery_state == TASK_PENDING);
    lardon3d_task_destroy(running_task);

    Lardon3DTaskDurableSnapshot restarted_snapshot = snapshot_for(TASK_RUNNING);
    restarted_snapshot.started_at = (struct timespec) {.tv_sec = 10, .tv_nsec = 20};
    restarted_snapshot.finished_at = (struct timespec) {.tv_sec = 30, .tv_nsec = 40};
    CaptureContext restarted = {0};
    Lardon3DTask *restarted_task = lardon3d_task_restore(
        &restarted_snapshot,
        capture_running_callback,
        &restarted
    );
    CHECK(restarted_task);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(
        governor, &resources, &estimate, &decision, &reservation
    ));
    CHECK(lardon3d_task_start(restarted_task, governor, reservation));
    CHECK(restarted.captured);
    CHECK(restarted.snapshot.started_at.tv_sec != 10);
    CHECK(restarted.snapshot.finished_at.tv_sec == 0);
    CHECK(restarted.snapshot.finished_at.tv_nsec == 0);
    lardon3d_task_destroy(restarted_task);

    SequenceContext sequence = {.governor = governor};
    Lardon3DTask *sequence_task = lardon3d_task_create(
        "Sequence",
        &estimate,
        capture_sequence_callback,
        &sequence
    );
    CHECK(sequence_task && lardon3d_task_assign_id(sequence_task, 52));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(
        governor, &resources, &estimate, &decision, &reservation
    ));
    CHECK(lardon3d_task_start(sequence_task, governor, reservation));
    CHECK(sequence.captured && sequence.snapshot.saved_state == TASK_RUNNING);
    CHECK(sequence.snapshot.recovery_state == TASK_PENDING);
    CHECK(sequence.snapshot.sequence_count == 1);
    Lardon3DTask *restored_sequence = lardon3d_task_restore(
        &sequence.snapshot,
        unused_callback,
        NULL
    );
    CHECK(restored_sequence);
    CHECK(lardon3d_task_sequence_count(restored_sequence) == 1);
    Lardon3DTaskExecutionContract restored_contract;
    CHECK(!lardon3d_task_execution_contract(
        restored_sequence,
        &restored_contract
    ));
    lardon3d_task_destroy(restored_sequence);
    lardon3d_task_destroy(sequence_task);
    lardon3d_resource_governor_destroy(governor);

    CHECK(unlink(temporary) == 0);
    CHECK(unlink(corrupt) == 0);
    CHECK(unlink(uncertain_path) == 0);
    CHECK(rmdir(uncertain_directory) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
