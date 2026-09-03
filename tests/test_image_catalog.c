#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <lardon3d/app_state.h>
#include <lardon3d/image_catalog.h>

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
write_all(int descriptor, const char *content, size_t length)
{
    size_t total = 0;
    while (total < length) {
        ssize_t written = write(descriptor, content + total, length - total);
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
create_file(const char *path, const char *content)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        return false;
    }
    bool success = write_all(descriptor, content, strlen(content));
    if (close(descriptor) != 0) {
        success = false;
    }
    return success;
}

static bool
replace_manifest(const char *path, const char *content)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        return false;
    }
    return create_file(path, content);
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
expect_invalid(
    const Lardon3DAppState *state,
    const char *manifest,
    const char *content
)
{
    CHECK(replace_manifest(manifest, content));
    char error[256];
    Lardon3DImageCatalog *catalog = lardon3d_image_catalog_load(
        state,
        error,
        sizeof(error)
    );
    CHECK(!catalog);
    CHECK(error[0]);
    lardon3d_image_catalog_destroy(catalog);
    return true;
}

static bool
test_sizes(void)
{
    char text[64];
    lardon3d_image_catalog_format_size(512, text, sizeof(text));
    CHECK(strcmp(text, "512 octets") == 0);
    lardon3d_image_catalog_format_size(1536, text, sizeof(text));
    CHECK(strcmp(text, "1,5 Kio") == 0);
    lardon3d_image_catalog_format_size(2 * 1024 * 1024, text, sizeof(text));
    CHECK(strcmp(text, "2,0 Mio") == 0);
    lardon3d_image_catalog_format_size(
        UINT64_C(3) * 1024 * 1024 * 1024,
        text,
        sizeof(text)
    );
    CHECK(strcmp(text, "3,0 Gio") == 0);
    lardon3d_image_catalog_format_size(0, NULL, 0);
    return true;
}

static bool
run_test(void)
{
    char base[] = "/tmp/lardon3d-catalog-test.XXXXXX";
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

    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    state.project_loaded = true;
    CHECK(snprintf(
        state.project_path,
        sizeof(state.project_path),
        "%s",
        project
    ) > 0);

    char error[256];
    Lardon3DImageCatalog *catalog = lardon3d_image_catalog_load(
        &state,
        error,
        sizeof(error)
    );
    CHECK(catalog);
    CHECK(lardon3d_image_catalog_count(catalog) == 0);
    CHECK(strcmp(error, "No imported image.") == 0);
    CHECK(lardon3d_image_catalog_get(catalog, 0) == NULL);
    lardon3d_image_catalog_destroy(catalog);
    lardon3d_image_catalog_destroy(NULL);

    char first[PATH_MAX];
    char utf8[PATH_MAX];
    char link_path[PATH_MAX];
    CHECK(join_path(first, originals, "première image.jpg"));
    CHECK(join_path(utf8, originals, "été.png"));
    CHECK(join_path(link_path, originals, "lien.jpg"));
    CHECK(create_file(first, "abc"));
    CHECK(create_file(utf8, "12345"));

    const char valid_manifest[] =
        "filename\tsize_bytes\tsource_path\n"
        "première image.jpg\t3\t/tmp/source avec espaces/première image.jpg\n"
        "été.png\t5\t/tmp/source/été.png\n";
    CHECK(replace_manifest(manifest, valid_manifest));
    catalog = lardon3d_image_catalog_load(&state, error, sizeof(error));
    CHECK(catalog);
    CHECK(error[0] == '\0');
    CHECK(lardon3d_image_catalog_count(catalog) == 2);
    CHECK(lardon3d_image_catalog_total_size(catalog) == 8);
    const Lardon3DImageEntry *entry = lardon3d_image_catalog_get(catalog, 0);
    CHECK(entry);
    CHECK(strcmp(entry->filename, "première image.jpg") == 0);
    CHECK(strcmp(
        entry->source_path,
        "/tmp/source avec espaces/première image.jpg"
    ) == 0);
    CHECK(entry->size_bytes == 3);
    entry = lardon3d_image_catalog_get(catalog, 1);
    CHECK(entry && strcmp(entry->filename, "été.png") == 0);
    CHECK(lardon3d_image_catalog_get(catalog, 2) == NULL);
    lardon3d_image_catalog_destroy(catalog);

    CHECK(expect_invalid(
        &state,
        manifest,
        "bad\theader\npremière image.jpg\t3\t/tmp/source\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\npremière image.jpg\t3\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\npremière image.jpg\t3\t/a\textra\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\npremière image.jpg\t12x\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\n"
        "première image.jpg\t18446744073709551616\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\n\t3\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\ndir/image.jpg\t3\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\ndir\\image.jpg\t3\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\npremière image.jpg\t3\t\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\n"
        "première image.jpg\t3\t/a\npremière image.jpg\t3\t/b\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\nabsente.jpg\t1\t/a\n"
    ));
    CHECK(symlink(first, link_path) == 0);
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\nlien.jpg\t3\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\npremière image.jpg\t4\t/a\n"
    ));
    CHECK(expect_invalid(
        &state,
        manifest,
        "filename\tsize_bytes\tsource_path\npremière image.jpg\t3\t/a"
    ));

    CHECK(replace_manifest(manifest, valid_manifest));
    catalog = lardon3d_image_catalog_load(&state, error, sizeof(error));
    CHECK(catalog && lardon3d_image_catalog_count(catalog) == 2);
    lardon3d_image_catalog_destroy(catalog);
    CHECK(test_sizes());
    CHECK(remove_tree(base));
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
