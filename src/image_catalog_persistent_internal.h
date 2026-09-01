#ifndef LARDON3D_IMAGE_CATALOG_PERSISTENT_INTERNAL_H
#define LARDON3D_IMAGE_CATALOG_PERSISTENT_INTERNAL_H

#include <lardon3d/image_catalog.h>

#if defined(LARDON3D_IMAGE_CATALOG_PERSISTENT_TESTING)

/* TEST CONTRACT: this direct descriptor seam exists only in
 * test-persistent-image-catalog so its /dev/full fixture can distinguish a
 * destination write failure. It owns neither descriptor and is absent from
 * every production object and public ABI. */
Lardon3DImageCatalogAssetPublishResult lardon3d_image_catalog_test_copy_hash(
    int input,
    int output
);

#endif

#endif
