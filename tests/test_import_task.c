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
setup_runtime(Lardon3DAppState *state)
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
        ? lardon3d_task_queue_create(state->resource_governor, 4) : NULL;
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
    CHECK(setup_runtime(&state));
    CHECK(lardon3d_project_create(&state, "Persistent Import"));

    CHECK(setenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH", "1", 1) == 0);
    CHECK(setenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
    uint64_t task_id = 0;
    CHECK(lardon3d_project_enqueue_image_import(&state, source, &task_id));
    CHECK(task_id > 0 && task_id <= INT64_MAX);
    Lardon3DTaskSnapshot runtime;
    CHECK(wait_for_state(state.task_queue, task_id, TASK_PAUSED, &runtime));
    CHECK(runtime.progress > 0 && runtime.progress < 100);
    lardon3d_task_queue_destroy(state.task_queue); state.task_queue = NULL;
    lardon3d_project_close(&state);
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH") == 0);
    CHECK(unsetenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT") == 0);

    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 4);
    CHECK(state.task_queue && lardon3d_project_open(&state, "Persistent Import"));
    Lardon3DProjectRecoveryEntry entry; size_t count = 0;
    const Lardon3DTaskKindRegistry *registry =
        lardon3d_task_kind_registry_production();
    CHECK(lardon3d_project_list_recoverable(&state, registry, 0, &entry, 1,
        &count) == LARDON3D_PROJECT_DB_OK && count == 1);
    CHECK(entry.task_id == task_id
        && strcmp(entry.task_kind, LARDON3D_IMAGE_IMPORT_TASK_KIND) == 0
        && entry.task_kind_version == LARDON3D_IMAGE_IMPORT_TASK_KIND_VERSION);
    Lardon3DProjectDbImageImport persisted_parameters;
    CHECK(lardon3d_project_db_load_image_import(state.project_db, task_id,
        &persisted_parameters) == LARDON3D_PROJECT_DB_OK);
    CHECK(strcmp(persisted_parameters.source_path, source) == 0);
    Lardon3DImageImportReconstructionContext reconstruction = {
        .project_path = state.project_path,
        .project_db = state.project_db,
        .resource_governor = state.resource_governor,
    };
    Lardon3DTask *restored = NULL;
    char unavailable_source[PATH_MAX];
    CHECK(join_path(unavailable_source, root, "source-unavailable"));
    CHECK(rename(source, unavailable_source) == 0);
    CHECK(lardon3d_task_kind_registry_restore(registry, entry.task_kind,
        entry.task_kind_version, &entry.snapshot, &reconstruction, &restored)
        == LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED);
    CHECK(!restored && rename(unavailable_source, source) == 0);
    CHECK(lardon3d_task_kind_registry_restore(registry, entry.task_kind,
        entry.task_kind_version, &entry.snapshot, &reconstruction, &restored)
        == LARDON3D_TASK_KIND_OK);
    CHECK(restored && lardon3d_task_id(restored) == task_id);
    CHECK(lardon3d_task_queue_add(state.task_queue, restored, NULL));
    CHECK(wait_for_state(state.task_queue, task_id, TASK_COMPLETED, &runtime));
    CHECK(runtime.progress == 100);

    char error[256];
    Lardon3DImageCatalog *catalog = lardon3d_image_catalog_load(
        &state, error, sizeof(error));
    CHECK(catalog && lardon3d_image_catalog_count(catalog) == 80);
    lardon3d_image_catalog_destroy(catalog);
    lardon3d_task_queue_destroy(state.task_queue); state.task_queue = NULL;
    lardon3d_project_close(&state);

    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 4);
    CHECK(state.task_queue && lardon3d_project_open(&state, "Persistent Import"));
    CHECK(lardon3d_project_list_recoverable(&state, registry, 0, &entry, 1,
        &count) == LARDON3D_PROJECT_DB_OK && count == 0);
    lardon3d_task_queue_destroy(state.task_queue); state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    CHECK(unsetenv("LARDON3D_PROJECTS_ROOT") == 0);
    CHECK(remove_tree(root));
    return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
