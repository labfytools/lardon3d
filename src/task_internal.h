#ifndef LARDON3D_TASK_INTERNAL_H
#define LARDON3D_TASK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/task.h>

#include "resource_governor_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The envelope is operation-owned policy reconstructed from Task kind/version
 * and runtime support. Task copies it; it is neither durable state nor part of
 * the public Task ABI. The selected capability is immutable until the Task
 * crosses its next sequence boundary. */
bool lardon3d_task_internal_set_capability_envelope(
    Lardon3DTask *task,
    const Lardon3DTaskCapabilityEnvelope *envelope
);

/* Confirms the universal fixed default after generic reconstruction. Optional
 * kind-owned private hooks may then replace it using runtime/business context;
 * this avoids guessing scientific eligibility from a resource estimate. */
bool lardon3d_task_internal_enable_known_capabilities(Lardon3DTask *task);

/* Queue and sequence_break are the only admission owners. A successful call
 * records the one selection associated with reservation so task_start cannot
 * install a contract from a different capability. */
bool lardon3d_task_internal_reserve_available(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
);

bool lardon3d_task_internal_record_sequence(
    Lardon3DTask *task,
    uint64_t wall_time_ns,
    size_t items_completed
);
bool lardon3d_task_internal_record_sequence_execution(
    Lardon3DTask *task,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason
);
bool lardon3d_task_internal_record_sequence_execution_metrics(
    Lardon3DTask *task,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason,
    const Lardon3DResourceExecutionMetrics *metrics
);

/* Matcher calls this only after the exact candidate pair is durable. Task
 * owns a bounded current-run high-water mark so an in-process retry cannot
 * count the same ordered pair twice; restart intentionally reconstructs no
 * operational telemetry. The operation does not create throughput feedback. */
bool lardon3d_task_internal_record_fallback_item(
    Lardon3DTask *task,
    uint64_t candidate_pair_id,
    Lardon3DResourceFallbackItemCause cause
);

/* Copies the immutable operational selection installed for the executing
 * sequence. Task retains ownership; callbacks use the copy only to honor
 * private dimensions (currently Matcher inflight) absent from the stable
 * public execution-contract ABI. Observation never changes admission. */
bool lardon3d_task_internal_execution_selection(
    const Lardon3DTask *task,
    Lardon3DResourceCapabilitySelection *selection
);

#ifdef LARDON3D_TASK_TESTING
/* Deterministically exercises the post-reserve association failure that is
 * otherwise unreachable without corrupting private Task state. */
bool lardon3d_task_internal_test_force_sequence_association_mismatch(
    Lardon3DTask *task
);
bool lardon3d_task_internal_test_has_reservation_ownership(
    Lardon3DTask *task
);
unsigned int lardon3d_task_internal_test_association_failure_releases(
    Lardon3DTask *task
);
#endif

#ifdef __cplusplus
}
#endif

#endif
