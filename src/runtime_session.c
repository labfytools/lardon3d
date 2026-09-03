#include <stdio.h>

#include <lardon3d/project.h>
#include <lardon3d/runtime_session.h>
#include <lardon3d/task_queue.h>

bool
lardon3d_runtime_project_boundary(
    Lardon3DAppState *state,
    size_t queue_capacity
)
{
    if (!state || !state->resource_governor || queue_capacity == 0) {
        return false;
    }

    /* INVARIANT: callbacks may dereference Project DB until Queue destroy
     * returns. Keeping state->task_queue published during destroy also
     * preserves the documented read-only callback reentrancy window. */
    lardon3d_task_queue_destroy(state->task_queue);
    state->task_queue = NULL;
    if (state->project_loaded || state->project_db) {
        lardon3d_project_close(state);
    }

    state->task_queue = lardon3d_task_queue_create(
        state->resource_governor, queue_capacity);
    if (!state->task_queue) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Error: unable to recreate the task queue.");
        return false;
    }
    return true;
}
