#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/image_catalog.h>
#include <lardon3d/import_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>

#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); return false; \
} } while (0)

static bool
join_path(char output[PATH_MAX], const char *parent, const char *child)
{
    int written = snprintf(output, PATH_MAX, "%s/%s", parent, child);
    return written > 0 && (size_t)written < PATH_MAX;
}

static bool
write_fixture(const char *path, unsigned int value)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return false;
    unsigned char data[64];
    memset(data, (int)(value & 0xffU), sizeof(data));
    bool ok = write(descriptor, data, sizeof(data)) == (ssize_t)sizeof(data);
    return close(descriptor) == 0 && ok;
}

static bool
remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        if (!join_path(child, path, entry->d_name) || !remove_tree(child)) ok = false;
    }
    if (closedir(directory) != 0 || rmdir(path) != 0) ok = false;
    return ok;
}

static bool
setup_runtime(Lardon3DAppState *state, size_t queue_capacity)
{
    state->hardware_profile = (Lardon3DHardwareProfile) {
        .logical_cpu_count = 1024,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    state->resource_governor = lardon3d_resource_governor_create(
        &state->hardware_profile, &policy);
    state->task_queue = state->resource_governor
        ? lardon3d_task_queue_create(state->resource_governor, queue_capacity)
        : NULL;
    return state->task_queue != NULL;
}

static bool
wait_for_state(Lardon3DTaskQueue *queue, uint64_t id,
    Lardon3DTaskState expected, Lardon3DTaskSnapshot *snapshot)
{
    for (size_t attempt = 0; attempt < 1000000; ++attempt) {
        if (lardon3d_task_queue_get(queue, id, snapshot)
            && snapshot->state == expected) return true;
        (void)sched_yield();
    }
    return false;
}

static bool
run_test(void)
{
    char root[] = "/tmp/lardon3d-import-generic-XXXXXX";
    CHECK(mkdtemp(root));
    char source[PATH_MAX]; CHECK(join_path(source, root, "source"));
    CHECK(mkdir(source, 0700) == 0);
    for (unsigned int index = 0; index < 80; ++index) {
        char name[32], path[PATH_MAX];
        CHECK(snprintf(name, sizeof(name), "image-%03u.jpg", index) > 0);
        CHECK(join_path(path, source, name) && write_fixture(path, index));
    }
    CHECK(setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
    Lardon3DAppState state; lardon3d_app_state_init(&state);
    CHECK(setup_runtime(&state, 4));
    CHECK(lardon3d_project_create(&state, "Persistent Import"));
    Lardon3DProjectDbScanSet scanset;
    CHECK(lardon3d_image_catalog_create_scanset(&state, "Campagne A", &scanset));

    CHECK(setenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH", "1", 1) == 0);
    CHECK(setenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
    uint64_t task_id = 0;
    CHECK(lardon3d_project_enqueue_image_import(&state, scanset.scanset_id,
        source, &task_id));
    CHECK(task_id > 0 && task_id <= INT64_MAX);
    Lardon3DTaskSnapshot runtime;
    CHECK(wait_for_state(state.task_queue, task_id, TASK_PAUSED, &runtime));
    CHECK(runtime.progress > 0 && runtime.progress < 100);
    lardon3d_task_queue_destroy(state.task_queue); state.task_queue = NULL;
    char checkpoint_path[PATH_MAX];
    CHECK(snprintf(checkpoint_path, sizeof(checkpoint_path),
        "%s/.lardon3d/checkpoints/%llu.chk", state.project_path,
        (unsigned long long)task_id) > 0);
    Lardon3DTaskDurableSnapshot persisted_snapshot;
    CHECK(lardon3d_task_checkpoint_load(checkpoint_path, &persisted_snapshot,
        NULL) == LARDON3D_TASK_CHECKPOINT_OK);
    Lardon3DProjectDbCheckpoint published_not_durable = {
        .format_version = LARDON3D_TASK_CHECKPOINT_VERSION,
        .durability = LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE,
        .updated_at = 1,
    };
    CHECK(snprintf(published_not_durable.path,
        sizeof(published_not_durable.path),
        ".lardon3d/checkpoints/%llu.chk",
        (unsigned long long)task_id) > 0);
    CHECK(lardon3d_project_db_record_image_import_task(state.project_db,
        &persisted_snapshot, LARDON3D_IMAGE_IMPORT_TASK_KIND,
        LARDON3D_IMAGE_IMPORT_TASK_KIND_VERSION, &published_not_durable,
        source, scanset.scanset_id, 1) == LARDON3D_PROJECT_DB_OK);
    lardon3d_project_close(&state);
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH") == 0);
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT") == 0);

    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 4);
    CHECK(setenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH", "1", 1) == 0);
    CHECK(state.task_queue && lardon3d_project_open(&state, "Persistent Import"));
    Lardon3DProjectRecoverySummary recovery;
    CHECK(lardon3d_project_last_recovery_summary(&state, &recovery));
    CHECK(recovery.inspected == 1 && recovery.resumed == 1
        && recovery.skipped == 0 && recovery.failed == 0
        && recovery.published_not_durable == 1);
    CHECK(wait_for_state(state.task_queue, task_id, TASK_PAUSED, &runtime));
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH") == 0);
    CHECK(lardon3d_task_queue_resume(state.task_queue, task_id));
    Lardon3DProjectDbImageImport persisted_parameters;
    CHECK(lardon3d_project_db_load_image_import(state.project_db, task_id,
        &persisted_parameters) == LARDON3D_PROJECT_DB_OK);
    CHECK(strcmp(persisted_parameters.source_path, source) == 0);
    CHECK(persisted_parameters.scanset_id == scanset.scanset_id);
    CHECK(wait_for_state(state.task_queue, task_id, TASK_COMPLETED, &runtime));
    CHECK(runtime.progress == 100);

    uint64_t image_count = 0;
    CHECK(lardon3d_project_db_count_images(state.project_db,
        scanset.scanset_id, &image_count) == LARDON3D_PROJECT_DB_OK
        && image_count == 80);
    char catalog_error[256];
    Lardon3DImageCatalog *legacy_catalog = lardon3d_image_catalog_load(&state,
        catalog_error, sizeof(catalog_error));
    CHECK(legacy_catalog && lardon3d_image_catalog_count(legacy_catalog) == 80);
    lardon3d_image_catalog_destroy(legacy_catalog);
    lardon3d_task_queue_destroy(state.task_queue); state.task_queue = NULL;
    lardon3d_project_close(&state);

    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 4);
    CHECK(state.task_queue && lardon3d_project_open(&state, "Persistent Import"));
    CHECK(lardon3d_project_last_recovery_summary(&state, &recovery)
        && recovery.inspected == 0 && recovery.resumed == 0);
    lardon3d_task_queue_destroy(state.task_queue); state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    CHECK(unsetenv("LARDON3D_PROJECTS_ROOT") == 0);
    CHECK(remove_tree(root));
    return true;
}

static bool
test_selective_capacity_one(void)
{
    char root[] = "/tmp/lardon3d-import-recovery-window-XXXXXX";
    CHECK(mkdtemp(root));
    char missing_source[PATH_MAX], valid_source[PATH_MAX], second_valid_source[PATH_MAX];
    char unavailable_source[PATH_MAX];
    CHECK(join_path(missing_source, root, "missing-source"));
    CHECK(join_path(valid_source, root, "valid-source"));
    CHECK(join_path(second_valid_source, root, "second-valid-source"));
    CHECK(join_path(unavailable_source, root, "source-unavailable"));
    CHECK(mkdir(missing_source, 0700) == 0
        && mkdir(valid_source, 0700) == 0
        && mkdir(second_valid_source, 0700) == 0);
    for (unsigned int index = 0; index < 40; ++index) {
        char name[32], path[PATH_MAX];
        CHECK(snprintf(name, sizeof(name), "missing-%03u.jpg", index) > 0);
        CHECK(join_path(path, missing_source, name)
            && write_fixture(path, index));
        CHECK(snprintf(name, sizeof(name), "valid-%03u.jpg", index) > 0);
        CHECK(join_path(path, valid_source, name)
            && write_fixture(path, index + 100));
        CHECK(snprintf(name, sizeof(name), "second-%03u.jpg", index) > 0);
        CHECK(join_path(path, second_valid_source, name)
            && write_fixture(path, index + 200));
    }
    CHECK(setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    CHECK(setup_runtime(&state, 4));
    CHECK(lardon3d_project_create(&state, "Selective Recovery"));
    Lardon3DProjectDbScanSet scanset;
    CHECK(lardon3d_image_catalog_create_scanset(&state, "Sélectif", &scanset));
    CHECK(setenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH", "1", 1) == 0);
    CHECK(setenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT", "1", 1)
        == 0);
    uint64_t missing_id = 0, valid_id = 0, second_valid_id = 0;
    CHECK(lardon3d_project_enqueue_image_import(&state, scanset.scanset_id,
        missing_source,
        &missing_id));
    CHECK(lardon3d_project_enqueue_image_import(&state, scanset.scanset_id,
        valid_source,
        &valid_id));
    CHECK(lardon3d_project_enqueue_image_import(&state, scanset.scanset_id,
        second_valid_source,
        &second_valid_id));
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_for_state(state.task_queue, missing_id, TASK_PAUSED, &snapshot));
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH") == 0);
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT") == 0);
    CHECK(rename(missing_source, unavailable_source) == 0);

    const Lardon3DResourceEstimate held_estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_IMPORT,
    };
    Lardon3DResourceSnapshot available = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *held = NULL;
    CHECK(lardon3d_resource_governor_reserve(state.resource_governor,
        &available, &held_estimate, &decision, &held));
    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 1);
    CHECK(state.task_queue
        && lardon3d_project_open(&state, "Selective Recovery"));
    Lardon3DProjectRecoverySummary recovery;
    CHECK(lardon3d_project_last_recovery_summary(&state, &recovery));
    CHECK(recovery.inspected == 3 && recovery.resumed == 1
        && recovery.skipped == 1 && recovery.failed == 1
        && recovery.queue_full);
    CHECK(lardon3d_resource_governor_release(state.resource_governor, held));
    lardon3d_task_queue_resources_changed(state.task_queue);
    CHECK(wait_for_state(state.task_queue, valid_id, TASK_COMPLETED, &snapshot));
    CHECK(snapshot.id == valid_id && snapshot.progress == 100);
    CHECK(!lardon3d_task_queue_get(state.task_queue, missing_id, &snapshot));
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);

    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 1);
    CHECK(state.task_queue
        && lardon3d_project_open(&state, "Selective Recovery"));
    CHECK(lardon3d_project_last_recovery_summary(&state, &recovery));
    CHECK(recovery.inspected == 2 && recovery.resumed == 1
        && recovery.failed == 1);
    CHECK(wait_for_state(state.task_queue, second_valid_id, TASK_COMPLETED,
        &snapshot));
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);

    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 1);
    CHECK(state.task_queue
        && lardon3d_project_open(&state, "Selective Recovery"));
    CHECK(lardon3d_project_last_recovery_summary(&state, &recovery));
    CHECK(recovery.inspected == 1 && recovery.resumed == 0
        && recovery.failed == 1);
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    CHECK(unsetenv("LARDON3D_PROJECTS_ROOT") == 0);
    CHECK(remove_tree(root));
    return true;
}

int main(void)
{
    return run_test() && test_selective_capacity_one()
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
