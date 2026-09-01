#ifndef LARDON3D_TUI_SSD_ASYNC_INTERNAL_H
#define LARDON3D_TUI_SSD_ASYNC_INTERNAL_H

#include <lardon3d/tui_ssd_async.h>

/* Deterministic provider seam for testing Governor publication without a real
 * D-Bus controller. `controller_identity` is an opaque borrowed registry token
 * and is never dereferenced by this object. Production uses the public bound
 * constructor with the real controller object. */
Lardon3DTuiSsdAsync *
lardon3d_tui_ssd_async_internal_create_with_provider_and_governor(
    Lardon3DTuiSsdAsyncProvider provider,
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller_identity
);

#endif
