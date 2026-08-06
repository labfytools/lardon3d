#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/app_state.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>

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
create_file(const char *path, size_t size)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        return false;
    }
    char bytes[16] = {0};
    size_t remaining = size;
    while (remaining > 0) {
        size_t length = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        ssize_t written = write(descriptor, bytes, length);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            (void)close(descriptor);
            return false;
        }
        remaining -= (size_t)written;
    }
    return close(descriptor) == 0;
}

static bool
write_manifest(const char *path, const char *content)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        return false;
    }
    size_t length = strlen(content);
    size_t total = 0;
    while (total < length) {
        ssize_t written = write(descriptor, content + total, length - total);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            (void)close(descriptor);
            return false;
        }
        total += (size_t)written;
    }
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

static Lardon3DImageCatalog *
load_catalog(const char *project)
{
    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    state.project_loaded = true;
    (void)snprintf(state.project_path, sizeof(state.project_path), "%s", project);
    char error[256];
    return lardon3d_image_catalog_load(&state, error, sizeof(error));
}

static bool
expect_names(
    const Lardon3DImageView *view,
    const char *const *names,
    size_t count
)
{
    CHECK(lardon3d_image_view_count(view) == count);
    for (size_t index = 0; index < count; ++index) {
        const Lardon3DImageEntry *entry = lardon3d_image_view_get(view, index);
        CHECK(entry && strcmp(entry->filename, names[index]) == 0);
    }
    CHECK(lardon3d_image_view_get(view, count) == NULL);
    return true;
}

static bool
run_test(void)
{
    char base[] = "/tmp/lardon3d-image-view.XXXXXX";
    CHECK(mkdtemp(base));
    char project[PATH_MAX];
    char images[PATH_MAX];
    char originals[PATH_MAX];
    char manifest[PATH_MAX];
    CHECK(join_path(project, base, "project"));
    CHECK(join_path(images, project, "images"));
    CHECK(join_path(originals, images, "originals"));
    CHECK(join_path(manifest, images, "manifest.tsv"));
    CHECK(mkdir(project, 0755) == 0);
    CHECK(mkdir(images, 0755) == 0);
    CHECK(mkdir(originals, 0755) == 0);

    Lardon3DImageCatalog *empty_catalog = load_catalog(project);
    CHECK(empty_catalog);
    Lardon3DImageView *view = lardon3d_image_view_create(empty_catalog);
    CHECK(view);
    CHECK(lardon3d_image_view_count(view) == 0);
    CHECK(lardon3d_image_view_total_size(view) == 0);
    lardon3d_image_view_normalize(view, 0);
    CHECK(lardon3d_image_view_selection(view) == 0);
    CHECK(lardon3d_image_view_offset(view) == 0);
    lardon3d_image_view_destroy(view);
    lardon3d_image_catalog_destroy(empty_catalog);
    lardon3d_image_view_destroy(NULL);

    static const struct {
        const char *name;
        size_t size;
    } files[] = {
        {"Zulu.JPG", 5},
        {"alpha.png", 2},
        {"Beta.jpg", 5},
        {"été.png", 3},
        {"motor final.TIF", 8},
    };
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); ++index) {
        char path[PATH_MAX];
        CHECK(join_path(path, originals, files[index].name));
        CHECK(create_file(path, files[index].size));
    }
    CHECK(write_manifest(
        manifest,
        "filename\tsize_bytes\tsource_path\n"
        "Zulu.JPG\t5\t/tmp/source/Zulu.JPG\n"
        "alpha.png\t2\t/tmp/source/alpha.png\n"
        "Beta.jpg\t5\t/tmp/source/Beta.jpg\n"
        "été.png\t3\t/tmp/source avec espaces/été.png\n"
        "motor final.TIF\t8\t/tmp/source/motor final.TIF\n"
    ));
    Lardon3DImageCatalog *catalog = load_catalog(project);
    CHECK(catalog && lardon3d_image_catalog_count(catalog) == 5);
    const Lardon3DImageEntry *original_entries[5];
    for (size_t index = 0; index < 5; ++index) {
        original_entries[index] = lardon3d_image_catalog_get(catalog, index);
    }
    view = lardon3d_image_view_create(catalog);
    CHECK(view);

    static const char *const import_order[] = {
        "Zulu.JPG", "alpha.png", "Beta.jpg", "été.png", "motor final.TIF",
    };
    static const char *const name_ascending[] = {
        "Beta.jpg", "Zulu.JPG", "alpha.png", "motor final.TIF", "été.png",
    };
    static const char *const name_descending[] = {
        "été.png", "motor final.TIF", "alpha.png", "Zulu.JPG", "Beta.jpg",
    };
    static const char *const size_ascending[] = {
        "alpha.png", "été.png", "Beta.jpg", "Zulu.JPG", "motor final.TIF",
    };
    static const char *const size_descending[] = {
        "motor final.TIF", "Beta.jpg", "Zulu.JPG", "été.png", "alpha.png",
    };
    CHECK(expect_names(view, import_order, 5));
    CHECK(lardon3d_image_view_total_size(view) == 23);
    CHECK(lardon3d_image_view_rebuild(view));
    CHECK(expect_names(view, import_order, 5));
    CHECK(lardon3d_image_view_set_sort(view, LARDON3D_IMAGE_SORT_NAME_ASC));
    CHECK(expect_names(view, name_ascending, 5));
    CHECK(lardon3d_image_view_set_sort(view, LARDON3D_IMAGE_SORT_NAME_DESC));
    CHECK(expect_names(view, name_descending, 5));
    CHECK(lardon3d_image_view_set_sort(view, LARDON3D_IMAGE_SORT_SIZE_ASC));
    CHECK(expect_names(view, size_ascending, 5));
    CHECK(lardon3d_image_view_set_sort(view, LARDON3D_IMAGE_SORT_SIZE_DESC));
    CHECK(expect_names(view, size_descending, 5));
    CHECK(!lardon3d_image_view_set_sort(view, (Lardon3DImageSort)99));

    char error[256];
    CHECK(lardon3d_image_view_set_filter(view, "", error, sizeof(error)));
    CHECK(lardon3d_image_view_count(view) == 5);
    CHECK(lardon3d_image_view_set_filter(view, "JPG", error, sizeof(error)));
    CHECK(lardon3d_image_view_count(view) == 2);
    CHECK(lardon3d_image_view_total_size(view) == 10);
    CHECK(lardon3d_image_view_ascii_contains("Alpha.JPEG", "pHa.j"));
    CHECK(!lardon3d_image_view_ascii_contains("été.png", "ÉTÉ"));
    CHECK(lardon3d_image_view_set_filter(view, "été", error, sizeof(error)));
    CHECK(expect_names(view, (const char *const[]){"été.png"}, 1));
    CHECK(lardon3d_image_view_set_filter(view, "absent", error, sizeof(error)));
    CHECK(lardon3d_image_view_count(view) == 0);
    CHECK(lardon3d_image_view_selection(view) == 0);
    CHECK(lardon3d_image_view_offset(view) == 0);

    CHECK(lardon3d_image_view_set_filter(view, "", error, sizeof(error)));
    CHECK(lardon3d_image_view_set_sort(view, LARDON3D_IMAGE_SORT_IMPORT_ORDER));
    lardon3d_image_view_select(view, 2, 2);
    size_t selected_catalog;
    CHECK(lardon3d_image_view_catalog_index(view, 2, &selected_catalog));
    CHECK(selected_catalog == 2);
    CHECK(!lardon3d_image_view_catalog_index(view, 99, &selected_catalog));
    CHECK(lardon3d_image_view_set_sort(view, LARDON3D_IMAGE_SORT_NAME_ASC));
    CHECK(strcmp(
        lardon3d_image_view_get(
            view,
            lardon3d_image_view_selection(view)
        )->filename,
        "Beta.jpg"
    ) == 0);
    CHECK(lardon3d_image_view_set_filter(view, "beta", error, sizeof(error)));
    CHECK(lardon3d_image_view_selection(view) == 0);
    CHECK(lardon3d_image_view_set_filter(view, "alpha", error, sizeof(error)));
    CHECK(lardon3d_image_view_selection(view) == 0);
    CHECK(strcmp(lardon3d_image_view_get(view, 0)->filename, "alpha.png") == 0);

    char too_long[LARDON3D_IMAGE_FILTER_CAPACITY + 1];
    (void)memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!lardon3d_image_view_set_filter(view, too_long, error, sizeof(error)));
    CHECK(error[0]);
    CHECK(strcmp(lardon3d_image_view_filter(view), "alpha") == 0);

    for (size_t index = 0; index < 5; ++index) {
        CHECK(lardon3d_image_catalog_get(catalog, index) == original_entries[index]);
        CHECK(strcmp(original_entries[index]->filename, import_order[index]) == 0);
    }

    char small_project[PATH_MAX];
    char small_images[PATH_MAX];
    char small_originals[PATH_MAX];
    char small_manifest[PATH_MAX];
    CHECK(join_path(small_project, base, "small"));
    CHECK(join_path(small_images, small_project, "images"));
    CHECK(join_path(small_originals, small_images, "originals"));
    CHECK(join_path(small_manifest, small_images, "manifest.tsv"));
    CHECK(mkdir(small_project, 0755) == 0);
    CHECK(mkdir(small_images, 0755) == 0);
    CHECK(mkdir(small_originals, 0755) == 0);
    char only[PATH_MAX];
    CHECK(join_path(only, small_originals, "only.jpg"));
    CHECK(create_file(only, 1));
    CHECK(write_manifest(
        small_manifest,
        "filename\tsize_bytes\tsource_path\nonly.jpg\t1\t/tmp/only.jpg\n"
    ));
    Lardon3DImageCatalog *small_catalog = load_catalog(small_project);
    CHECK(small_catalog);
    CHECK(lardon3d_image_view_set_filter(view, "", error, sizeof(error)));
    CHECK(lardon3d_image_view_set_catalog(view, small_catalog));
    CHECK(lardon3d_image_view_count(view) == 1);
    CHECK(strcmp(lardon3d_image_view_get(view, 0)->filename, "only.jpg") == 0);
    lardon3d_image_view_select(view, 100, 3);
    CHECK(lardon3d_image_view_selection(view) == 0);
    CHECK(lardon3d_image_view_offset(view) == 0);

    lardon3d_image_view_destroy(view);
    lardon3d_image_catalog_destroy(small_catalog);
    lardon3d_image_catalog_destroy(catalog);
    CHECK(remove_tree(base));
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
