#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lardon3d/task_kind_registry.h>

bool
lardon3d_task_kind_registry_init(
    Lardon3DTaskKindRegistry *registry,
    const Lardon3DTaskKindDescriptor *descriptors,
    size_t count
)
{
    if (!registry || count > LARDON3D_TASK_KIND_REGISTRY_MAX
        || (count > 0 && !descriptors)) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!lardon3d_task_kind_is_valid(descriptors[index].kind)
            || descriptors[index].kind_version == 0
            || !descriptors[index].reconstruct) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(descriptors[index].kind,
                    descriptors[previous].kind) == 0) {
                return false;
            }
        }
    }
    *registry = (Lardon3DTaskKindRegistry) {
        .descriptors = descriptors,
        .count = count,
    };
    return true;
}

Lardon3DTaskKindResult
lardon3d_task_kind_registry_lookup(
    const Lardon3DTaskKindRegistry *registry,
    const char *kind,
    uint32_t kind_version,
    const Lardon3DTaskKindDescriptor **descriptor
)
{
    if (descriptor) {
        *descriptor = NULL;
    }
    if (!registry || !descriptor || !lardon3d_task_kind_is_valid(kind)
        || kind_version == 0 || registry->count > LARDON3D_TASK_KIND_REGISTRY_MAX
        || (registry->count > 0 && !registry->descriptors)) {
        return LARDON3D_TASK_KIND_INVALID_ARGUMENT;
    }
    bool kind_known = false;
    for (size_t index = 0; index < registry->count; ++index) {
        const Lardon3DTaskKindDescriptor *candidate =
            &registry->descriptors[index];
        if (strcmp(candidate->kind, kind) == 0) {
            kind_known = true;
            if (candidate->kind_version == kind_version) {
                *descriptor = candidate;
                return LARDON3D_TASK_KIND_OK;
            }
        }
    }
    return kind_known ? LARDON3D_TASK_KIND_UNSUPPORTED_VERSION
                      : LARDON3D_TASK_KIND_UNKNOWN;
}

Lardon3DTaskKindResult
lardon3d_task_kind_registry_restore(
    const Lardon3DTaskKindRegistry *registry,
    const char *kind,
    uint32_t kind_version,
    const Lardon3DTaskDurableSnapshot *snapshot,
    void *context,
    Lardon3DTask **task
)
{
    if (task) {
        *task = NULL;
    }
    if (!snapshot || !task) {
        return LARDON3D_TASK_KIND_INVALID_ARGUMENT;
    }
    const Lardon3DTaskKindDescriptor *descriptor = NULL;
    Lardon3DTaskKindResult result = lardon3d_task_kind_registry_lookup(
        registry, kind, kind_version, &descriptor
    );
    if (result != LARDON3D_TASK_KIND_OK) {
        return result;
    }
    Lardon3DTaskKindBinding binding = {0};
    if (!descriptor->reconstruct(snapshot, context, &binding)
        || !binding.callback) {
        if (binding.userdata_destroy) {
            binding.userdata_destroy(binding.userdata);
        }
        return LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED;
    }
    *task = lardon3d_task_restore_typed(
        snapshot,
        kind,
        kind_version,
        binding.callback,
        binding.userdata,
        binding.userdata_destroy
    );
    if (!*task) {
        if (binding.userdata_destroy) {
            binding.userdata_destroy(binding.userdata);
        }
        return LARDON3D_TASK_KIND_RESTORE_FAILED;
    }
    if (binding.finished_callback
        && !lardon3d_task_set_finished_callback(*task,
            binding.finished_callback, binding.finished_userdata)) {
        lardon3d_task_destroy(*task);
        *task = NULL;
        return LARDON3D_TASK_KIND_RESTORE_FAILED;
    }
    return LARDON3D_TASK_KIND_OK;
}
