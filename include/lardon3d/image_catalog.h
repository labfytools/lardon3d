#ifndef LARDON3D_IMAGE_CATALOG_H
#define LARDON3D_IMAGE_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/project_db.h>

typedef enum {
    LARDON3D_IMAGE_CATALOG_IMPORTED = 0,
    LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT,
    LARDON3D_IMAGE_CATALOG_INVALID_ARGUMENT,
    LARDON3D_IMAGE_CATALOG_SOURCE_ERROR,
    LARDON3D_IMAGE_CATALOG_PUBLICATION_ERROR,
    LARDON3D_IMAGE_CATALOG_DB_ERROR
} Lardon3DImageCatalogImportResult;

bool lardon3d_image_catalog_create_scanset(
    Lardon3DAppState *state, const char *name,
    Lardon3DProjectDbScanSet *scanset
);
Lardon3DImageCatalogImportResult lardon3d_image_catalog_import_file(
    Lardon3DAppState *state, uint64_t scanset_id, const char *source_path,
    uint64_t producer_task_id, Lardon3DProjectDbImage *image,
    Lardon3DProjectDbImageAsset *asset
);
Lardon3DProjectDbResult lardon3d_image_catalog_list(
    Lardon3DAppState *state, uint64_t scanset_id, uint64_t after_image_id,
    Lardon3DProjectDbImage *images, Lardon3DProjectDbImageAsset *assets,
    size_t capacity, size_t *count
);

typedef struct {
    char *filename;
    char *source_path;
    uint64_t size_bytes;
} Lardon3DImageEntry;

Lardon3DImageCatalog *lardon3d_image_catalog_load(
    const Lardon3DAppState *state,
    char *error_message,
    size_t error_message_size
);

void lardon3d_image_catalog_destroy(Lardon3DImageCatalog *catalog);
size_t lardon3d_image_catalog_count(const Lardon3DImageCatalog *catalog);
uint64_t lardon3d_image_catalog_total_size(
    const Lardon3DImageCatalog *catalog
);
const Lardon3DImageEntry *lardon3d_image_catalog_get(
    const Lardon3DImageCatalog *catalog,
    size_t index
);
void lardon3d_image_catalog_format_size(
    uint64_t size_bytes,
    char *text,
    size_t text_size
);

#endif
