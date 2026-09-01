#ifndef LARDON3D_RUNTIME_SESSION_H
#define LARDON3D_RUNTIME_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include <lardon3d/app_state.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Establishes the interactive Project/Queue lifetime boundary. The existing
 * sole Queue is cancelled, joined, and destroyed (including finished
 * callbacks and retained history) before the Project DB is closed; only then
 * is one empty Queue created with a fresh ID namespace. `state`, its Governor,
 * and a positive capacity are required. On allocation failure the Project is
 * still safely closed and state->task_queue is NULL. Main-thread owner only;
 * callers must release Queue/DB observers before entry and rebind them after.
 */
bool lardon3d_runtime_project_boundary(
    Lardon3DAppState *state,
    size_t queue_capacity
);

#ifdef __cplusplus
}
#endif

#endif
