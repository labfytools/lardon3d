#ifndef LARDON3D_IMAGE_CATALOG_H
#define LARDON3D_IMAGE_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>

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
