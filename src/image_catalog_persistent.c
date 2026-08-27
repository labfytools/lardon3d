#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/image_catalog.h>

enum { COPY_BUFFER_SIZE = 64 * 1024 };

typedef enum {
    COPY_OK = 0,
    COPY_SOURCE_ERROR,
    COPY_DESTINATION_ERROR
} CopyResult;

static bool
join_path(char output[PATH_MAX], const char *parent, const char *child)
{
    int written = snprintf(output, PATH_MAX, "%s/%s", parent, child);
    return written > 0 && (size_t)written < PATH_MAX;
}

static bool
ensure_directory(const char *path)
{
    struct stat information;
    if (lstat(path, &information) == 0)
        return S_ISDIR(information.st_mode) && !S_ISLNK(information.st_mode);
    if (errno != ENOENT) return false;
    if (mkdir(path, 0755) == 0) return true;
    return errno == EEXIST && lstat(path, &information) == 0
        && S_ISDIR(information.st_mode) && !S_ISLNK(information.st_mode);
}

static bool
write_all(int descriptor, const unsigned char *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(descriptor, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

static CopyResult
hash_stream(int input, int output,
    unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE], uint64_t *size)
{
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) return COPY_SOURCE_ERROR;
    bool success = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    CopyResult result = COPY_OK;
    unsigned char buffer[COPY_BUFFER_SIZE];
    uint64_t total = 0;
    while (success) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { success = false; result = COPY_SOURCE_ERROR; break; }
        if (count == 0) break;
        size_t bytes = (size_t)count;
        if (total > (uint64_t)INT64_MAX - bytes
            || EVP_DigestUpdate(context, buffer, bytes) != 1) {
            success = false;
            result = COPY_SOURCE_ERROR;
            break;
        }
        if (output >= 0 && !write_all(output, buffer, bytes)) {
            success = false;
            result = COPY_DESTINATION_ERROR;
            break;
        }
        total += bytes;
    }
    unsigned int hash_size = 0;
    if (!success || EVP_DigestFinal_ex(context, hash, &hash_size) != 1
        || hash_size != LARDON3D_PROJECT_DB_SHA256_SIZE) {
        success = false;
        if (result == COPY_OK) result = COPY_SOURCE_ERROR;
    }
    EVP_MD_CTX_free(context);
    if (success) *size = total;
    return success ? COPY_OK : result;
}

static void
hash_hex(const unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE],
    char text[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < LARDON3D_PROJECT_DB_SHA256_SIZE; ++index) {
        text[index * 2] = digits[hash[index] >> 4];
        text[index * 2 + 1] = digits[hash[index] & 15U];
    }
    text[64] = '\0';
}

static bool
same_published_asset(const char *path,
    const unsigned char expected[LARDON3D_PROJECT_DB_SHA256_SIZE],
    uint64_t expected_size)
{
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat information;
    unsigned char actual[LARDON3D_PROJECT_DB_SHA256_SIZE];
    uint64_t size = 0;
    bool success = fstat(descriptor, &information) == 0
        && S_ISREG(information.st_mode)
        && hash_stream(descriptor, -1, actual, &size) == COPY_OK
        && size == expected_size
        && memcmp(actual, expected, sizeof(actual)) == 0;
    (void)close(descriptor);
    return success;
}

static bool
sync_directory(const char *path)
{
    int descriptor = open(path, O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return false;
    bool success = fsync(descriptor) == 0;
    if (close(descriptor) != 0) success = false;
    return success;
}

static bool
same_source_snapshot(const struct stat *before, const struct stat *after)
{
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino
        && before->st_size == after->st_size
        && before->st_mtim.tv_sec == after->st_mtim.tv_sec
        && before->st_mtim.tv_nsec == after->st_mtim.tv_nsec
        && before->st_ctim.tv_sec == after->st_ctim.tv_sec
        && before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

Lardon3DImageCatalogAssetPublishResult
lardon3d_image_catalog_publish_asset_file(Lardon3DAppState *state,
    const char *source_path, int64_t created_at, uint64_t max_source_bytes,
    Lardon3DProjectDbImageAsset *asset)
{
    if (!state || !state->project_loaded || !state->project_db || !source_path
        || !source_path[0] || created_at < 0 || max_source_bytes == 0 || !asset)
        return LARDON3D_IMAGE_CATALOG_ASSET_INVALID_ARGUMENT;
    memset(asset, 0, sizeof(*asset));
    int input = open(source_path, O_RDONLY | O_NOFOLLOW);
    if (input < 0) return LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_ERROR;
    struct stat source_before;
    if (fstat(input, &source_before) != 0 || !S_ISREG(source_before.st_mode)
        || source_before.st_size < 0) {
        (void)close(input);
        return LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_ERROR;
    }
    if ((uint64_t)source_before.st_size > max_source_bytes) {
        (void)close(input);
        return LARDON3D_IMAGE_CATALOG_ASSET_TOO_LARGE;
    }
    char assets[PATH_MAX], images[PATH_MAX], temporary[PATH_MAX];
    if (!join_path(assets, state->project_path, "assets")
        || !ensure_directory(assets) || !sync_directory(state->project_path)
        || !join_path(images, assets, "images") || !ensure_directory(images)
        || !sync_directory(assets)
        || snprintf(temporary, sizeof(temporary), "%s/.asset.tmp.XXXXXX", images) <= 0) {
        (void)close(input);
        return LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR;
    }
    int output = mkstemp(temporary);
    if (output < 0) {
        (void)close(input);
        return LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR;
    }
    unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE];
    uint64_t size = 0;
    struct stat source_after;
    CopyResult copy = hash_stream(input, output, hash, &size);
    bool copied = copy == COPY_OK;
    bool source_changed = copied && (fstat(input, &source_after) != 0
        || !same_source_snapshot(&source_before, &source_after));
    bool source_close_ok = close(input) == 0;
    bool destination_sync_ok = copied && !source_changed && source_close_ok
        && fsync(output) == 0;
    int destination_close_result = close(output);
    if (copy == COPY_DESTINATION_ERROR || (copied && !source_changed && source_close_ok
        && (!destination_sync_ok || destination_close_result != 0))) {
        (void)unlink(temporary);
        return LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR;
    }
    if (source_changed) {
        (void)unlink(temporary);
        return LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_CHANGED;
    }
    if (copy != COPY_OK || !source_close_ok) {
        (void)unlink(temporary);
        return LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_ERROR;
    }
    char hex[65], prefix[3], prefix_path[PATH_MAX], final[PATH_MAX];
    hash_hex(hash, hex);
    prefix[0] = hex[0]; prefix[1] = hex[1]; prefix[2] = '\0';
    if (!join_path(prefix_path, images, prefix) || !ensure_directory(prefix_path)
        || !sync_directory(images) || !join_path(final, prefix_path, hex)) {
        (void)unlink(temporary);
        return LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR;
    }
    bool published = false;
    if (link(temporary, final) == 0) {
        published = unlink(temporary) == 0 && sync_directory(prefix_path);
    } else if (errno == EEXIST && same_published_asset(final, hash, size)) {
        published = unlink(temporary) == 0;
    }
    if (!published) {
        (void)unlink(temporary);
        return LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR;
    }
    char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    int relative_size = snprintf(relative, sizeof(relative), "assets/images/%s/%s", prefix, hex);
    if (relative_size <= 0 || (size_t)relative_size >= sizeof(relative)
        || lardon3d_project_db_register_image_asset(state->project_db, hash, relative,
            size, created_at, asset) != LARDON3D_PROJECT_DB_OK)
        return LARDON3D_IMAGE_CATALOG_ASSET_DB_ERROR;
    return LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED;
}

Lardon3DImageCatalogAssetPublishResult
lardon3d_image_catalog_test_copy_hash(int input, int output)
{
    unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE];
    uint64_t size = 0;
    CopyResult result = hash_stream(input, output, hash, &size);
    return result == COPY_OK ? LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED
        : result == COPY_DESTINATION_ERROR ? LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR
                                           : LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_ERROR;
}

bool
lardon3d_image_catalog_create_scanset(Lardon3DAppState *state,
    const char *name, Lardon3DProjectDbScanSet *scanset)
{
    return state && state->project_loaded && state->project_db
        && lardon3d_project_db_create_scanset(state->project_db, name, scanset)
            == LARDON3D_PROJECT_DB_OK;
}

Lardon3DImageCatalogImportResult
lardon3d_image_catalog_import_file(Lardon3DAppState *state,
    uint64_t scanset_id, const char *source_path, uint64_t producer_task_id,
    Lardon3DProjectDbImage *image, Lardon3DProjectDbImageAsset *asset)
{
    if (!state || !state->project_loaded || !state->project_db
        || scanset_id == 0 || !source_path || !source_path[0] || !image || !asset)
        return LARDON3D_IMAGE_CATALOG_INVALID_ARGUMENT;
    const char *basename = strrchr(source_path, '/');
    basename = basename ? basename + 1 : source_path;
    if (!basename[0] || strchr(basename, '\\') || strchr(basename, '\t')
        || strchr(basename, '\r') || strchr(basename, '\n')
        || strnlen(basename, LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY)
            >= LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY)
        return LARDON3D_IMAGE_CATALOG_INVALID_ARGUMENT;
    Lardon3DProjectDbScanSet scanset;
    if (lardon3d_project_db_load_scanset(state->project_db, scanset_id, &scanset)
        != LARDON3D_PROJECT_DB_OK) return LARDON3D_IMAGE_CATALOG_INVALID_ARGUMENT;
    int64_t imported_at = (int64_t)time(NULL);
    if (imported_at < 0) return LARDON3D_IMAGE_CATALOG_PUBLICATION_ERROR;
    Lardon3DImageCatalogAssetPublishResult published =
        lardon3d_image_catalog_publish_asset_file(state, source_path, imported_at,
            UINT64_MAX, asset);
    if (published == LARDON3D_IMAGE_CATALOG_ASSET_INVALID_ARGUMENT)
        return LARDON3D_IMAGE_CATALOG_INVALID_ARGUMENT;
    if (published == LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_ERROR)
        return LARDON3D_IMAGE_CATALOG_SOURCE_ERROR;
    if (published == LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR)
        return LARDON3D_IMAGE_CATALOG_PUBLICATION_ERROR;
    if (published != LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED)
        return LARDON3D_IMAGE_CATALOG_DB_ERROR;
    Lardon3DProjectDbImageRegisterStatus status;
    if (imported_at < 0 || lardon3d_project_db_register_image(state->project_db,
            scanset_id, asset->sha256, asset->path, asset->size_bytes, basename, source_path,
            producer_task_id, imported_at, &status, image)
        != LARDON3D_PROJECT_DB_OK) return LARDON3D_IMAGE_CATALOG_DB_ERROR;
    if (lardon3d_project_db_load_image(state->project_db, image->image_id,
            image, asset) != LARDON3D_PROJECT_DB_OK)
        return LARDON3D_IMAGE_CATALOG_DB_ERROR;
    return status == LARDON3D_PROJECT_DB_IMAGE_ALREADY_PRESENT
        ? LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT
        : LARDON3D_IMAGE_CATALOG_IMPORTED;
}

Lardon3DProjectDbResult
lardon3d_image_catalog_list(Lardon3DAppState *state, uint64_t scanset_id,
    uint64_t after_image_id, Lardon3DProjectDbImage *images,
    Lardon3DProjectDbImageAsset *assets, size_t capacity, size_t *count)
{
    if (!state || !state->project_loaded || !state->project_db)
        return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
    return lardon3d_project_db_list_images(state->project_db, scanset_id,
        after_image_id, images, assets, capacity, count);
}
