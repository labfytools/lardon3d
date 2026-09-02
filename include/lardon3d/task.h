#ifndef LARDON3D_TASK_H
#define LARDON3D_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/resource_governor.h>

enum {
    LARDON3D_TASK_NAME_CAPACITY = 128,
    LARDON3D_TASK_MESSAGE_CAPACITY = 256,
    LARDON3D_TASK_KIND_CAPACITY = 65,
};

typedef enum {
    TASK_PENDING = 0,
    TASK_RUNNING,
    TASK_PAUSED,
    TASK_CANCELLED,
    TASK_FAILED,
    TASK_COMPLETED
} Lardon3DTaskState;

typedef struct Lardon3DTask Lardon3DTask;
typedef bool (*Lardon3DTaskCallback)(Lardon3DTask *task, void *userdata);
typedef void (*Lardon3DTaskUserdataDestroy)(void *userdata);
typedef void (*Lardon3DTaskFinishedCallback)(
    const Lardon3DTask *task,
    void *userdata
);

typedef struct {
    size_t batch_size;
    uint64_t memory_bytes;
    uint64_t gpu_memory_bytes;
    unsigned int cpu_threads;
    unsigned int gpu_slots;
    unsigned int io_slots;
} Lardon3DTaskExecutionContract;

/* Historical ABI snapshot. Its field order and size are frozen: additive
 * runtime observability belongs to Lardon3DTaskObservation below. */
typedef struct {
    uint64_t id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    unsigned int progress;
    Lardon3DTaskState state;
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    struct timespec started_at;
    struct timespec finished_at;
} Lardon3DTaskSnapshot;

/* Additive, caller-owned, mutex-consistent runtime observation. The legacy
 * prefix is deliberately repeated rather than extending TaskSnapshot: old
 * binaries must never receive a write larger than their compiled object.
 * Typed identity, durable progress, sequence count and the installed
 * execution contract are operational observations only. Durable counts exist
 * only after the typed owner publishes its committed prefix; they are never
 * inferred from generic percentage, name, or message. When
 * has_execution_contract is false every contract field is zero. */
typedef struct {
    uint64_t id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    unsigned int progress;
    Lardon3DTaskState state;
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    struct timespec started_at;
    struct timespec finished_at;
    bool has_task_kind;
    char task_kind[LARDON3D_TASK_KIND_CAPACITY];
    uint32_t task_kind_version;
    bool durable_progress_known;
    uint64_t durable_completed;
    uint64_t durable_total;
    unsigned int sequence_count;
    bool has_execution_contract;
    Lardon3DTaskExecutionContract execution_contract;
} Lardon3DTaskObservation;

typedef struct {
    uint64_t id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    Lardon3DResourceEstimate estimate;
    unsigned int progress;
    Lardon3DTaskState saved_state;
    Lardon3DTaskState recovery_state;
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    struct timespec started_at;
    struct timespec finished_at;
    unsigned int sequence_count;
} Lardon3DTaskDurableSnapshot;

Lardon3DTask *lardon3d_task_create(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DTaskCallback callback,
    void *userdata
);
Lardon3DTask *lardon3d_task_create_typed(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DTaskCallback callback,
    void *userdata,
    Lardon3DTaskUserdataDestroy userdata_destroy
);
void lardon3d_task_destroy(Lardon3DTask *task);
/* Executes the callback on the calling thread. The callback runs outside
 * the Task mutex; the execution contract and state remain owned by the Task.
 */
bool lardon3d_task_start(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation
);
void lardon3d_task_request_cancel(Lardon3DTask *task);
bool lardon3d_task_pause(Lardon3DTask *task);
bool lardon3d_task_resume(Lardon3DTask *task);
bool lardon3d_task_join(Lardon3DTask *task);
bool lardon3d_task_checkpoint(Lardon3DTask *task);
bool lardon3d_task_set_progress(
    Lardon3DTask *task,
    unsigned int progress,
    const char *message
);
/* Publishes an already-durable typed-business prefix as operational
 * observation and derives the generic percentage without overflow. The
 * caller must invoke this only after its own transaction/cursor commit;
 * Task/Queue do not persist or reinterpret these counts. A later ordinary
 * set_progress clears them rather than retaining a stale exact-looking value.
 * Untyped Tasks are rejected. Generic Task completion sets its percentage to
 * 100 but never fabricates a missing typed-business commit. total must be
 * positive and completed <= total. */
bool lardon3d_task_set_durable_progress(
    Lardon3DTask *task,
    uint64_t completed,
    uint64_t total,
    const char *message
);
bool lardon3d_task_fail(Lardon3DTask *task, const char *message);
bool lardon3d_task_snapshot(
    const Lardon3DTask *task,
    Lardon3DTaskSnapshot *snapshot
);
/* Copies one caller-owned coherent value under the Task mutex. A non-NULL
 * output is zeroed before validation, so invalid Task arguments never leave a
 * stale typed identity, durable count, or execution contract visible. */
bool lardon3d_task_observation(
    const Lardon3DTask *task,
    Lardon3DTaskObservation *observation
);
bool lardon3d_task_durable_snapshot(
    const Lardon3DTask *task,
    Lardon3DTaskDurableSnapshot *snapshot
);
Lardon3DTask *lardon3d_task_restore(
    const Lardon3DTaskDurableSnapshot *snapshot,
    Lardon3DTaskCallback callback,
    void *userdata
);
Lardon3DTask *lardon3d_task_restore_typed(
    const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DTaskCallback callback,
    void *userdata,
    Lardon3DTaskUserdataDestroy userdata_destroy
);
/* A successful typed restore transfers userdata/userdata_destroy ownership
 * to the Task. On failure, the caller retains ownership. */
bool lardon3d_task_kind_is_valid(const char *task_kind);
bool lardon3d_task_kind(
    const Lardon3DTask *task,
    char task_kind[LARDON3D_TASK_KIND_CAPACITY],
    uint32_t *task_kind_version
);
/* Invoked at most once, outside the Task mutex and after the terminal
 * reservation is released. Task userdata remains alive until the callback returns. */
bool lardon3d_task_set_finished_callback(
    Lardon3DTask *task,
    Lardon3DTaskFinishedCallback callback,
    void *userdata
);
uint64_t lardon3d_task_id(const Lardon3DTask *task);
bool lardon3d_task_assign_id(Lardon3DTask *task, uint64_t id);
bool lardon3d_task_resource_estimate(
    const Lardon3DTask *task,
    Lardon3DResourceEstimate *estimate
);
/* Execution does not receive admission policy: the Governor must confirm
 * the reservation before execution.
 */
bool lardon3d_task_execution_contract(
    const Lardon3DTask *task,
    Lardon3DTaskExecutionContract *contract
);
/* Releases the current reservation, obtains a new one from the Governor,
 * and updates the execution contract. Call only from the currently executing
 * callback. A Governor WAIT is temporary unavailability: this function waits
 * for a resource change and retries admission without failing the Task. Batch
 * bounds continue under the new admission. Returns false if the Task is
 * cancelled (TASK_CANCELLED), the Governor returns REJECT, or an internal
 * error fails the Task (TASK_FAILED). */
bool lardon3d_task_sequence_break(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation **out_reservation,
    Lardon3DTaskExecutionContract *out_contract
);
unsigned int lardon3d_task_sequence_count(const Lardon3DTask *task);
bool lardon3d_task_reject(Lardon3DTask *task, const char *message);
const char *lardon3d_task_state_name(Lardon3DTaskState state);

#endif
