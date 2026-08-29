#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lardon3d/project.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>

#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); return false; \
} } while (0)

static bool
unused_callback(Lardon3DTask *task, void *userdata)
{
    (void)task;
    (void)userdata;
    return true;
}

static bool
reconstruct_test_task(
    const Lardon3DTaskDurableSnapshot *snapshot,
    void *context,
    Lardon3DTaskKindBinding *binding
)
{
    (void)snapshot;
    binding->callback = unused_callback;
    binding->userdata = context;
    return true;
}

static const Lardon3DTaskKindDescriptor test_descriptors[] = {{
    .kind = "test.persisted",
    .kind_version = 1,
    .reconstruct = reconstruct_test_task,
}};

static bool
write_ini(const char *path, const char *name, const char *stable_id, unsigned int version)
{
    FILE *file = fopen(path, "w");
    if (!file) return false;
    bool written = version == 1
        ? fprintf(file, "[project]\nname=%s\nversion=1\n", name) > 0
        : fprintf(file, "[project]\nname=%s\nstable_id=%s\nversion=2\n", name, stable_id) > 0;
    return fclose(file) == 0 && written;
}

typedef struct {
    Lardon3DAppState *state;
    Lardon3DTask *task;
    Lardon3DProjectTaskCheckpointResult result;
} CheckpointThread;

static void *
checkpoint_thread(void *userdata)
{
    CheckpointThread *context = userdata;
    context->result = lardon3d_project_checkpoint_task(context->state, context->task);
    return NULL;
}

typedef struct {
    Lardon3DAppState *state;
    const Lardon3DTaskKindRegistry *registry;
    Lardon3DProjectRecoveryEntry entry;
    size_t count;
    Lardon3DProjectDbResult result;
} RecoveryThread;

static void *
recovery_thread(void *userdata)
{
    RecoveryThread *context = userdata;
    context->result = lardon3d_project_list_recoverable(
        context->state, context->registry, 0, &context->entry, 1, &context->count);
    return NULL;
}

static bool
checkpoint_rendezvous(int sockets[2], const char *variable)
{
    char descriptor[32];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0
        || snprintf(descriptor, sizeof(descriptor), "%d", sockets[1]) <= 0
        || setenv(variable, descriptor, 1) != 0) {
        return false;
    }
    return true;
}

static bool
wait_rendezvous(int socket)
{
    unsigned char token = 0;
    return read(socket, &token, sizeof(token)) == (ssize_t)sizeof(token);
}

static bool
release_rendezvous(int socket)
{
    const unsigned char token = 1;
    return write(socket, &token, sizeof(token)) == (ssize_t)sizeof(token);
}

static bool
run_test(void)
{
    char root[] = "/tmp/lardon3d-project-life-XXXXXX";
    CHECK(mkdtemp(root));
    CHECK(setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    Lardon3DTaskKindRegistry registry;
    CHECK(lardon3d_task_kind_registry_init(&registry, test_descriptors, 1));
    CHECK(lardon3d_project_create(&state, "Projet Cycle"));
    CHECK(state.project_loaded && state.project_db && strlen(state.project_stable_id) == 32);

    char project_path[512], database_path[512], ini_path[512], checkpoint_path[512];
    CHECK(snprintf(project_path, sizeof(project_path), "%s/Projet Cycle", root) > 0);
    CHECK(snprintf(database_path, sizeof(database_path), "%s/project.db", project_path) > 0);
    CHECK(snprintf(ini_path, sizeof(ini_path), "%s/project.ini", project_path) > 0);
    CHECK(snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/.lardon3d/checkpoints/1.chk", project_path) > 0);
    struct stat information;
    CHECK(stat(database_path, &information) == 0 && S_ISREG(information.st_mode));
    char stable_id[65]; (void)snprintf(stable_id, sizeof(stable_id), "%s", state.project_stable_id);
    for (size_t index = 0; index < 32; ++index) {
        CHECK((stable_id[index] >= '0' && stable_id[index] <= '9')
            || (stable_id[index] >= 'a' && stable_id[index] <= 'f'));
    }
    Lardon3DProjectDbProject db_project;
    CHECK(lardon3d_project_db_get_project(state.project_db, &db_project) == LARDON3D_PROJECT_DB_OK);
    CHECK(strcmp(db_project.stable_id, stable_id) == 0);

    lardon3d_project_close(&state);
    CHECK(!state.project_loaded && !state.project_db && !state.project_stable_id[0]);
    CHECK(lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(strcmp(state.project_stable_id, stable_id) == 0);
    lardon3d_project_close(&state);
    CHECK(write_ini(ini_path, "Projet Cycle", "A0000000000000000000000000000000", 2));
    CHECK(!lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(!state.project_loaded && !state.project_db);
    CHECK(strstr(state.status_message, "project.ini invalide") != NULL);
    CHECK(write_ini(ini_path, "Projet Cycle", "", 1));
    CHECK(lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(strcmp(state.project_stable_id, stable_id) == 0);
    lardon3d_project_close(&state);
    CHECK(unlink(database_path) == 0);
    CHECK(lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(strcmp(state.project_stable_id, stable_id) == 0);

    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Persistée", &estimate, "test.persisted", 1,
        unused_callback, NULL, NULL);
    CHECK(task && lardon3d_task_assign_id(task, 1));
    CHECK(lardon3d_task_set_progress(task, 10, "frontière 10"));
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    CHECK(stat(checkpoint_path, &information) == 0 && S_ISREG(information.st_mode));
    Lardon3DProjectDbTask db_task;
    Lardon3DProjectRecoveryEntry entries[2]; size_t count = 0;
    Lardon3DTaskDurableSnapshot disk_snapshot; uint32_t version = 0;
    CHECK(lardon3d_project_db_load_task(state.project_db, 1, &db_task) == LARDON3D_PROJECT_DB_OK);
    CHECK(db_task.progress == 10 && strcmp(db_task.checkpoint.path, ".lardon3d/checkpoints/1.chk") == 0);

    CHECK(setenv("LARDON3D_TEST_CHECKPOINT_SYNC_DIRECTORY_FAILURE", "1", 1) == 0);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE);
    CHECK(unsetenv("LARDON3D_TEST_CHECKPOINT_SYNC_DIRECTORY_FAILURE") == 0);
    CHECK(lardon3d_project_db_load_task(state.project_db, 1, &db_task) == LARDON3D_PROJECT_DB_OK);
    CHECK(db_task.checkpoint.durability == LARDON3D_DB_CHECKPOINT_DURABLE);
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(count == 1 && entries[0].status == LARDON3D_PROJECT_RECOVERABLE);

    CHECK(lardon3d_task_set_progress(task, 20, "frontière 20"));
    CHECK(setenv("LARDON3D_TEST_CHECKPOINT_PREPUBLICATION_FAILURE", "1", 1) == 0);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_IO_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_CHECKPOINT_PREPUBLICATION_FAILURE") == 0);
    CHECK(lardon3d_project_db_load_task(state.project_db, 1, &db_task) == LARDON3D_PROJECT_DB_OK && db_task.progress == 10);
    Lardon3DTask *unpublished = lardon3d_task_create_typed(
        "Non publiée", &estimate, "test.persisted", 1,
        unused_callback, NULL, NULL);
    CHECK(unpublished && lardon3d_task_assign_id(unpublished, 3));
    CHECK(setenv("LARDON3D_TEST_CHECKPOINT_PREPUBLICATION_FAILURE", "1", 1) == 0);
    CHECK(lardon3d_project_checkpoint_task(&state, unpublished) == LARDON3D_PROJECT_TASK_CHECKPOINT_IO_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_CHECKPOINT_PREPUBLICATION_FAILURE") == 0);
    CHECK(lardon3d_project_db_load_task(state.project_db, 3, &db_task) == LARDON3D_PROJECT_DB_NOT_FOUND);
    lardon3d_task_destroy(unpublished);

    CHECK(lardon3d_task_set_progress(task, 25, "frontière 25"));
    CHECK(setenv("LARDON3D_TEST_PROJECT_DB_BUSY_CHECKPOINT", "1", 1) == 0);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_DB_BUSY);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_BUSY_CHECKPOINT") == 0);
    CHECK(lardon3d_task_checkpoint_load(checkpoint_path, &disk_snapshot, &version) == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(disk_snapshot.progress == 10);
    CHECK(lardon3d_task_checkpoint_load_staged(checkpoint_path, &disk_snapshot, &version)
        == LARDON3D_TASK_CHECKPOINT_OK && disk_snapshot.progress == 25);
    CHECK(lardon3d_project_db_load_task(state.project_db, 1, &db_task) == LARDON3D_PROJECT_DB_OK && db_task.progress == 10);
    /* DB=S0 retains canonical S0 despite a fully durable, stale staged S1. */
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count)
        == LARDON3D_PROJECT_DB_OK && count == 1
        && entries[0].status == LARDON3D_PROJECT_RECOVERABLE
        && entries[0].snapshot.progress == 10);
    CHECK(lardon3d_task_set_progress(task, 20, "frontière 20"));

    CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT", "1", 1) == 0);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_DB_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT") == 0);
    CHECK(lardon3d_task_checkpoint_load(checkpoint_path, &disk_snapshot, &version) == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(disk_snapshot.progress == 10);
    CHECK(lardon3d_task_checkpoint_load_staged(checkpoint_path, &disk_snapshot, &version)
        == LARDON3D_TASK_CHECKPOINT_OK && disk_snapshot.progress == 20);
    CHECK(lardon3d_project_db_load_task(state.project_db, 1, &db_task) == LARDON3D_PROJECT_DB_OK && db_task.progress == 10);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);

    /* Forced interruption after the DB transaction leaves DB=S1, canonical
     * S0, and staged S1.  Reopen/list recovery must accept only staged S1 and
     * repair canonical publication without any database reconciliation. */
    CHECK(lardon3d_task_set_progress(task, 30, "frontière 30"));
    CHECK(setenv("LARDON3D_TEST_PROJECT_CHECKPOINT_AFTER_DB_COMMIT", "1", 1) == 0);
    CHECK(lardon3d_project_checkpoint_task(&state, task)
        == LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_CHECKPOINT_AFTER_DB_COMMIT") == 0);
    CHECK(lardon3d_project_db_load_task(state.project_db, 1, &db_task)
        == LARDON3D_PROJECT_DB_OK && db_task.progress == 30);
    CHECK(lardon3d_task_checkpoint_load(checkpoint_path, &disk_snapshot, NULL)
        == LARDON3D_TASK_CHECKPOINT_OK && disk_snapshot.progress == 20);
    CHECK(lardon3d_task_checkpoint_load_staged(checkpoint_path, &disk_snapshot, NULL)
        == LARDON3D_TASK_CHECKPOINT_OK && disk_snapshot.progress == 30);
    lardon3d_project_close(&state);
    CHECK(lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count)
        == LARDON3D_PROJECT_DB_OK && count == 1
        && entries[0].snapshot.progress == 30);
    CHECK(lardon3d_task_checkpoint_load(checkpoint_path, &disk_snapshot, NULL)
        == LARDON3D_TASK_CHECKPOINT_OK && disk_snapshot.progress == 30);

    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(count == 1 && entries[0].status == LARDON3D_PROJECT_RECOVERABLE && entries[0].snapshot.progress == 30);

    /* Recovery enumerates S0 before waiting. The writer then publishes S1;
     * recovery must reload S1 under .chk.lock rather than validate S0. */
    int stale_row_sockets[2];
    CHECK(checkpoint_rendezvous(stale_row_sockets,
        "LARDON3D_TEST_RECOVERY_BEFORE_CHECKPOINT_LOCK_FD"));
    RecoveryThread stale_row = {.state = &state, .registry = &registry};
    pthread_t stale_row_thread;
    CHECK(pthread_create(&stale_row_thread, NULL, recovery_thread, &stale_row) == 0);
    CHECK(wait_rendezvous(stale_row_sockets[0]));
    CHECK(lardon3d_task_set_progress(task, 35, "frontière 35"));
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    CHECK(release_rendezvous(stale_row_sockets[0]));
    CHECK(pthread_join(stale_row_thread, NULL) == 0);
    CHECK(unsetenv("LARDON3D_TEST_RECOVERY_BEFORE_CHECKPOINT_LOCK_FD") == 0);
    CHECK(close(stale_row_sockets[0]) == 0 && close(stale_row_sockets[1]) == 0);
    CHECK(stale_row.result == LARDON3D_PROJECT_DB_OK && stale_row.count == 1
        && stale_row.entry.status == LARDON3D_PROJECT_RECOVERABLE
        && stale_row.entry.snapshot.progress == 35);

    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 257, &count) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

    CHECK(unlink(checkpoint_path) == 0);
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(count == 1 && entries[0].status == LARDON3D_PROJECT_RECOVERY_MISSING_CHECKPOINT);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    int descriptor = open(checkpoint_path, O_WRONLY | O_TRUNC); CHECK(descriptor >= 0);
    CHECK(write(descriptor, "corrupt", 7) == 7 && close(descriptor) == 0);
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(entries[0].status == LARDON3D_PROJECT_RECOVERY_INVALID_CHECKPOINT);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    descriptor = open(checkpoint_path, O_RDWR); CHECK(descriptor >= 0);
    unsigned char future_version[4] = {2, 0, 0, 0};
    CHECK(pwrite(descriptor, future_version, sizeof(future_version), 8) == 4 && close(descriptor) == 0);
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(entries[0].status == LARDON3D_PROJECT_RECOVERY_UNSUPPORTED_CHECKPOINT);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    CHECK(lardon3d_task_checkpoint_load(checkpoint_path, &disk_snapshot, NULL)
        == LARDON3D_TASK_CHECKPOINT_OK);
    disk_snapshot.id = 99;
    CHECK(lardon3d_task_checkpoint_save(checkpoint_path, &disk_snapshot)
        == LARDON3D_TASK_CHECKPOINT_OK);
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2,
        &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(entries[0].status == LARDON3D_PROJECT_RECOVERY_INVALID_CHECKPOINT);
    CHECK(lardon3d_project_checkpoint_task(&state, task)
        == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);

    CheckpointThread contexts[2] = {{.state = &state, .task = task}, {.state = &state, .task = task}};
    pthread_t threads[2]; CHECK(pthread_create(&threads[0], NULL, checkpoint_thread, &contexts[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, checkpoint_thread, &contexts[1]) == 0);
    CHECK(pthread_join(threads[0], NULL) == 0 && pthread_join(threads[1], NULL) == 0);
    CHECK((contexts[0].result == LARDON3D_PROJECT_TASK_CHECKPOINT_OK
           || contexts[0].result == LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE)
        && (contexts[1].result == LARDON3D_PROJECT_TASK_CHECKPOINT_OK
            || contexts[1].result == LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE));

    Lardon3DTask *terminal = lardon3d_task_create_typed(
        "Terminale", &estimate, "test.persisted", 1,
        unused_callback, NULL, NULL);
    CHECK(terminal && lardon3d_task_assign_id(terminal, 2)); lardon3d_task_request_cancel(terminal);
    CHECK(lardon3d_project_checkpoint_task(&state, terminal) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK && count == 1);
    lardon3d_task_destroy(terminal);

    lardon3d_project_close(&state);
    CHECK(lardon3d_project_checkpoint_task(&state, task) == LARDON3D_PROJECT_TASK_CHECKPOINT_NO_PROJECT);
    CHECK(lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0, entries, 2, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(count == 1 && entries[0].status == LARDON3D_PROJECT_RECOVERABLE);
    Lardon3DHardwareProfile recovery_profile = {
        .logical_cpu_count = 2,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy recovery_policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    state.resource_governor = lardon3d_resource_governor_create(
        &recovery_profile, &recovery_policy);
    state.task_queue = state.resource_governor
        ? lardon3d_task_queue_create(state.resource_governor, 1) : NULL;
    CHECK(state.task_queue);
    Lardon3DProjectRecoverySummary busy_summary;
    CHECK(setenv("LARDON3D_TEST_PROJECT_DB_BUSY_RECOVERY", "1", 1) == 0);
    CHECK(lardon3d_project_resume_recoverable_tasks(&state, &registry,
        &busy_summary) == LARDON3D_PROJECT_DB_BUSY);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_BUSY_RECOVERY") == 0);
    CHECK(busy_summary.inspected == 0 && busy_summary.resumed == 0
        && busy_summary.failed == 1);
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_resource_governor_destroy(state.resource_governor);
    state.resource_governor = NULL;
    CHECK(strcmp(entries[0].task_kind, "test.persisted") == 0
        && entries[0].task_kind_version == 1);
    Lardon3DTask *restored = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
        &registry, entries[0].task_kind, entries[0].task_kind_version,
        &entries[0].snapshot, NULL, &restored) == LARDON3D_TASK_KIND_OK);
    Lardon3DTaskSnapshot restored_snapshot;
    CHECK(restored && lardon3d_task_snapshot(restored, &restored_snapshot));
    CHECK(restored_snapshot.id == 1 && restored_snapshot.state == TASK_PENDING
        && restored_snapshot.progress == 35
        && lardon3d_task_sequence_count(restored) == 0);
    lardon3d_task_destroy(restored);

    Lardon3DTask *unknown = lardon3d_task_create_typed(
        "Unknown", &estimate, "unknown.work", 1, unused_callback, NULL, NULL);
    Lardon3DTask *future_kind = lardon3d_task_create_typed(
        "Future kind", &estimate, "test.persisted", 2,
        unused_callback, NULL, NULL);
    CHECK(unknown && future_kind && lardon3d_task_assign_id(unknown, 4)
        && lardon3d_task_assign_id(future_kind, 5));
    CHECK(lardon3d_project_checkpoint_task(&state, unknown)
        == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    CHECK(lardon3d_project_checkpoint_task(&state, future_kind)
        == LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
    Lardon3DTask *legacy = lardon3d_task_create(
        "Legacy", &estimate, unused_callback, NULL);
    CHECK(legacy && lardon3d_task_assign_id(legacy, 6));
    CHECK(lardon3d_project_checkpoint_task(&state, legacy)
        == LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK);
    Lardon3DTaskDurableSnapshot legacy_snapshot;
    CHECK(lardon3d_task_durable_snapshot(legacy, &legacy_snapshot));
    char legacy_path[512];
    CHECK(snprintf(legacy_path, sizeof(legacy_path),
        "%s/.lardon3d/checkpoints/6.chk", project_path) > 0);
    CHECK(lardon3d_task_checkpoint_save(legacy_path, &legacy_snapshot)
        == LARDON3D_TASK_CHECKPOINT_OK);
    Lardon3DProjectDbCheckpoint legacy_checkpoint = {
        .format_version = LARDON3D_TASK_CHECKPOINT_VERSION,
        .durability = LARDON3D_DB_CHECKPOINT_DURABLE,
        .updated_at = 500,
    };
    (void)snprintf(legacy_checkpoint.path, sizeof(legacy_checkpoint.path),
        ".lardon3d/checkpoints/6.chk");
    CHECK(lardon3d_project_db_record_task(state.project_db, &legacy_snapshot,
        NULL, 0, &legacy_checkpoint, 500) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectRecoveryEntry negative_entries[4];
    CHECK(lardon3d_project_list_recoverable(&state, &registry, 0,
        negative_entries, 4, &count) == LARDON3D_PROJECT_DB_OK && count == 4);
    CHECK(negative_entries[1].task_id == 4
        && negative_entries[1].status == LARDON3D_PROJECT_RECOVERY_UNKNOWN_TASK_KIND);
    CHECK(negative_entries[2].task_id == 5
        && negative_entries[2].status
            == LARDON3D_PROJECT_RECOVERY_UNSUPPORTED_TASK_KIND_VERSION);
    CHECK(negative_entries[3].task_id == 6
        && negative_entries[3].status == LARDON3D_PROJECT_RECOVERY_LEGACY_UNTYPED);
    state.resource_governor = lardon3d_resource_governor_create(
        &recovery_profile, &recovery_policy);
    state.task_queue = state.resource_governor
        ? lardon3d_task_queue_create(state.resource_governor, 4) : NULL;
    CHECK(state.task_queue);
    Lardon3DProjectRecoverySummary selective_summary;
    CHECK(lardon3d_project_resume_recoverable_tasks(&state, &registry,
        &selective_summary) == LARDON3D_PROJECT_DB_OK);
    CHECK(selective_summary.inspected == 4 && selective_summary.resumed == 1
        && selective_summary.skipped == 3 && selective_summary.failed == 0);
    Lardon3DTaskSnapshot resumed_snapshot;
    CHECK(lardon3d_task_queue_get(state.task_queue, 1, &resumed_snapshot));
    CHECK(lardon3d_project_resume_recoverable_tasks(&state, &registry,
        &selective_summary) == LARDON3D_PROJECT_DB_OK);
    CHECK(selective_summary.resumed == 0 && selective_summary.skipped == 4);
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_resource_governor_destroy(state.resource_governor);
    state.resource_governor = NULL;
    lardon3d_task_destroy(legacy);
    lardon3d_task_destroy(future_kind);
    lardon3d_task_destroy(unknown);
    lardon3d_project_close(&state);

    CHECK(write_ini(ini_path, "Projet Cycle", "00000000000000000000000000000000", 2));
    CHECK(!lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(!state.project_loaded && !state.project_db);
    CHECK(write_ini(ini_path, "Projet Cycle", stable_id, 2));
    descriptor = open(database_path, O_WRONLY | O_TRUNC); CHECK(descriptor >= 0);
    CHECK(write(descriptor, "not sqlite", 10) == 10 && close(descriptor) == 0);
    CHECK(!lardon3d_project_open(&state, "Projet Cycle"));
    CHECK(!state.project_loaded && !state.project_db);
    lardon3d_task_destroy(task);

    char terminal_checkpoint[512]; CHECK(snprintf(terminal_checkpoint, sizeof(terminal_checkpoint), "%s/.lardon3d/checkpoints/2.chk", project_path) > 0);
    char unknown_checkpoint[512], future_kind_checkpoint[512];
    CHECK(snprintf(unknown_checkpoint, sizeof(unknown_checkpoint), "%s/.lardon3d/checkpoints/4.chk", project_path) > 0);
    CHECK(snprintf(future_kind_checkpoint, sizeof(future_kind_checkpoint), "%s/.lardon3d/checkpoints/5.chk", project_path) > 0);
    CHECK(unlink(checkpoint_path) == 0); CHECK(unlink(terminal_checkpoint) == 0);
    CHECK(unlink(unknown_checkpoint) == 0); CHECK(unlink(future_kind_checkpoint) == 0);
    CHECK(unlink(legacy_path) == 0);
    char path[512];
    const unsigned int locked_task_ids[] = {1, 2, 3, 4, 5};
    for (size_t index = 0; index < sizeof(locked_task_ids) / sizeof(locked_task_ids[0]); ++index) {
        CHECK(snprintf(path, sizeof(path), "%s/.lardon3d/checkpoints/%u.chk.lock", project_path,
            locked_task_ids[index]) > 0 && unlink(path) == 0);
    }
    CHECK(unlink(database_path) == 0); CHECK(unlink(ini_path) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/.lardon3d/checkpoints", project_path) > 0 && rmdir(path) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/.lardon3d", project_path) > 0 && rmdir(path) == 0);
    const char *directories[] = {"images", "reconstruction", "exports", "logs"};
    for (size_t index = 0; index < 4; ++index) { CHECK(snprintf(path, sizeof(path), "%s/%s", project_path, directories[index]) > 0); CHECK(rmdir(path) == 0); }
    CHECK(rmdir(project_path) == 0); CHECK(rmdir(root) == 0);
    CHECK(unsetenv("LARDON3D_PROJECTS_ROOT") == 0);
    return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
