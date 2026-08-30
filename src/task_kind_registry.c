#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lardon3d/task_kind_registry.h>

#include "task_internal.h"

/* Production Matcher provides this private post-reconstruction hook. Minimal
 * Registry-only targets intentionally omit it; weak absence preserves the
 * universal fixed-envelope default without adding a public descriptor field. */
#if defined(__GNUC__) || defined(__clang__)
extern bool lardon3d_matcher_task_internal_configure_restored(
    Lardon3DTask *task,
    void *userdata
) __attribute__((weak));
extern bool lardon3d_acquisition_campaign_task_internal_configure_restored(
    Lardon3DTask *task,
    void *userdata
) __attribute__((weak));
#endif

enum {
    CANDIDATE_LEGACY_FIXED_BYTES = 128 * 1024,
    CANDIDATE_CURRENT_FIXED_BYTES = 256 * 1024,
    CANDIDATE_PER_ITEM_BYTES = 64 * 1024,
    MATCHER_LEGACY_FIXED_BYTES = 10 * 1024 * 1024,
    MATCHER_CURRENT_PER_ITEM_BYTES = 10 * 1024 * 1024,
    MATCHER_GPU_FIXED_BYTES = 640 * 1024,
    SIFT_FIXED_BYTES = 64 * 1024 * 1024,
};

static bool
estimate_equals(const Lardon3DResourceEstimate *left,
                const Lardon3DResourceEstimate *right)
{
    return left->memory_fixed_bytes == right->memory_fixed_bytes
        && left->gpu_memory_fixed_bytes == right->gpu_memory_fixed_bytes
        && left->memory_bytes_per_item == right->memory_bytes_per_item
        && left->gpu_memory_bytes_per_item == right->gpu_memory_bytes_per_item
        && left->minimum_batch_size == right->minimum_batch_size
        && left->maximum_batch_size == right->maximum_batch_size
        && left->desired_cpu_threads == right->desired_cpu_threads
        && left->desired_gpu_slots == right->desired_gpu_slots
        && left->desired_io_slots == right->desired_io_slots
        && left->task_class == right->task_class;
}

static bool
normalize_known_legacy_estimate(const char *kind,
                                const Lardon3DResourceEstimate *durable,
                                Lardon3DResourceEstimate *effective)
{
    Lardon3DResourceEstimate current = {0};
    Lardon3DResourceEstimate historical = {0};
    Lardon3DResourceEstimate oldest = {0};
    bool has_oldest = false;
    if (strcmp(kind, "candidate_pair.generate") == 0) {
        current = (Lardon3DResourceEstimate) {
            .memory_fixed_bytes = CANDIDATE_CURRENT_FIXED_BYTES,
            .memory_bytes_per_item = CANDIDATE_PER_ITEM_BYTES,
            .minimum_batch_size = 1,
            .maximum_batch_size = 64,
            .desired_cpu_threads = 12,
            .desired_io_slots = 1,
            .task_class = LARDON3D_RESOURCE_TASK_CPU,
        };
        historical = current;
        historical.memory_fixed_bytes = CANDIDATE_LEGACY_FIXED_BYTES;
        historical.desired_cpu_threads = 1;
    } else if (strcmp(kind, "matcher.run") == 0) {
        const bool gpu = durable->desired_gpu_slots == 1;
        current = (Lardon3DResourceEstimate) {
            .gpu_memory_fixed_bytes = gpu ? MATCHER_GPU_FIXED_BYTES : 0,
            .memory_bytes_per_item = MATCHER_CURRENT_PER_ITEM_BYTES,
            .minimum_batch_size = 1,
            .maximum_batch_size = 12,
            .desired_cpu_threads = gpu ? 1U : 12U,
            .desired_gpu_slots = gpu ? 1U : 0U,
            .desired_io_slots = 1,
            .task_class = LARDON3D_RESOURCE_TASK_CPU,
        };
        historical = current;
        /* CPU8/GPU0 and CPU1/GPU1 are the immediately preceding durable
         * operational forms. They are normalized only in memory: thread
         * count is not Matcher scientific identity and no estimate-only
         * checkpoint is published during recovery. */
        historical.maximum_batch_size = 8;
        historical.desired_cpu_threads = gpu ? 1U : 8U;
        oldest = current;
        oldest.memory_fixed_bytes = MATCHER_LEGACY_FIXED_BYTES;
        oldest.memory_bytes_per_item = 0;
        oldest.maximum_batch_size = 8;
        oldest.desired_cpu_threads = 12;
        has_oldest = true;
        Lardon3DResourceEstimate automatic = current;
        automatic.task_class = LARDON3D_RESOURCE_TASK_MIXED;
        if (!gpu && estimate_equals(durable, &automatic)) {
            /* MIXED is the truthful durable signature for new normal AUTO:
             * execution may consume either its CPU or Vulkan capability. */
            *effective = *durable;
            return true;
        }
    } else if (strcmp(kind, "features.extract.sift") == 0
               || strcmp(kind, "features.extract.rootsift") == 0) {
        current = (Lardon3DResourceEstimate) {
            .memory_fixed_bytes = SIFT_FIXED_BYTES,
            .memory_bytes_per_item = UINT64_C(1024) * 1024 * 1024,
            .minimum_batch_size = 1,
            .maximum_batch_size = 1,
            .desired_cpu_threads = 12,
            .desired_io_slots = 1,
            .task_class = LARDON3D_RESOURCE_TASK_CPU,
        };
        historical = current;
        historical.desired_cpu_threads = 1;
    } else {
        *effective = *durable;
        return true;
    }
    if (estimate_equals(durable, &current)) {
        *effective = *durable;
        return true;
    }
    if (!estimate_equals(durable, &historical)
        && (!has_oldest || !estimate_equals(durable, &oldest))) {
        return false;
    }
    /* These exact signatures are historical operational policy, not scientific
     * identity. Normalization is deliberately ephemeral: a pre-terminal crash
     * may repeat it, while persistence retains one unambiguous checkpoint
     * summary until ordinary Task progress advances it. */
    *effective = current;
    return true;
}

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
    Lardon3DTaskDurableSnapshot effective_snapshot = *snapshot;
    if (!normalize_known_legacy_estimate(kind, &snapshot->estimate,
                                         &effective_snapshot.estimate)) {
        if (binding.userdata_destroy) {
            binding.userdata_destroy(binding.userdata);
        }
        return LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED;
    }
    *task = lardon3d_task_restore_typed(
        &effective_snapshot,
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
    bool capabilities_configured =
        lardon3d_task_internal_enable_known_capabilities(*task);
#if defined(__GNUC__) || defined(__clang__)
    if (capabilities_configured && strcmp(kind, "matcher.run") == 0
        && lardon3d_matcher_task_internal_configure_restored) {
        capabilities_configured =
            lardon3d_matcher_task_internal_configure_restored(
                *task, binding.userdata);
    }
    if (capabilities_configured
        && strcmp(kind, "acquisition_campaign.run") == 0
        && lardon3d_acquisition_campaign_task_internal_configure_restored) {
        capabilities_configured =
            lardon3d_acquisition_campaign_task_internal_configure_restored(
                *task, binding.userdata);
    }
#endif
    if (!capabilities_configured) {
        lardon3d_task_destroy(*task);
        *task = NULL;
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
