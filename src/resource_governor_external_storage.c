#include <stdbool.h>

#include <lardon3d/resource_governor.h>

#include "resource_governor_internal.h"

static bool
scratch_operation(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease,
    bool acquire
)
{
    if (!lardon3d_resource_governor_internal_begin_scratch_operation(
            governor, controller, lease, acquire)) {
        return false;
    }

    /* LOCK ORDER: begin has released the Governor mutex before this physical
     * call takes the controller mutex. The controller has no Governor callback.
     * A concurrent drain/replacement therefore wins at the controller's exact
     * capability check instead of creating a nested-lock cycle. */
    bool physical_success = acquire
        ? lardon3d_ssd_controller_acquire_scratch(controller, lease)
        : lardon3d_ssd_controller_release_scratch(controller, lease);

    Lardon3DSsdSnapshot snapshot;
    Lardon3DResourceExternalStorage storage;
    bool storage_valid = lardon3d_ssd_controller_copy_snapshot(
            controller, &snapshot)
        && lardon3d_resource_external_storage_from_ssd_snapshot(
            &snapshot, &storage);

    return lardon3d_resource_governor_internal_finish_scratch_operation(
        governor, controller, lease, acquire, physical_success,
        storage_valid ? &storage : NULL);
}

bool
lardon3d_resource_governor_acquire_scratch(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
)
{
    return scratch_operation(governor, controller, lease, true);
}

bool
lardon3d_resource_governor_release_scratch(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease
)
{
    return scratch_operation(governor, controller, lease, false);
}
