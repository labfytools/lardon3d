#ifndef LARDON3D_IMAGE_VIEW_H
#define LARDON3D_IMAGE_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/image_catalog.h>

enum {
    LARDON3D_IMAGE_FILTER_CAPACITY = 128,
};

typedef enum {
    LARDON3D_IMAGE_SORT_IMPORT_ORDER = 0,
    LARDON3D_IMAGE_SORT_NAME_ASC,
    LARDON3D_IMAGE_SORT_NAME_DESC,
    LARDON3D_IMAGE_SORT_SIZE_ASC,
    LARDON3D_IMAGE_SORT_SIZE_DESC
} Lardon3DImageSort;

Lardon3DImageView *lardon3d_image_view_create(
    const Lardon3DImageCatalog *catalog
);
void lardon3d_image_view_destroy(Lardon3DImageView *view);
bool lardon3d_image_view_set_catalog(
    Lardon3DImageView *view,
    const Lardon3DImageCatalog *catalog
);
bool lardon3d_image_view_set_sort(
    Lardon3DImageView *view,
    Lardon3DImageSort sort
);
bool lardon3d_image_view_set_filter(
    Lardon3DImageView *view,
    const char *filter,
    char *error_message,
    size_t error_message_size
);
bool lardon3d_image_view_rebuild(Lardon3DImageView *view);
size_t lardon3d_image_view_count(const Lardon3DImageView *view);
uint64_t lardon3d_image_view_total_size(const Lardon3DImageView *view);
const Lardon3DImageEntry *lardon3d_image_view_get(
    const Lardon3DImageView *view,
    size_t visible_index
);
bool lardon3d_image_view_catalog_index(
    const Lardon3DImageView *view,
    size_t visible_index,
    size_t *catalog_index
);
Lardon3DImageSort lardon3d_image_view_sort(const Lardon3DImageView *view);
const char *lardon3d_image_view_filter(const Lardon3DImageView *view);
const char *lardon3d_image_view_sort_name(Lardon3DImageSort sort);
size_t lardon3d_image_view_selection(const Lardon3DImageView *view);
size_t lardon3d_image_view_offset(const Lardon3DImageView *view);
void lardon3d_image_view_select(
    Lardon3DImageView *view,
    size_t visible_index,
    size_t page_size
);
void lardon3d_image_view_normalize(
    Lardon3DImageView *view,
    size_t page_size
);
bool lardon3d_image_view_ascii_contains(
    const char *text,
    const char *filter
);

#endif
