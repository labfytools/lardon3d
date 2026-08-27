#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/image_catalog.h>

Lardon3DImageCatalogAssetPublishResult lardon3d_image_catalog_test_copy_hash(
    int input, int output);

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
write_file(const char *path, const char *content)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return false;
    size_t size = strlen(content), offset = 0;
    while (offset < size) {
        ssize_t written = write(descriptor, content + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { (void)close(descriptor); return false; }
        offset += (size_t)written;
    }
    return close(descriptor) == 0;
}

static bool
canonical_asset_path(const char *path)
{
    static const char prefix[] = "assets/images/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *short_hash = path + sizeof(prefix) - 1;
    if (strlen(short_hash) != 2 + 1 + 64 || short_hash[2] != '/') return false;
    for (size_t index = 0; index < 2; ++index) {
        if (short_hash[index] != short_hash[index + 3]) return false;
    }
    for (size_t index = 0; index < 64; ++index) {
        char character = short_hash[index + 3];
        if (!((character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f'))) return false;
    }
    return true;
}

static bool
remove_tree(const char *path)
{
    struct stat information;
    if (lstat(path, &information) != 0) return errno == ENOENT;
    if (!S_ISDIR(information.st_mode)) return unlink(path) == 0;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool success = true;
    for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        if (!join_path(child, path, entry->d_name) || !remove_tree(child)) success = false;
    }
    if (closedir(directory) != 0 || rmdir(path) != 0) success = false;
    return success;
}

static size_t
count_regular_files(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) return 0;
    size_t count = 0;
    for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[PATH_MAX]; struct stat information;
        if (!join_path(child, path, entry->d_name) || lstat(child, &information) != 0)
            continue;
        if (S_ISDIR(information.st_mode)) count += count_regular_files(child);
        else if (S_ISREG(information.st_mode)) ++count;
    }
    (void)closedir(directory);
    return count;
}

typedef struct {
    Lardon3DAppState *state;
    uint64_t scanset_id;
    const char *path;
    Lardon3DImageCatalogImportResult result;
    uint64_t image_id;
} ImportThread;

typedef struct {
    Lardon3DProjectDb *database;
    uint64_t capture_id;
    uint64_t asset_id;
    Lardon3DProjectDbResult result;
} AttachSourceThread;

static void *
import_thread(void *userdata)
{
    ImportThread *thread = userdata;
    Lardon3DProjectDbImage image;
    Lardon3DProjectDbImageAsset asset;
    thread->result = lardon3d_image_catalog_import_file(thread->state,
        thread->scanset_id, thread->path, 0, &image, &asset);
    thread->image_id = image.image_id;
    return NULL;
}

static void *
attach_source_thread(void *userdata)
{
    AttachSourceThread *thread = userdata;
    thread->result = lardon3d_project_db_attach_capture_source_asset(thread->database,
        thread->capture_id, thread->asset_id);
    return NULL;
}

static bool
has_initial_capture(Lardon3DProjectDb *database, const Lardon3DProjectDbImage *image,
    const Lardon3DProjectDbImageAsset *asset, uint64_t scanset_id,
    Lardon3DProjectDbCapture *capture)
{
    if (lardon3d_project_db_find_capture_for_image(database, image->image_id, capture)
            != LARDON3D_PROJECT_DB_OK
        || capture->scanset_id != scanset_id) return false;
    uint64_t selected_image = 0;
    if (lardon3d_project_db_get_selected_capture_image(database, capture->capture_id,
            &selected_image) != LARDON3D_PROJECT_DB_OK
        || selected_image != image->image_id) return false;
    Lardon3DProjectDbCaptureAsset capture_asset;
    size_t count = 0;
    return lardon3d_project_db_list_capture_assets(database, capture->capture_id, 0,
               &capture_asset, 1, &count) == LARDON3D_PROJECT_DB_OK
        && count == 1 && capture_asset.asset_id == asset->asset_id
        && capture_asset.role == LARDON3D_DB_CAPTURE_ASSET_SOURCE;
}

static bool
run_test(void)
{
    char root[] = "/tmp/lardon3d-catalog-v1-XXXXXX";
    CHECK(mkdtemp(root));
    char database_path[PATH_MAX], source_a[PATH_MAX], source_b[PATH_MAX], copied_path[PATH_MAX];
    CHECK(join_path(database_path, root, "project.db"));
    CHECK(join_path(source_a, root, "photo001.jpg"));
    CHECK(join_path(source_b, root, "other-name.jpg"));
    CHECK(join_path(copied_path, root, "copy.bin"));
    CHECK(write_file(source_a, "same-image-content"));
    CHECK(write_file(source_b, "same-image-content"));
    int copy_source = open(source_a, O_RDONLY);
    int copy_destination = open(copied_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(copy_source >= 0 && copy_destination >= 0);
    CHECK(lardon3d_image_catalog_test_copy_hash(copy_source, copy_destination)
        == LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
    CHECK(close(copy_source) == 0 && close(copy_destination) == 0);
    struct stat full_information;
    CHECK(stat("/dev/full", &full_information) == 0 && S_ISCHR(full_information.st_mode));
    copy_source = open(source_a, O_RDONLY);
    copy_destination = open("/dev/full", O_WRONLY);
    CHECK(copy_source >= 0 && copy_destination >= 0);
    CHECK(lardon3d_image_catalog_test_copy_hash(copy_source, copy_destination)
        == LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR);
    CHECK(close(copy_source) == 0 && close(copy_destination) == 0);
    Lardon3DProjectDb *database = NULL;
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    CHECK(lardon3d_project_db_open(database_path, &database, error)
        == LARDON3D_PROJECT_DB_OK);
    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    state.project_loaded = true;
    state.project_db = database;
    CHECK(snprintf(state.project_path, sizeof(state.project_path), "%s", root) > 0);

    Lardon3DProjectDbScanSet a, b;
    CHECK(lardon3d_image_catalog_create_scanset(&state, "Campagne générale", &a));
    CHECK(lardon3d_image_catalog_create_scanset(&state, "Pièce démontée", &b));
    CHECK(a.scanset_id != b.scanset_id);
    Lardon3DProjectDbCapture preexisting_capture;
    CHECK(lardon3d_project_db_create_capture(database, a.scanset_id, 1,
        &preexisting_capture) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbScanSet scanset_page[2]; size_t scanset_count = 0;
    CHECK(lardon3d_project_db_list_scansets(database, 0, scanset_page, 1,
        &scanset_count) == LARDON3D_PROJECT_DB_OK && scanset_count == 1
        && scanset_page[0].scanset_id == a.scanset_id);
    CHECK(lardon3d_project_db_list_scansets(database, a.scanset_id,
        scanset_page, 2, &scanset_count) == LARDON3D_PROJECT_DB_OK
        && scanset_count == 1 && scanset_page[0].scanset_id == b.scanset_id);
    uint64_t count = 99;
    CHECK(lardon3d_project_db_count_images(database, a.scanset_id, &count)
        == LARDON3D_PROJECT_DB_OK && count == 0);
    unsigned char zero_hash[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0};
    Lardon3DProjectDbImageRegisterStatus invalid_status;
    Lardon3DProjectDbImage invalid_image;
    CHECK(lardon3d_project_db_register_image(database, a.scanset_id, zero_hash,
        "../../outside", 1, "x.jpg", "/source/x.jpg", 0, 1,
        &invalid_status, &invalid_image) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
    CHECK(lardon3d_project_db_count_images(database, UINT64_C(999999), &count)
        == LARDON3D_PROJECT_DB_NOT_FOUND);

    Lardon3DProjectDbImage image_a, duplicate_a, image_b;
    Lardon3DProjectDbImageAsset asset_a, duplicate_asset, asset_b;
    CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id, source_a, 0,
        &image_a, &asset_a) == LARDON3D_IMAGE_CATALOG_IMPORTED);
    CHECK(canonical_asset_path(asset_a.path));
    Lardon3DProjectDbCapture capture_a, capture_b, capture_different;
    CHECK(has_initial_capture(database, &image_a, &asset_a, a.scanset_id, &capture_a));
    CHECK(capture_a.capture_id != image_a.image_id);
    CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id, source_b, 0,
        &duplicate_a, &duplicate_asset) == LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT);
    CHECK(image_a.image_id == duplicate_a.image_id
        && asset_a.asset_id == duplicate_asset.asset_id);
    CHECK(has_initial_capture(database, &duplicate_a, &duplicate_asset, a.scanset_id, &capture_a));
    CHECK(lardon3d_image_catalog_import_file(&state, b.scanset_id, source_b, 0,
        &image_b, &asset_b) == LARDON3D_IMAGE_CATALOG_IMPORTED);
    CHECK(image_b.image_id != image_a.image_id && asset_b.asset_id == asset_a.asset_id);
    CHECK(strcmp(asset_b.path, asset_a.path) == 0);
    CHECK(has_initial_capture(database, &image_b, &asset_b, b.scanset_id, &capture_b));
    CHECK(capture_b.capture_id != capture_a.capture_id);

    char different_dir[PATH_MAX], same_name[PATH_MAX];
    CHECK(join_path(different_dir, root, "different") && mkdir(different_dir, 0700) == 0);
    CHECK(join_path(same_name, different_dir, "photo001.jpg"));
    CHECK(write_file(same_name, "different-content"));
    Lardon3DProjectDbImage different_image;
    Lardon3DProjectDbImageAsset different_asset;
    CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id, same_name, 0,
        &different_image, &different_asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
    CHECK(different_image.image_id != image_a.image_id
        && different_asset.asset_id != asset_a.asset_id
        && canonical_asset_path(different_asset.path)
        && strcmp(different_asset.path, asset_a.path) != 0);
    CHECK(has_initial_capture(database, &different_image, &different_asset, a.scanset_id,
        &capture_different));
    CHECK(capture_different.capture_id != capture_a.capture_id);

    char orphan_source[PATH_MAX], asset_root[PATH_MAX];
    CHECK(join_path(orphan_source, root, "orphan.jpg"));
    CHECK(join_path(asset_root, root, "assets/images"));
    CHECK(write_file(orphan_source, "published-before-db-failure"));
    size_t files_before = count_regular_files(asset_root);
    uint64_t logical_before = 0;
    CHECK(lardon3d_project_db_count_images(database, a.scanset_id,
        &logical_before) == LARDON3D_PROJECT_DB_OK);
    CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_IMAGE_REGISTER", "1", 1) == 0);
    CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id,
        orphan_source, 0, &different_image, &different_asset)
        == LARDON3D_IMAGE_CATALOG_DB_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_IMAGE_REGISTER") == 0);
    CHECK(count_regular_files(asset_root) == files_before + 1);
    uint64_t logical_after = 0;
    CHECK(lardon3d_project_db_count_images(database, a.scanset_id,
        &logical_after) == LARDON3D_PROJECT_DB_OK
        && logical_after == logical_before);

    char concurrent_path[PATH_MAX];
    CHECK(join_path(concurrent_path, root, "concurrent.jpg"));
    CHECK(write_file(concurrent_path, "concurrent-content"));
    ImportThread contexts[2] = {
        {.state = &state, .scanset_id = a.scanset_id, .path = concurrent_path},
        {.state = &state, .scanset_id = a.scanset_id, .path = concurrent_path},
    };
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, import_thread, &contexts[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, import_thread, &contexts[1]) == 0);
    CHECK(pthread_join(threads[0], NULL) == 0 && pthread_join(threads[1], NULL) == 0);
    CHECK(((contexts[0].result == LARDON3D_IMAGE_CATALOG_IMPORTED
            && contexts[1].result == LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT)
        || (contexts[1].result == LARDON3D_IMAGE_CATALOG_IMPORTED
            && contexts[0].result == LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT))
        && contexts[0].image_id == contexts[1].image_id);

    /* S3-C: explicit, idempotent SOURCE association does not mutate catalog policy. */
    Lardon3DProjectDbImage concurrent_image;
    Lardon3DProjectDbImageAsset concurrent_asset;
    CHECK(lardon3d_project_db_load_image(database, contexts[0].image_id,
        &concurrent_image, &concurrent_asset) == LARDON3D_PROJECT_DB_OK);
    uint64_t image_count_a_before = 0, image_count_b_before = 0;
    CHECK(lardon3d_project_db_count_images(database, a.scanset_id,
        &image_count_a_before) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_count_images(database, b.scanset_id,
        &image_count_b_before) == LARDON3D_PROJECT_DB_OK);
    uint64_t selection_before = 0;
    CHECK(lardon3d_project_db_get_selected_capture_image(database, capture_a.capture_id,
        &selection_before) == LARDON3D_PROJECT_DB_OK
        && selection_before == image_a.image_id);

    CHECK(lardon3d_project_db_attach_capture_source_asset(database, capture_a.capture_id,
        different_asset.asset_id) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_attach_capture_source_asset(database, capture_a.capture_id,
        different_asset.asset_id) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_attach_capture_source_asset(database, capture_a.capture_id,
        concurrent_asset.asset_id) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbCaptureAsset source_page[4];
    size_t source_count = 0;
    CHECK(lardon3d_project_db_list_capture_assets(database, capture_a.capture_id, 0,
        source_page, 4, &source_count) == LARDON3D_PROJECT_DB_OK && source_count == 3);
    for (size_t index = 0; index < source_count; ++index) {
        CHECK(source_page[index].capture_id == capture_a.capture_id
            && source_page[index].role == LARDON3D_DB_CAPTURE_ASSET_SOURCE
            && (index == 0 || source_page[index - 1].asset_id < source_page[index].asset_id));
    }

    AttachSourceThread attach_contexts[2] = {
        {.database = database, .capture_id = preexisting_capture.capture_id,
            .asset_id = asset_a.asset_id},
        {.database = database, .capture_id = preexisting_capture.capture_id,
            .asset_id = asset_a.asset_id},
    };
    pthread_t attach_threads[2];
    CHECK(pthread_create(&attach_threads[0], NULL, attach_source_thread,
        &attach_contexts[0]) == 0);
    CHECK(pthread_create(&attach_threads[1], NULL, attach_source_thread,
        &attach_contexts[1]) == 0);
    CHECK(pthread_join(attach_threads[0], NULL) == 0
        && pthread_join(attach_threads[1], NULL) == 0
        && attach_contexts[0].result == LARDON3D_PROJECT_DB_OK
        && attach_contexts[1].result == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_attach_capture_source_asset(database, capture_b.capture_id,
        asset_a.asset_id) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbCapture conflict_capture;
    CHECK(lardon3d_project_db_create_capture(database, a.scanset_id, 2,
        &conflict_capture) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_attach_capture_asset(database, conflict_capture.capture_id,
        concurrent_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_DERIVED)
        == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_attach_capture_source_asset(database,
        conflict_capture.capture_id, concurrent_asset.asset_id)
        == LARDON3D_PROJECT_DB_CONSTRAINT);
    size_t conflict_count = 0;
    CHECK(lardon3d_project_db_list_capture_assets(database, conflict_capture.capture_id, 0,
        source_page, 1, &conflict_count) == LARDON3D_PROJECT_DB_OK
        && conflict_count == 1
        && source_page[0].role == LARDON3D_DB_CAPTURE_ASSET_DERIVED);

    lardon3d_project_db_close(database);
    database = NULL;
    state.project_db = NULL;
    CHECK(lardon3d_project_db_open(database_path, &database, error)
        == LARDON3D_PROJECT_DB_OK);
    state.project_db = database;
    CHECK(lardon3d_project_db_attach_capture_source_asset(database, capture_a.capture_id,
        different_asset.asset_id) == LARDON3D_PROJECT_DB_OK);
    source_count = 0;
    CHECK(lardon3d_project_db_list_capture_assets(database, capture_a.capture_id, 0,
        source_page, 4, &source_count) == LARDON3D_PROJECT_DB_OK && source_count == 3);
    uint64_t selection_after = 0, image_count_a_after = 0, image_count_b_after = 0;
    CHECK(lardon3d_project_db_get_selected_capture_image(database, capture_a.capture_id,
        &selection_after) == LARDON3D_PROJECT_DB_OK && selection_after == selection_before);
    CHECK(lardon3d_project_db_count_images(database, a.scanset_id,
        &image_count_a_after) == LARDON3D_PROJECT_DB_OK
        && image_count_a_after == image_count_a_before);
    CHECK(lardon3d_project_db_count_images(database, b.scanset_id,
        &image_count_b_after) == LARDON3D_PROJECT_DB_OK
        && image_count_b_after == image_count_b_before);

    char bulk_directory[PATH_MAX];
    CHECK(join_path(bulk_directory, root, "bulk") && mkdir(bulk_directory, 0700) == 0);
    for (uint64_t index = 1; index <= 2048; ++index) {
        char name[64], source[PATH_MAX], content[64];
        CHECK(snprintf(name, sizeof(name), "bulk-%llu.jpg",
            (unsigned long long)index) > 0);
        CHECK(join_path(source, bulk_directory, name));
        CHECK(snprintf(content, sizeof(content), "unique-content-%llu",
            (unsigned long long)index) > 0);
        CHECK(write_file(source, content));
        Lardon3DProjectDbImage bulk_image;
        Lardon3DProjectDbImageAsset bulk_asset;
        CHECK(lardon3d_image_catalog_import_file(&state, a.scanset_id, source,
            0, &bulk_image, &bulk_asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
    }
    CHECK(lardon3d_project_db_count_images(database, a.scanset_id, &count)
        == LARDON3D_PROJECT_DB_OK && count == 2051);
    Lardon3DProjectDbImage page[8]; Lardon3DProjectDbImageAsset assets[8];
    size_t page_count = 0; uint64_t cursor = 0, visited = 0;
    while (lardon3d_image_catalog_list(&state, a.scanset_id, cursor, page,
            assets, 8, &page_count) == LARDON3D_PROJECT_DB_OK && page_count) {
        for (size_t index = 0; index < page_count; ++index) {
            CHECK(page[index].image_id > cursor);
            cursor = page[index].image_id; ++visited;
        }
    }
    CHECK(visited == count);
    size_t one_count = 0;
    CHECK(lardon3d_image_catalog_list(&state, a.scanset_id, 0, page, assets,
        1, &one_count) == LARDON3D_PROJECT_DB_OK && one_count == 1);
    Lardon3DProjectDbImage maximum_page[LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX];
    Lardon3DProjectDbImageAsset maximum_assets[LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX];
    size_t maximum_count = 0;
    CHECK(lardon3d_image_catalog_list(&state, a.scanset_id, 0, maximum_page,
        maximum_assets, LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX, &maximum_count)
        == LARDON3D_PROJECT_DB_OK
        && maximum_count == LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX);
    CHECK(lardon3d_image_catalog_list(&state, a.scanset_id, 0, page, assets,
        LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX + 1, &page_count)
        == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
    CHECK(lardon3d_image_catalog_list(&state, UINT64_C(999999), 0, page,
        assets, 8, &page_count) == LARDON3D_PROJECT_DB_NOT_FOUND);
    CHECK(unlink(source_a) == 0 && unlink(source_b) == 0);
    Lardon3DProjectDbImage loaded; Lardon3DProjectDbImageAsset loaded_asset;
    CHECK(lardon3d_project_db_load_image(database, image_a.image_id, &loaded,
        &loaded_asset) == LARDON3D_PROJECT_DB_OK
        && loaded.asset_id == asset_a.asset_id);

    lardon3d_project_db_close(database);
    state.project_db = NULL;
    CHECK(remove_tree(root));
    return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
