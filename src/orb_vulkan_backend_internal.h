#ifndef LARDON3D_ORB_VULKAN_BACKEND_INTERNAL_H
#define LARDON3D_ORB_VULKAN_BACKEND_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/orb_vulkan_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LARDON3D_INTERNAL_VISIBILITY __attribute__((visibility("hidden")))
#else
#define LARDON3D_INTERNAL_VISIBILITY
#endif

typedef struct {
    uint64_t serial;
    uint64_t submits;
    uint64_t completions;
    uint64_t submit_cpu_ns;
    uint64_t fence_wait_ns;
    uint64_t readback_ns;
    bool gpu_timestamps_available;
    uint64_t gpu_execution_ns;
    uint64_t starvation_ns;
    uint64_t failures;
    uint64_t discards;
    bool slot_pending;
    uint32_t pending_slots;
    uint32_t retained_capacity;
    uint64_t retained_payload_bytes;
    bool sequence_capacity_active;
} Lardon3DOrbVulkanTelemetry;

enum {
    LARDON3D_ORB_VULKAN_MAX_INFLIGHT = 2,
    /* Explicit host-visible Vulkan buffer payload. Each slot owns two 8192x32
     * descriptor inputs and one 8192x4x32-bit readback buffer. Device,
     * pipeline, layouts and cache are immutable shared objects with opaque
     * driver allocations; no invented byte charge is assigned to them. */
    LARDON3D_ORB_VULKAN_FIXED_BYTES = 0,
    LARDON3D_ORB_VULKAN_PER_SLOT_BYTES =
        LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES
};

typedef struct {
    uint32_t slot;
    uint64_t generation;
} Lardon3DOrbVulkanRequest;

/* Matcher owns one capacity lease for the immutable executing sequence. The
 * backend may grow/shrink mapped request payload only while no request is
 * pending. End releases any depth-two payload and restores the public/default
 * depth-one capacity before the Task crosses its next admission boundary. */
LARDON3D_INTERNAL_VISIBILITY bool
lardon3d_orb_vulkan_internal_begin_sequence(
    Lardon3DOrbVulkanBackend *backend,
    uint32_t inflight_capacity
);
LARDON3D_INTERNAL_VISIBILITY bool
lardon3d_orb_vulkan_internal_end_sequence(
    Lardon3DOrbVulkanBackend *backend
);

/* Up to two private backend-owned requests may be in flight. The handle is the
 * exact slot identity plus a nonzero generation: finish/discard can consume
 * only that request and stale or mismatched handles never redirect evidence.
 * Begin stores the submitted feature counts in the slot; finish never trusts
 * fresh caller counts. Every finish result for a valid active handle consumes
 * or fails that slot, including invalid output capacity. */
LARDON3D_INTERNAL_VISIBILITY Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_begin(
    Lardon3DOrbVulkanBackend *backend,
    const unsigned char *descriptors_a,
    uint32_t feature_count_a,
    const unsigned char *descriptors_b,
    uint32_t feature_count_b,
    Lardon3DOrbVulkanRequest *request
);
LARDON3D_INTERNAL_VISIBILITY Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_finish(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request,
    Lardon3DOrbTop2 *output,
    size_t output_capacity
);
LARDON3D_INTERNAL_VISIBILITY Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_discard(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request
);
/* Cumulative counters saturate at UINT64_MAX. The snapshot is protected by
 * backend request-state ownership and therefore observes slot state and every
 * counter from one instant without widening the public backend-info ABI. */
LARDON3D_INTERNAL_VISIBILITY bool
lardon3d_orb_vulkan_internal_telemetry(
    Lardon3DOrbVulkanBackend *backend,
    Lardon3DOrbVulkanTelemetry *telemetry
);

#ifdef LARDON3D_ORB_VULKAN_TESTING
/* Inactive-slot saturation seam. It exercises production generation-retire
 * branching without billions of submissions and never exists in lardon3d. */
LARDON3D_INTERNAL_VISIBILITY bool
lardon3d_orb_vulkan_internal_test_set_slot_generation(
    Lardon3DOrbVulkanBackend *backend,
    uint32_t slot,
    uint64_t generation
);
#endif

#undef LARDON3D_INTERNAL_VISIBILITY

#ifdef __cplusplus
}
#endif

#endif
