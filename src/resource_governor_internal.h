#ifndef LARDON3D_RESOURCE_GOVERNOR_INTERNAL_H
#define LARDON3D_RESOURCE_GOVERNOR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/resource_governor.h>
#include <lardon3d/task.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LARDON3D_RESOURCE_CAPABILITY_MAX = 4,
    LARDON3D_RESOURCE_DIAGNOSTIC_REASON_CAPACITY = 96,
    LARDON3D_RESOURCE_CPU_MAX = 1024,
    LARDON3D_RESOURCE_CPU_MASK_WORDS = LARDON3D_RESOURCE_CPU_MAX / 64,
};

typedef enum {
    LARDON3D_RESOURCE_BACKEND_FIXED = 0,
    LARDON3D_RESOURCE_BACKEND_CPU = 1,
    LARDON3D_RESOURCE_BACKEND_ORB_VULKAN = 2,
    /* Diagnostics only: one admitted GPU sequence completed whole pairs on
     * both backends after an operational Vulkan failure. Never a capability. */
    LARDON3D_RESOURCE_BACKEND_MIXED = 3,
} Lardon3DResourceBackend;

typedef struct {
    Lardon3DResourceEstimate estimate;
    Lardon3DResourceBackend backend;
    /* Maximum validated simultaneous requests. A zero minimum means the
     * established fixed-capability shorthand minimum==maximum; adaptive
     * capabilities state the lower bound explicitly. The canonical durable
     * estimate remains untouched while per-inflight operational GPU bytes are
     * reconstructed privately for each sequence. */
    size_t inflight_limit;
    size_t minimum_inflight_limit;
    uint64_t gpu_memory_bytes_per_inflight;
    unsigned int helper_limit;
    bool preferred;
    bool cpu_reducible;
    bool batch_adaptive;
    /* Current ORB Vulkan batch trials use a longer observation window than
     * generic CPU adaptation. This private operational property is
     * reconstructed with the capability and is never durable/scientific. */
    bool sustained_gpu_batch_feedback;
    bool inflight_adaptive;
    bool requires_runtime_backend;
} Lardon3DTaskCapability;

typedef struct {
    size_t count;
    Lardon3DTaskCapability capabilities[LARDON3D_RESOURCE_CAPABILITY_MAX];
} Lardon3DTaskCapabilityEnvelope;

typedef struct {
    size_t capability_index;
    Lardon3DTaskCapability capability;
    /* Exact per-sequence estimate used only for reservation. The envelope's
     * canonical maximums stay intact for next-sequence feedback. */
    Lardon3DResourceEstimate reservation_estimate;
    Lardon3DResourceDecision decision;
    /* Frozen operational value selected for this sequence. */
    size_t inflight_limit;
    Lardon3DResourcePressure pressure;
    char reason[LARDON3D_RESOURCE_DIAGNOSTIC_REASON_CAPACITY];
} Lardon3DResourceCapabilitySelection;

typedef struct {
    bool memory_available_known;
    uint64_t memory_available_bytes;
    bool memory_psi_some_known;
    uint32_t memory_psi_some_basis_points;
    bool memory_psi_full_known;
    uint32_t memory_psi_full_basis_points;
    bool io_psi_some_known;
    uint32_t io_psi_some_basis_points;
    bool io_psi_full_known;
    uint32_t io_psi_full_basis_points;
    bool swap_delta_known;
    uint64_t swap_pages_in_delta;
    uint64_t swap_pages_out_delta;
    bool compute_pool_utilization_known;
    uint32_t compute_pool_utilization_basis_points;
    bool gpu_busy_known;
    uint32_t gpu_busy_basis_points;
    bool process_rss_known;
    uint64_t process_rss_bytes;
    bool process_peak_rss_known;
    uint64_t process_peak_rss_bytes;
} Lardon3DResourceHostTelemetry;

/* Private raw-input seam used by production's bounded proc/sysfs readers and
 * deterministic tests. Pointers are borrowed only for the call. A missing
 * optional input produces an unknown metric, never a Task failure. */
typedef struct {
    const char *proc_stat;
    const char *meminfo;
    const char *memory_psi;
    const char *io_psi;
    const char *vmstat;
    const char *process_status;
    const char *gpu_busy_percent;
} Lardon3DResourceTelemetryRaw;

typedef struct {
    uint64_t vulkan_submits;
    uint64_t vulkan_completions;
    uint64_t vulkan_submit_cpu_ns;
    uint64_t vulkan_fence_wait_ns;
    uint64_t vulkan_readback_ns;
    bool vulkan_gpu_time_known;
    uint64_t vulkan_gpu_ns;
    uint64_t vulkan_starvation_ns;
    uint64_t matcher_cpu_ns;
    uint64_t publication_ns;
    /* A Vulkan-selected Matcher pair is classified only after its complete
     * CPU fallback is durably published. Normal CPU-selected work is not a
     * fallback. The sequence bound keeps these counters small; the explicit
     * flag still makes injected/overflowed private telemetry fail closed. */
    bool fallback_items_saturated;
    uint64_t local_ineligible_fallback_items;
    uint64_t backend_failure_fallback_items;
    uint64_t backend_other_fallback_items;
} Lardon3DResourceExecutionMetrics;

/* Item-level fallback evidence is committed at the exact durable publication
 * boundary, independently of end-of-sequence throughput feedback. The enum is
 * private because these are operational benchmark classes, not scientific or
 * persisted Matcher identities. */
typedef enum {
    LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE = 1,
    LARDON3D_RESOURCE_FALLBACK_ITEM_BACKEND_FAILURE,
    LARDON3D_RESOURCE_FALLBACK_ITEM_OTHER,
} Lardon3DResourceFallbackItemCause;

typedef struct {
    uint64_t serial;
    char task_kind[LARDON3D_TASK_KIND_CAPACITY];
    uint32_t task_kind_version;
    /* `backend` is the immutable selected capability. `actual_backend` is
     * execution feedback and may differ only through complete CPU fallback. */
    Lardon3DResourceBackend backend;
    Lardon3DResourceBackend actual_backend;
    bool backend_fallback;
    uint64_t previous_wall_time_ns;
    size_t items_completed;
    uint64_t durable_items_per_second_milli;
    size_t batch_size;
    size_t inflight_limit;
    unsigned int helper_limit;
    uint64_t memory_bytes;
    uint64_t gpu_memory_bytes;
    unsigned int cpu_threads;
    unsigned int gpu_slots;
    unsigned int io_slots;
    Lardon3DResourcePressure pressure;
    Lardon3DResourceHostTelemetry host;
    Lardon3DResourceExecutionMetrics execution;
    char reason[LARDON3D_RESOURCE_DIAGNOSTIC_REASON_CAPACITY];
    char backend_reason[LARDON3D_RESOURCE_DIAGNOSTIC_REASON_CAPACITY];
} Lardon3DResourceSequenceDiagnostic;

/* Fixed-size Governor-lifetime evidence. This aggregate makes fast sequences
 * countable even when a polling runner observes only the latest diagnostic
 * change. Counters saturate, gauges retain extrema, and no per-sequence history
 * or scientific state is persisted. */
typedef struct {
    bool saturated;
    uint64_t admission_count;
    uint64_t sequence_count;
    uint64_t durable_items;
    uint64_t total_wall_time_ns;
    uint64_t backend_fallback_sequences;
    /* Benchmark evidence must distinguish the scientifically valid complete
     * CPU handling of a locally Vulkan-ineligible pair from an unhealthy
     * backend. These bounded counters classify exact execution reasons; they
     * remain operational Governor telemetry and are never persisted. */
    uint64_t backend_ineligible_fallback_sequences;
    uint64_t backend_failure_fallback_sequences;
    uint64_t backend_other_fallback_sequences;
    /* Item counters, unlike the sequence classifiers above, are invariant to
     * benchmark batch regrouping. They are summed with the aggregate's shared
     * saturation flag and remain fixed-size, operational, and non-persistent. */
    uint64_t local_ineligible_fallback_items;
    uint64_t backend_failure_fallback_items;
    uint64_t backend_other_fallback_items;
    uint64_t selected_backend_admissions[LARDON3D_RESOURCE_BACKEND_MIXED + 1];
    uint64_t actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_MIXED + 1];
    uint64_t contract_change_count;
    bool memory_available_known;
    uint64_t minimum_memory_available_bytes;
    bool gpu_busy_known;
    uint32_t maximum_gpu_busy_basis_points;
    bool process_rss_known;
    uint64_t maximum_process_rss_bytes;
    bool process_peak_rss_known;
    uint64_t maximum_process_peak_rss_bytes;
    uint64_t vulkan_submits;
    uint64_t vulkan_completions;
    uint64_t vulkan_submit_cpu_ns;
    uint64_t vulkan_fence_wait_ns;
    uint64_t vulkan_readback_ns;
    uint64_t vulkan_gpu_known_sequences;
    uint64_t vulkan_gpu_ns;
    uint64_t vulkan_starvation_ns;
    uint64_t matcher_cpu_ns;
    uint64_t publication_ns;
} Lardon3DResourceSequenceAggregate;

typedef struct {
    unsigned int cpu_id;
    unsigned int package_id;
    unsigned int core_id;
} Lardon3DResourceCpuTopologyEntry;

typedef struct {
    bool affinity_available;
    bool topology_available;
    size_t allowed_cpu_count;
    unsigned int allowed_cpu_ids[LARDON3D_RESOURCE_CPU_MAX];
    size_t topology_entry_count;
    Lardon3DResourceCpuTopologyEntry
        topology_entries[LARDON3D_RESOURCE_CPU_MAX];
} Lardon3DResourceCpuTopologyInput;

typedef struct {
    bool affinity_configured;
    bool affinity_attempted;
    bool affinity_active;
    bool runtime_thread_policy_active;
    bool mesa_shader_cache_disabled;
    bool externally_constrained;
    unsigned int compute_cpu_count;
    unsigned int reserved_cpu_count;
    uint64_t allowed_mask[LARDON3D_RESOURCE_CPU_MASK_WORDS];
    uint64_t compute_mask[LARDON3D_RESOURCE_CPU_MASK_WORDS];
    uint64_t reserved_mask[LARDON3D_RESOURCE_CPU_MASK_WORDS];
    char reason[LARDON3D_RESOURCE_DIAGNOSTIC_REASON_CAPACITY];
    char runtime_thread_policy_reason[
        LARDON3D_RESOURCE_DIAGNOSTIC_REASON_CAPACITY];
} Lardon3DResourceCpuPolicyDiagnostic;

typedef enum {
    LARDON3D_RESOURCE_DRIVER_POLICY_FAILED = 0,
    LARDON3D_RESOURCE_DRIVER_POLICY_DEFAULTED,
    LARDON3D_RESOURCE_DRIVER_POLICY_INHERITED_SAFE,
    LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE,
} Lardon3DResourceDriverPolicyResult;

bool lardon3d_resource_governor_internal_reserve_capability_available(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DTaskCapabilityEnvelope *envelope,
    Lardon3DResourceCapabilitySelection *selection,
    Lardon3DResourceReservation **reservation
);
bool lardon3d_resource_governor_internal_reserve_capability(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DTaskCapabilityEnvelope *envelope,
    Lardon3DResourceCapabilitySelection *selection,
    Lardon3DResourceReservation **reservation
);
bool lardon3d_resource_governor_internal_record_sequence(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    uint64_t wall_time_ns,
    size_t items_completed
);
bool lardon3d_resource_governor_internal_record_sequence_execution(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason
);
bool lardon3d_resource_governor_internal_record_sequence_execution_metrics(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason,
    const Lardon3DResourceExecutionMetrics *metrics
);
/* Adds fixed-size ephemeral evidence only. This operation never creates a
 * sequence observation, trains throughput feedback, or changes admission.
 * `item_count` permits deterministic overflow testing; Task execution passes
 * exactly one after each durable fallback publication. */
bool lardon3d_resource_governor_internal_record_fallback_items(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    Lardon3DResourceFallbackItemCause cause,
    uint64_t item_count
);
bool lardon3d_resource_governor_internal_last_diagnostic(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DResourceSequenceDiagnostic *diagnostic
);
bool lardon3d_resource_governor_internal_diagnostic_since(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    uint64_t after_serial,
    Lardon3DResourceSequenceDiagnostic *diagnostic
);
bool lardon3d_resource_governor_internal_sequence_aggregate(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DResourceSequenceAggregate *aggregate
);
bool lardon3d_resource_governor_internal_format_diagnostic(
    const Lardon3DResourceSequenceDiagnostic *diagnostic,
    char *text,
    size_t capacity
);
bool lardon3d_resource_governor_internal_sample_telemetry_raw(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceTelemetryRaw *raw,
    Lardon3DResourceHostTelemetry *telemetry
);
/* Reads only the retained DRM card identity below root. No card scan or
 * fallback is permitted; O_NOFOLLOW and the production strict parser apply. */
bool lardon3d_resource_governor_internal_read_gpu_busy_at_root(
    const char *root,
    unsigned int card_index,
    uint32_t *basis_points
);
bool lardon3d_resource_governor_internal_set_backend_available(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceBackend backend,
    bool available
);
bool lardon3d_resource_governor_internal_capability_hardware_safe(
    Lardon3DResourceGovernor *governor,
    const Lardon3DTaskCapability *capability
);

/* Governor owns the bounded topology snapshot and compute mask. Queue calls
 * apply only from its sole heavy-compute worker; callers/main threads must not
 * be constrained. Failure is observable and leaves Task scientific state
 * untouched. */
bool lardon3d_resource_governor_internal_cpu_policy(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceCpuPolicyDiagnostic *diagnostic
);
bool lardon3d_resource_governor_internal_apply_worker_affinity(
    Lardon3DResourceGovernor *governor
);
/* Must run on the startup thread before any application pthread is created.
 * An absent Mesa setting is defaulted to the safe value; an explicit safe
 * value is retained, while false/malformed values are rejected rather than
 * overwritten. The policy is harmless on non-Mesa drivers and is operational,
 * never scientific identity or persistence. */
Lardon3DResourceDriverPolicyResult
lardon3d_resource_governor_internal_configure_driver_policy(void);

/* Deterministic private injection seam. It replaces only ephemeral topology
 * policy and is rejected while CPU reservations are active. */
bool lardon3d_resource_governor_internal_configure_cpu_topology(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceCpuTopologyInput *input
);
void lardon3d_resource_governor_internal_force_worker_affinity_failure(
    Lardon3DResourceGovernor *governor,
    bool force_failure
);
/* Private parser seam for the exact bounded sysfs reader. The file must
 * contain one unsigned decimal token followed only by ASCII whitespace and
 * EOF; signs, overflow, tails, and capacity truncation are rejected. */
bool lardon3d_resource_governor_internal_read_topology_value_file(
    const char *path,
    unsigned int *value
);

typedef struct {
    unsigned int pressure_streak;
    unsigned int recovery_streak;
    unsigned int slow_start_streak;
} Lardon3DResourceGovernorInternalCounters;

bool lardon3d_resource_governor_internal_set_next_reservation_id(
    Lardon3DResourceGovernor *governor,
    uint64_t next_reservation_id
);
bool lardon3d_resource_governor_internal_set_diagnostic_serial(
    Lardon3DResourceGovernor *governor,
    uint64_t diagnostic_serial
);
bool lardon3d_resource_governor_internal_set_counters(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceGovernorInternalCounters *counters
);
bool lardon3d_resource_governor_internal_get_counters(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceGovernorInternalCounters *counters
);
bool lardon3d_resource_governor_internal_set_monotonic_now(
    Lardon3DResourceGovernor *governor,
    const struct timespec *now
);
void lardon3d_resource_governor_internal_force_capture_failure(
    Lardon3DResourceGovernor *governor,
    bool force_failure
);

#ifdef __cplusplus
}
#endif

#endif
