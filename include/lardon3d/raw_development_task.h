#ifndef LARDON3D_RAW_DEVELOPMENT_TASK_H
#define LARDON3D_RAW_DEVELOPMENT_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/task_kind_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_RAW_DEVELOPMENT_TASK_KIND "raw.develop"
enum { LARDON3D_RAW_DEVELOPMENT_TASK_KIND_VERSION = 1 };
#define LARDON3D_RAW_DEVELOPMENT_BATCH_TASK_KIND "raw.develop.batch"
enum { LARDON3D_RAW_DEVELOPMENT_BATCH_TASK_KIND_VERSION = 1 };

/* Create one durable, initially unqueued S3-B1 Task for the exact existing
 * Capture-owned SOURCE RAW asset. The two IDs are immutable and are never
 * inferred from paths, hashes, names, images, or Task identity. task_id is
 * required and receives the durable operational ID. The caller owns the
 * returned Task and must enqueue or destroy it. */
Lardon3DTask *lardon3d_project_create_raw_development_task(
    Lardon3DAppState *state, uint64_t capture_id, uint64_t source_asset_id,
    uint64_t *task_id);

/* Create and transfer one durable RAW Task to state's existing Queue. Queue
 * admission owns the Task's bounded CPU/RAM/I/O reservation until callback
 * completion, failure, or cancellation; no worker or scheduler is created. */
bool lardon3d_project_enqueue_raw_development(
    Lardon3DAppState *state, uint64_t capture_id, uint64_t source_asset_id,
    uint64_t *task_id);

/* Reconstruct callback state only from the validated typed v22 row. snapshot,
 * reconstruction context, and binding are required and caller-owned. The
 * binding owns its returned context until the runtime invokes its destroyer. */
bool lardon3d_raw_development_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

/* Create one durable, initially unqueued parent Task for the exact immutable
 * selected execution. The selected execution, not Task identity, owns item
 * order, explicit Capture/SOURCE RAW identity, and the durable cursor. Queue
 * admission bounds each parallel window; the caller owns the returned Task. */
Lardon3DTask *lardon3d_project_create_raw_development_batch_task(
    Lardon3DAppState *state, uint64_t selected_execution_id, uint64_t *task_id);

/* Create and transfer the parent Task to state's existing sole Queue. The
 * Queue/Governor reservation covers only one bounded window at a time and is
 * released/re-established at each sequence boundary. */
bool lardon3d_project_enqueue_raw_development_batch(
    Lardon3DAppState *state, uint64_t selected_execution_id, uint64_t *task_id);

/* Reconstruct only from the validated v24 Task -> selected execution row.
 * binding owns its returned context until the runtime invokes its destroyer. */
bool lardon3d_raw_development_batch_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

#ifdef __cplusplus
}
#endif

#endif
