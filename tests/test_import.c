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
#include <lardon3d/import.h>

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
create_directory(const char *path)
{
    return mkdir(path, 0755) == 0;
}

static bool
create_file(const char *path, const char *content)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        return false;
    }

    size_t length = strlen(content);
    size_t written_total = 0;
    while (written_total < length) {
        ssize_t written = write(
            descriptor,
            content + written_total,
            length - written_total
        );
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            (void)close(descriptor);
            return false;
        }
        written_total += (size_t)written;
    }
    return close(descriptor) == 0;
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
file_equals(const char *path, const char *expected)
{
    char content[256];
    return read_file(path, content, sizeof(content))
        && strcmp(content, expected) == 0;
}

static bool
manifest_has_unique_filenames(const char *path, size_t expected_entries)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }

    char filenames[16][NAME_MAX + 1];
    size_t count = 0;
    char line[PATH_MAX + 512];
    bool valid = fgets(line, sizeof(line), file)
        && strcmp(line, "filename\tsize_bytes\tsource_path\n") == 0;
    while (valid && fgets(line, sizeof(line), file)) {
        char *tab = strchr(line, '\t');
        if (!tab || count >= 16) {
            valid = false;
            break;
        }
        *tab = '\0';
        for (size_t index = 0; index < count; ++index) {
            if (strcmp(filenames[index], line) == 0) {
                valid = false;
            }
        }
        int written = snprintf(
            filenames[count],
            sizeof(filenames[count]),
            "%s",
            line
        );
        if (written < 0 || (size_t)written >= sizeof(filenames[count])) {
            valid = false;
        }
        ++count;
    }
    if (ferror(file) || fclose(file) != 0) {
        valid = false;
    }
    return valid && count == expected_entries;
}

static bool
has_manifest_temporary(const char *images_path)
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
run_test(void)
{
    char base[] = "/tmp/lardon3d-import-test.XXXXXX";
    CHECK(mkdtemp(base));

    char project[PATH_MAX];
    char images[PATH_MAX];
    char originals[PATH_MAX];
    char valid_source[PATH_MAX];
    char tab_source[PATH_MAX];
    char newline_source[PATH_MAX];
    CHECK(join_path(project, base, "project"));
    CHECK(join_path(images, project, "images"));
    CHECK(join_path(originals, images, "originals"));
    CHECK(join_path(valid_source, base, "valid"));
    CHECK(join_path(tab_source, base, "tab"));
    CHECK(join_path(newline_source, base, "newline"));
    CHECK(create_directory(project));
    CHECK(create_directory(images));
    CHECK(create_directory(valid_source));
    CHECK(create_directory(tab_source));
    CHECK(create_directory(newline_source));

    char path[PATH_MAX];
    CHECK(join_path(path, valid_source, "one.jpg"));
    CHECK(create_file(path, "one"));
    CHECK(join_path(path, valid_source, "two.PNG"));
    CHECK(create_file(path, "two-two"));

    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    state.project_loaded = true;
    CHECK(snprintf(
        state.project_path,
        sizeof(state.project_path),
        "%s",
        project
    ) > 0);

    Lardon3DImportResult result;
    CHECK(lardon3d_import_directory(&state, valid_source, &result));
    CHECK(result.admissible_found == 2);
    CHECK(result.copied == 2);
    CHECK(result.already_present == 0);

    char manifest[PATH_MAX];
    CHECK(join_path(manifest, images, "manifest.tsv"));
    char manifest_before[8192];
    CHECK(read_file(manifest, manifest_before, sizeof(manifest_before)));

    CHECK(join_path(path, tab_source, "would-copy.jpg"));
    CHECK(create_file(path, "must-not-copy"));
    CHECK(join_path(path, tab_source, "bad\tname.jpg"));
    CHECK(create_file(path, "invalid"));
    CHECK(!lardon3d_import_directory(&state, tab_source, &result));
    CHECK(join_path(path, originals, "would-copy.jpg"));
    CHECK(access(path, F_OK) != 0);
    char manifest_after[8192];
    CHECK(read_file(manifest, manifest_after, sizeof(manifest_after)));
    CHECK(strcmp(manifest_before, manifest_after) == 0);
    CHECK(!has_manifest_temporary(images));

    CHECK(join_path(path, newline_source, "also-not-copied.jpg"));
    CHECK(create_file(path, "must-not-copy-either"));
    CHECK(join_path(path, newline_source, "bad\nname.png"));
    CHECK(create_file(path, "invalid"));
    CHECK(!lardon3d_import_directory(&state, newline_source, &result));
    CHECK(join_path(path, originals, "also-not-copied.jpg"));
    CHECK(access(path, F_OK) != 0);
    CHECK(read_file(manifest, manifest_after, sizeof(manifest_after)));
    CHECK(strcmp(manifest_before, manifest_after) == 0);
    CHECK(!has_manifest_temporary(images));

    CHECK(join_path(path, valid_source, "three.tiff"));
    CHECK(create_file(path, "three-three-three"));
    CHECK(lardon3d_import_directory(&state, valid_source, &result));
    CHECK(result.copied == 1);
    CHECK(result.already_present == 2);
    CHECK(lardon3d_import_directory(&state, valid_source, &result));
    CHECK(result.copied == 0);
    CHECK(result.already_present == 3);
    CHECK(manifest_has_unique_filenames(manifest, 3));
    CHECK(join_path(path, originals, "one.jpg"));
    CHECK(file_equals(path, "one"));
    CHECK(join_path(path, originals, "two.PNG"));
    CHECK(file_equals(path, "two-two"));
    CHECK(join_path(path, originals, "three.tiff"));
    CHECK(file_equals(path, "three-three-three"));
    CHECK(!has_manifest_temporary(images));

    CHECK(unlink(path) == 0);
    CHECK(join_path(path, originals, "two.PNG"));
    CHECK(unlink(path) == 0);
    CHECK(join_path(path, originals, "one.jpg"));
    CHECK(unlink(path) == 0);
    CHECK(unlink(manifest) == 0);
    CHECK(rmdir(originals) == 0);
    CHECK(rmdir(images) == 0);
    CHECK(rmdir(project) == 0);

    CHECK(join_path(path, valid_source, "three.tiff"));
    CHECK(unlink(path) == 0);
    CHECK(join_path(path, valid_source, "two.PNG"));
    CHECK(unlink(path) == 0);
    CHECK(join_path(path, valid_source, "one.jpg"));
    CHECK(unlink(path) == 0);
    CHECK(rmdir(valid_source) == 0);

    CHECK(join_path(path, tab_source, "bad\tname.jpg"));
    CHECK(unlink(path) == 0);
    CHECK(join_path(path, tab_source, "would-copy.jpg"));
    CHECK(unlink(path) == 0);
    CHECK(rmdir(tab_source) == 0);

    CHECK(join_path(path, newline_source, "bad\nname.png"));
    CHECK(unlink(path) == 0);
    CHECK(join_path(path, newline_source, "also-not-copied.jpg"));
    CHECK(unlink(path) == 0);
    CHECK(rmdir(newline_source) == 0);
    CHECK(rmdir(base) == 0);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
