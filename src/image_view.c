#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/image_view.h>

struct Lardon3DImageView {
    const Lardon3DImageCatalog *catalog;
    size_t *indices;
    size_t count;
    uint64_t total_size;
    Lardon3DImageSort sort;
    char filter[LARDON3D_IMAGE_FILTER_CAPACITY];
    size_t selection;
    size_t offset;
};

static unsigned char
ascii_lower(unsigned char character)
{
    return character >= 'A' && character <= 'Z'
        ? (unsigned char)(character + ('a' - 'A'))
        : character;
}

bool
lardon3d_image_view_ascii_contains(
    const char *text,
    const char *filter
)
{
    if (!text || !filter) {
        return false;
    }
    if (!filter[0]) {
        return true;
    }
    for (const unsigned char *start = (const unsigned char *)text;
         *start;
         ++start) {
        const unsigned char *left = start;
        const unsigned char *right = (const unsigned char *)filter;
        while (*left && *right && ascii_lower(*left) == ascii_lower(*right)) {
            ++left;
            ++right;
        }
        if (!*right) {
            return true;
        }
    }
    return false;
}

static int
compare_indices(
    const Lardon3DImageView *view,
    size_t left_index,
    size_t right_index
)
{
    const Lardon3DImageEntry *left = lardon3d_image_catalog_get(
        view->catalog,
        left_index
    );
    const Lardon3DImageEntry *right = lardon3d_image_catalog_get(
        view->catalog,
        right_index
    );
    if (!left || !right) {
        return left ? -1 : right ? 1 : 0;
    }

    int name_comparison = strcmp(left->filename, right->filename);
    switch (view->sort) {
    case LARDON3D_IMAGE_SORT_NAME_ASC:
        return name_comparison;
    case LARDON3D_IMAGE_SORT_NAME_DESC:
        return name_comparison > 0 ? -1 : name_comparison < 0 ? 1 : 0;
    case LARDON3D_IMAGE_SORT_SIZE_ASC:
    case LARDON3D_IMAGE_SORT_SIZE_DESC:
        if (left->size_bytes != right->size_bytes) {
            bool left_first = view->sort == LARDON3D_IMAGE_SORT_SIZE_ASC
                ? left->size_bytes < right->size_bytes
                : left->size_bytes > right->size_bytes;
            return left_first ? -1 : 1;
        }
        return name_comparison;
    case LARDON3D_IMAGE_SORT_IMPORT_ORDER:
    default:
        return left_index < right_index ? -1 : left_index > right_index ? 1 : 0;
    }
}

static void
merge_sort_range(
    Lardon3DImageView *view,
    size_t *indices,
    size_t *temporary,
    size_t begin,
    size_t end
)
{
    if (end - begin < 2) {
        return;
    }
    size_t middle = begin + (end - begin) / 2;
    merge_sort_range(view, indices, temporary, begin, middle);
    merge_sort_range(view, indices, temporary, middle, end);

    size_t left = begin;
    size_t right = middle;
    size_t output = begin;
    while (left < middle || right < end) {
        if (right >= end
            || (left < middle
                && compare_indices(view, indices[left], indices[right]) <= 0)) {
            temporary[output++] = indices[left++];
        } else {
            temporary[output++] = indices[right++];
        }
    }
    (void)memcpy(
        indices + begin,
        temporary + begin,
        (end - begin) * sizeof(*indices)
    );
}

static bool
selected_catalog_index(
    const Lardon3DImageView *view,
    size_t *catalog_index
)
{
    if (!view || view->selection >= view->count) {
        return false;
    }
    *catalog_index = view->indices[view->selection];
    return true;
}

static bool
build_view(
    Lardon3DImageView *view,
    const Lardon3DImageCatalog *catalog,
    Lardon3DImageSort sort,
    const char *filter
)
{
    char filter_copy[LARDON3D_IMAGE_FILTER_CAPACITY];
    (void)snprintf(filter_copy, sizeof(filter_copy), "%s", filter);
    size_t preserved_index = 0;
    bool preserve = selected_catalog_index(view, &preserved_index);
    size_t catalog_count = lardon3d_image_catalog_count(catalog);
    if (catalog_count > SIZE_MAX / sizeof(*view->indices)) {
        return false;
    }
    size_t *indices = catalog_count > 0
        ? malloc(catalog_count * sizeof(*indices))
        : NULL;
    if (catalog_count > 0 && !indices) {
        return false;
    }

    size_t count = 0;
    uint64_t total_size = 0;
    for (size_t index = 0; index < catalog_count; ++index) {
        const Lardon3DImageEntry *entry = lardon3d_image_catalog_get(
            catalog,
            index
        );
        if (entry
            && lardon3d_image_view_ascii_contains(
                entry->filename,
                filter_copy
            )) {
            indices[count++] = index;
            total_size += entry->size_bytes;
        }
    }

    const Lardon3DImageCatalog *old_catalog = view->catalog;
    Lardon3DImageSort old_sort = view->sort;
    view->catalog = catalog;
    view->sort = sort;
    if (sort != LARDON3D_IMAGE_SORT_IMPORT_ORDER && count > 1) {
        size_t *temporary = malloc(count * sizeof(*temporary));
        if (!temporary) {
            view->catalog = old_catalog;
            view->sort = old_sort;
            free(indices);
            return false;
        }
        merge_sort_range(view, indices, temporary, 0, count);
        free(temporary);
    }

    free(view->indices);
    view->indices = indices;
    view->count = count;
    view->total_size = total_size;
    view->selection = 0;
    view->offset = 0;
    (void)snprintf(view->filter, sizeof(view->filter), "%s", filter_copy);
    if (preserve) {
        for (size_t index = 0; index < count; ++index) {
            if (indices[index] == preserved_index) {
                view->selection = index;
                break;
            }
        }
    }
    return true;
}

Lardon3DImageView *
lardon3d_image_view_create(const Lardon3DImageCatalog *catalog)
{
    Lardon3DImageView *view = calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }
    view->catalog = catalog;
    view->sort = LARDON3D_IMAGE_SORT_IMPORT_ORDER;
    if (!build_view(view, catalog, view->sort, "")) {
        free(view);
        return NULL;
    }
    return view;
}

void
lardon3d_image_view_destroy(Lardon3DImageView *view)
{
    if (!view) {
        return;
    }
    free(view->indices);
    free(view);
}

bool
lardon3d_image_view_set_catalog(
    Lardon3DImageView *view,
    const Lardon3DImageCatalog *catalog
)
{
    return view && build_view(view, catalog, view->sort, view->filter);
}

bool
lardon3d_image_view_set_sort(
    Lardon3DImageView *view,
    Lardon3DImageSort sort
)
{
    if (!view || sort < LARDON3D_IMAGE_SORT_IMPORT_ORDER
        || sort > LARDON3D_IMAGE_SORT_SIZE_DESC) {
        return false;
    }
    return build_view(view, view->catalog, sort, view->filter);
}

bool
lardon3d_image_view_set_filter(
    Lardon3DImageView *view,
    const char *filter,
    char *error_message,
    size_t error_message_size
)
{
    if (error_message && error_message_size > 0) {
        error_message[0] = '\0';
    }
    if (!view || !filter) {
        if (error_message && error_message_size > 0) {
            (void)snprintf(
                error_message,
                error_message_size,
                "Error: invalid filter."
            );
        }
        return false;
    }
    if (strnlen(filter, LARDON3D_IMAGE_FILTER_CAPACITY)
        >= LARDON3D_IMAGE_FILTER_CAPACITY) {
        if (error_message && error_message_size > 0) {
            (void)snprintf(
                error_message,
                error_message_size,
                "Error: filter is too long."
            );
        }
        return false;
    }
    if (!build_view(view, view->catalog, view->sort, filter)) {
        if (error_message && error_message_size > 0) {
            (void)snprintf(
                error_message,
                error_message_size,
                "Error: insufficient memory for the view."
            );
        }
        return false;
    }
    return true;
}

bool
lardon3d_image_view_rebuild(Lardon3DImageView *view)
{
    return view && build_view(view, view->catalog, view->sort, view->filter);
}

size_t
lardon3d_image_view_count(const Lardon3DImageView *view)
{
    return view ? view->count : 0;
}

uint64_t
lardon3d_image_view_total_size(const Lardon3DImageView *view)
{
    return view ? view->total_size : 0;
}

const Lardon3DImageEntry *
lardon3d_image_view_get(
    const Lardon3DImageView *view,
    size_t visible_index
)
{
    return view && visible_index < view->count
        ? lardon3d_image_catalog_get(view->catalog, view->indices[visible_index])
        : NULL;
}

bool
lardon3d_image_view_catalog_index(
    const Lardon3DImageView *view,
    size_t visible_index,
    size_t *catalog_index
)
{
    if (!view || !catalog_index || visible_index >= view->count) {
        return false;
    }
    *catalog_index = view->indices[visible_index];
    return true;
}

Lardon3DImageSort
lardon3d_image_view_sort(const Lardon3DImageView *view)
{
    return view ? view->sort : LARDON3D_IMAGE_SORT_IMPORT_ORDER;
}

const char *
lardon3d_image_view_filter(const Lardon3DImageView *view)
{
    return view ? view->filter : "";
}

const char *
lardon3d_image_view_sort_name(Lardon3DImageSort sort)
{
    switch (sort) {
    case LARDON3D_IMAGE_SORT_NAME_ASC:
        return "Name ascending";
    case LARDON3D_IMAGE_SORT_NAME_DESC:
        return "Name descending";
    case LARDON3D_IMAGE_SORT_SIZE_ASC:
        return "Size ascending";
    case LARDON3D_IMAGE_SORT_SIZE_DESC:
        return "Size descending";
    case LARDON3D_IMAGE_SORT_IMPORT_ORDER:
    default:
        return "Import order";
    }
}

size_t
lardon3d_image_view_selection(const Lardon3DImageView *view)
{
    return view ? view->selection : 0;
}

size_t
lardon3d_image_view_offset(const Lardon3DImageView *view)
{
    return view ? view->offset : 0;
}

void
lardon3d_image_view_normalize(
    Lardon3DImageView *view,
    size_t page_size
)
{
    if (!view) {
        return;
    }
    if (view->count == 0) {
        view->selection = 0;
        view->offset = 0;
        return;
    }
    if (view->selection >= view->count) {
        view->selection = view->count - 1;
    }
    if (view->offset > view->selection) {
        view->offset = view->selection;
    }
    if (page_size == 0) {
        page_size = 1;
    }
    if (view->selection - view->offset >= page_size) {
        view->offset = view->selection - page_size + 1;
    }
}

void
lardon3d_image_view_select(
    Lardon3DImageView *view,
    size_t visible_index,
    size_t page_size
)
{
    if (!view) {
        return;
    }
    view->selection = visible_index;
    lardon3d_image_view_normalize(view, page_size);
}
