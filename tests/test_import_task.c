#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/app_state.h>
#include <lardon3d/import_task.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

static bool
join_path(char destination[PATH_MAX], const char *parent, const char *child)
{
    int written = snprintf(destination, PATH_MAX, "%s/%s", parent, child);
    return written >= 0 && (size_t)written < PATH_MAX;
}

static bool
write_all(int descriptor, const void *data, size_t size)
{
    const char *bytes = data;
    size_t total = 0;
    while (total < size) {
        ssize_t written = write(descriptor, bytes + total, size - total);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        total += (size_t)written;
    }
    return true;
}

static bool
create_file(const char *path, size_t size)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        return false;
    }
    char block[64 * 1024];
    (void)memset(block, 'L', sizeof(block));
    bool success = true;
    while (size > 0) {
        size_t chunk = size < sizeof(block) ? size : sizeof(block);
        if (!write_all(descriptor, block, chunk)) {
            success = false;
            break;
        }
        size -= chunk;
    }
    if (close(descriptor) != 0) {
        success = false;
    }
    return success;
}

static bool
read_file(const char *path, char *content, size_t capacity)
{
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0 || capacity == 0) {
        return false;
    }
    size_t total = 0;
    while (total + 1 < capacity) {
        ssize_t count = read(descriptor, content + total, capacity - total - 1);
        if (count == 0) {
            break;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            (void)close(descriptor);
            return false;
        }
        total += (size_t)count;
    }
    content[total] = '\0';
    return close(descriptor) == 0;
}

static bool
remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(info.st_mode)) {
        return unlink(path) == 0;
    }

    DIR *directory = opendir(path);
    if (!directory) {
        return false;
    }
    bool success = true;
    for (struct dirent *entry = readdir(directory);
         entry;
         entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (!join_path(child, path, entry->d_name) || !remove_tree(child)) {
            success = false;
        }
    }
    if (closedir(directory) != 0 || rmdir(path) != 0) {
        success = false;
    }
    return success;
}

static bool
has_temporary_file(const char *images_path)
{
    DIR *directory = opendir(images_path);
    if (!directory) {
        return true;
    }
    bool found = false;
    for (struct dirent *entry = readdir(directory);
         entry;
         entry = readdir(directory)) {
        if (strncmp(entry->d_name, ".manifest.tsv.tmp.", 18) == 0) {
            found = true;
        }
    }
    if (closedir(directory) != 0) {
        found = true;
    }
    return found;
}

static bool
wait_until_finished(
    Lardon3DImportTask *task,
    Lardon3DImportTaskSnapshot *snapshot
)
{
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        if (!lardon3d_import_task_snapshot(task, snapshot)
            || snapshot->processed > snapshot->total) {
            return false;
        }
        if (snapshot->status != LARDON3D_IMPORT_TASK_RUNNING) {
            return true;
        }
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

static bool
run_success_tests(
    Lardon3DAppState *state,
    const char *source,
    const char *manifest
)
{
    Lardon3DImportTask *unused = lardon3d_import_task_create();
    CHECK(unused);
    CHECK(!lardon3d_import_task_start(NULL, state, source));
    CHECK(!lardon3d_import_task_start(unused, NULL, source));
    CHECK(!lardon3d_import_task_start(unused, state, NULL));
    lardon3d_import_task_destroy(unused);

    Lardon3DImportTask *task = lardon3d_import_task_create();
    CHECK(task);
    CHECK(lardon3d_import_task_start(task, state, source));
    CHECK(!lardon3d_import_task_start(task, state, source));
    Lardon3DImportTaskSnapshot snapshot;
    CHECK(wait_until_finished(task, &snapshot));
    CHECK(snapshot.status == LARDON3D_IMPORT_TASK_SUCCEEDED);
    CHECK(snapshot.total == 2);
    CHECK(snapshot.processed == 2);
    CHECK(snapshot.copied == 2);
    CHECK(snapshot.already_present == 0);
    CHECK(lardon3d_import_task_join(task));
    CHECK(lardon3d_import_task_join(task));
    lardon3d_import_task_destroy(task);
    CHECK(access(manifest, F_OK) == 0);

    task = lardon3d_import_task_create();
    CHECK(task && lardon3d_import_task_start(task, state, source));
    CHECK(wait_until_finished(task, &snapshot));
    CHECK(snapshot.status == LARDON3D_IMPORT_TASK_SUCCEEDED);
    CHECK(snapshot.copied == 0);
    CHECK(snapshot.already_present == 2);
    CHECK(lardon3d_import_task_join(task));
    lardon3d_import_task_destroy(task);
    return true;
}

static bool
run_cancellation_test(
    Lardon3DAppState *state,
    const char *source,
    const char *images,
    const char *originals,
    const char *manifest
)
{
    char manifest_before[8192];
    CHECK(read_file(manifest, manifest_before, sizeof(manifest_before)));

    Lardon3DImportTask *task = lardon3d_import_task_create();
    CHECK(task && lardon3d_import_task_start(task, state, source));
    Lardon3DImportTaskSnapshot snapshot;
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    bool copy_observed = false;
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        CHECK(lardon3d_import_task_snapshot(task, &snapshot));
        CHECK(snapshot.processed <= snapshot.total);
        if (snapshot.status != LARDON3D_IMPORT_TASK_RUNNING) {
            break;
        }
        if (snapshot.copied > 0) {
            copy_observed = true;
            break;
        }
        (void)nanosleep(&pause, NULL);
    }
    CHECK(copy_observed);
    lardon3d_import_task_request_cancel(task);
    CHECK(wait_until_finished(task, &snapshot));
    CHECK(snapshot.status == LARDON3D_IMPORT_TASK_CANCELLED);
    CHECK(lardon3d_import_task_join(task));
    lardon3d_import_task_destroy(task);

    char manifest_after[8192];
    CHECK(read_file(manifest, manifest_after, sizeof(manifest_after)));
    CHECK(strcmp(manifest_before, manifest_after) == 0);
    CHECK(!has_temporary_file(images));
    for (size_t index = 0; index < 8; ++index) {
        char filename[64];
        CHECK(snprintf(filename, sizeof(filename), "large-%zu.jpg", index) > 0);
        char destination[PATH_MAX];
        CHECK(join_path(destination, originals, filename));
        CHECK(access(destination, F_OK) != 0);
    }
    return true;
}

static bool
run_test(void)
{
    char base[] = "/tmp/lardon3d-import-task-test.XXXXXX";
    CHECK(mkdtemp(base));
    char project[PATH_MAX];
    char images[PATH_MAX];
    char originals[PATH_MAX];
    char source[PATH_MAX];
    char large_source[PATH_MAX];
    char manifest[PATH_MAX];
    CHECK(join_path(project, base, "project"));
    CHECK(join_path(images, project, "images"));
    CHECK(join_path(originals, images, "originals"));
    CHECK(join_path(source, base, "source"));
    CHECK(join_path(large_source, base, "large-source"));
    CHECK(join_path(manifest, images, "manifest.tsv"));
    CHECK(mkdir(project, 0755) == 0);
    CHECK(mkdir(images, 0755) == 0);
    CHECK(mkdir(source, 0755) == 0);
    CHECK(mkdir(large_source, 0755) == 0);

    char path[PATH_MAX];
    CHECK(join_path(path, source, "one.jpg"));
    CHECK(create_file(path, 3));
    CHECK(join_path(path, source, "two.png"));
    CHECK(create_file(path, 7));
    for (size_t index = 0; index < 8; ++index) {
        char filename[64];
        CHECK(snprintf(filename, sizeof(filename), "large-%zu.jpg", index) > 0);
        CHECK(join_path(path, large_source, filename));
        CHECK(create_file(path, 16 * 1024 * 1024));
    }

    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    state.project_loaded = true;
    CHECK(snprintf(
        state.project_path,
        sizeof(state.project_path),
        "%s",
        project
    ) > 0);
    CHECK(run_success_tests(&state, source, manifest));
    CHECK(run_cancellation_test(
        &state,
        large_source,
        images,
        originals,
        manifest
    ));
    CHECK(remove_tree(base));
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
