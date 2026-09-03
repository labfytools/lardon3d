#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <lardon3d/image_catalog.h>

struct Lardon3DImageCatalog {
    Lardon3DImageEntry *entries;
    size_t count;
    size_t capacity;
    uint64_t total_size;
};

static void
set_error(char *message, size_t size, const char *text)
{
    if (message && size > 0) {
        (void)snprintf(message, size, "%s", text);
    }
}

static char *
join_path(const char *parent, const char *child)
{
    size_t parent_length = strlen(parent);
    size_t child_length = strlen(child);
    if (parent_length > SIZE_MAX - child_length - 2) {
        return NULL;
    }
    size_t size = parent_length + child_length + 2;
    char *path = malloc(size);
    if (!path) {
        return NULL;
    }
    int written = snprintf(path, size, "%s/%s", parent, child);
    if (written < 0 || (size_t)written >= size) {
        free(path);
        return NULL;
    }
    return path;
}

static bool
parse_size(const char *text, uint64_t *value)
{
    if (!text[0]) {
        return false;
    }
    uint64_t parsed = 0;
    for (const unsigned char *digit = (const unsigned char *)text;
         *digit;
         ++digit) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
        uint64_t number = (uint64_t)(*digit - '0');
        if (parsed > (UINT64_MAX - number) / 10) {
            return false;
        }
        parsed = parsed * 10 + number;
    }
    *value = parsed;
    return true;
}

static bool
valid_filename(const char *filename)
{
    return filename[0]
        && !strchr(filename, '/')
        && !strchr(filename, '\\')
        && !strchr(filename, '\t')
        && !strchr(filename, '\r')
        && !strchr(filename, '\n');
}

static bool
filename_exists(
    const Lardon3DImageCatalog *catalog,
    const char *filename
)
{
    for (size_t index = 0; index < catalog->count; ++index) {
        if (strcmp(catalog->entries[index].filename, filename) == 0) {
            return true;
        }
    }
    return false;
}

static bool
append_entry(
    Lardon3DImageCatalog *catalog,
    const char *filename,
    const char *source_path,
    uint64_t size_bytes
)
{
    if (catalog->count == catalog->capacity) {
        size_t capacity = catalog->capacity == 0 ? 16 : catalog->capacity * 2;
        if (capacity < catalog->capacity
            || capacity > SIZE_MAX / sizeof(*catalog->entries)) {
            return false;
        }
        void *entries = realloc(
            catalog->entries,
            capacity * sizeof(*catalog->entries)
        );
        if (!entries) {
            return false;
        }
        catalog->entries = entries;
        catalog->capacity = capacity;
    }

    char *filename_copy = strdup(filename);
    char *source_copy = strdup(source_path);
    if (!filename_copy || !source_copy) {
        free(filename_copy);
        free(source_copy);
        return false;
    }
    catalog->entries[catalog->count] = (Lardon3DImageEntry) {
        .filename = filename_copy,
        .source_path = source_copy,
        .size_bytes = size_bytes,
    };
    ++catalog->count;
    catalog->total_size += size_bytes;
    return true;
}

void
lardon3d_image_catalog_destroy(Lardon3DImageCatalog *catalog)
{
    if (!catalog) {
        return;
    }
    for (size_t index = 0; index < catalog->count; ++index) {
        free(catalog->entries[index].filename);
        free(catalog->entries[index].source_path);
    }
    free(catalog->entries);
    free(catalog);
}

static bool
validate_file(
    const char *originals_path,
    const char *filename,
    uint64_t expected_size,
    char *error_message,
    size_t error_message_size
)
{
    char *path = join_path(originals_path, filename);
    if (!path) {
        set_error(error_message, error_message_size, "Error: image path is too long.");
        return false;
    }
    struct stat info;
    if (lstat(path, &info) != 0) {
        free(path);
        set_error(error_message, error_message_size, "Error: manifest image is missing.");
        return false;
    }
    free(path);
    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {
        set_error(error_message, error_message_size, "Error: manifest image is not a regular file.");
        return false;
    }
    if (info.st_size < 0 || (uintmax_t)info.st_size != (uintmax_t)expected_size) {
        set_error(error_message, error_message_size, "Error: inconsistent image size.");
        return false;
    }
    return true;
}

static bool
parse_line(
    Lardon3DImageCatalog *catalog,
    char *line,
    const char *originals_path,
    char *error_message,
    size_t error_message_size
)
{
    char *first_tab = strchr(line, '\t');
    char *second_tab = first_tab ? strchr(first_tab + 1, '\t') : NULL;
    if (!first_tab || !second_tab || strchr(second_tab + 1, '\t')) {
        set_error(error_message, error_message_size, "Error: invalid manifest line.");
        return false;
    }
    *first_tab = '\0';
    *second_tab = '\0';
    const char *filename = line;
    const char *size_text = first_tab + 1;
    const char *source_path = second_tab + 1;

    uint64_t size_bytes;
    if (!valid_filename(filename)) {
        set_error(error_message, error_message_size, "Error: invalid image name in manifest.");
        return false;
    }
    if (!source_path[0]) {
        set_error(error_message, error_message_size, "Error: empty source path in manifest.");
        return false;
    }
    if (!parse_size(size_text, &size_bytes)) {
        set_error(error_message, error_message_size, "Error: invalid size in manifest.");
        return false;
    }
    if (filename_exists(catalog, filename)) {
        set_error(error_message, error_message_size, "Error: duplicate image in manifest.");
        return false;
    }
    if (catalog->total_size > UINT64_MAX - size_bytes) {
        set_error(error_message, error_message_size, "Error: total catalog size is too large.");
        return false;
    }
    if (!validate_file(
            originals_path,
            filename,
            size_bytes,
            error_message,
            error_message_size
        )) {
        return false;
    }
    if (!append_entry(catalog, filename, source_path, size_bytes)) {
        set_error(error_message, error_message_size, "Error: insufficient memory for the catalog.");
        return false;
    }
    return true;
}

Lardon3DImageCatalog *
lardon3d_image_catalog_load(
    const Lardon3DAppState *state,
    char *error_message,
    size_t error_message_size
)
{
    set_error(error_message, error_message_size, "");
    if (!state || !state->project_loaded || !state->project_path[0]) {
        set_error(error_message, error_message_size, "No project loaded.");
        return NULL;
    }

    Lardon3DImageCatalog *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) {
        set_error(error_message, error_message_size, "Error: insufficient memory for the catalog.");
        return NULL;
    }
    char *images_path = join_path(state->project_path, "images");
    char *originals_path = images_path ? join_path(images_path, "originals") : NULL;
    char *manifest_path = images_path ? join_path(images_path, "manifest.tsv") : NULL;
    if (!images_path || !originals_path || !manifest_path) {
        set_error(error_message, error_message_size, "Error: catalog path is too long.");
        goto failure;
    }

    int descriptor = open(manifest_path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            set_error(error_message, error_message_size, "No imported image.");
            free(images_path);
            free(originals_path);
            free(manifest_path);
            return catalog;
        }
        set_error(error_message, error_message_size, "Error: unable to open manifest.tsv.");
        goto failure;
    }
    struct stat manifest_info;
    if (fstat(descriptor, &manifest_info) != 0 || !S_ISREG(manifest_info.st_mode)) {
        (void)close(descriptor);
        set_error(error_message, error_message_size, "Error: manifest.tsv is not a regular file.");
        goto failure;
    }
    FILE *file = fdopen(descriptor, "r");
    if (!file) {
        (void)close(descriptor);
        set_error(error_message, error_message_size, "Error: unable to read manifest.tsv.");
        goto failure;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    bool valid = length >= 0
        && strcmp(line, "filename\tsize_bytes\tsource_path\n") == 0;
    if (!valid) {
        set_error(error_message, error_message_size, "Error: invalid manifest.tsv header.");
    }
    while (valid && (length = getline(&line, &capacity, file)) >= 0) {
        if (length == 0 || line[(size_t)length - 1] != '\n') {
            set_error(error_message, error_message_size, "Error: truncated line in manifest.tsv.");
            valid = false;
            break;
        }
        line[(size_t)length - 1] = '\0';
        if (strchr(line, '\r') || strchr(line, '\n')
            || !parse_line(
                catalog,
                line,
                originals_path,
                error_message,
                error_message_size
            )) {
            if (!error_message || !error_message_size || !error_message[0]) {
                set_error(error_message, error_message_size, "Error: invalid manifest line.");
            }
            valid = false;
        }
    }
    if (ferror(file) || fclose(file) != 0) {
        set_error(error_message, error_message_size, "Error: unable to read manifest.tsv.");
        valid = false;
    }
    free(line);
    free(images_path);
    free(originals_path);
    free(manifest_path);
    if (!valid) {
        lardon3d_image_catalog_destroy(catalog);
        return NULL;
    }
    return catalog;

failure:
    free(images_path);
    free(originals_path);
    free(manifest_path);
    lardon3d_image_catalog_destroy(catalog);
    return NULL;
}

size_t
lardon3d_image_catalog_count(const Lardon3DImageCatalog *catalog)
{
    return catalog ? catalog->count : 0;
}

uint64_t
lardon3d_image_catalog_total_size(const Lardon3DImageCatalog *catalog)
{
    return catalog ? catalog->total_size : 0;
}

const Lardon3DImageEntry *
lardon3d_image_catalog_get(
    const Lardon3DImageCatalog *catalog,
    size_t index
)
{
    return catalog && index < catalog->count ? &catalog->entries[index] : NULL;
}

void
lardon3d_image_catalog_format_size(
    uint64_t size_bytes,
    char *text,
    size_t text_size
)
{
    if (!text || text_size == 0) {
        return;
    }
    static const char *units[] = {"octets", "Kio", "Mio", "Gio"};
    long double value = (long double)size_bytes;
    size_t unit = 0;
    while (value >= 1024.0L && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0L;
        ++unit;
    }
    if (unit == 0) {
        (void)snprintf(text, text_size, "%" PRIu64 " %s", size_bytes, units[unit]);
    } else {
        (void)snprintf(text, text_size, "%.1Lf %s", value, units[unit]);
        char *decimal = strchr(text, '.');
        if (decimal) {
            *decimal = ',';
        }
    }
}
