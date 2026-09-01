#ifndef LARDON3D_TUI_SSD_ASYNC_H
#define LARDON3D_TUI_SSD_ASYNC_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/resource_governor.h>
#include <lardon3d/ssd_controller.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lardon3DTuiSsdAsync Lardon3DTuiSsdAsync;

typedef enum {
    LARDON3D_TUI_SSD_ACTION_NONE = 0,
    /* Internal coalesced telemetry operation; F10 never selects it. */
    LARDON3D_TUI_SSD_ACTION_OBSERVE,
    LARDON3D_TUI_SSD_ACTION_ENABLE,
    LARDON3D_TUI_SSD_ACTION_DRAIN,
    LARDON3D_TUI_SSD_ACTION_CANCEL_DRAIN,
} Lardon3DTuiSsdAction;

typedef struct {
    bool running;
    Lardon3DTuiSsdAction action;
    bool result_known;
    Lardon3DSsdControlResult result;
    uint64_t generation;
    bool controller_snapshot_known;
    bool controller_snapshot_actionable;
    Lardon3DSsdSnapshot controller_snapshot;
    char reason[LARDON3D_SSD_REASON_CAPACITY];
} Lardon3DTuiSsdAsyncSnapshot;

typedef struct {
    bool (*monotonic_now_ns)(void *context, uint64_t *now_ns);
    bool (*snapshot)(void *context, Lardon3DSsdSnapshot *snapshot);
    Lardon3DSsdControlResult (*enable)(void *context);
    Lardon3DSsdControlResult (*disable)(void *context);
    bool (*cancel_drain)(void *context);
    void (*destroy)(void *context);
} Lardon3DTuiSsdAsyncProviderOps;

typedef struct {
    const Lardon3DTuiSsdAsyncProviderOps *ops;
    void *context;
} Lardon3DTuiSsdAsyncProvider;

/* The caller owns one object; TUI request/poll calls remain on the
 * ncurses/main thread. `controller` is borrowed and must outlive this object.
 * Exactly one joinable operation thread may exist. It runs only one bounded
 * synchronous controller transition; it never renders, schedules a Task,
 * polls in a loop, or becomes a second Queue. */
Lardon3DTuiSsdAsync *lardon3d_tui_ssd_async_create(
    Lardon3DSsdController *controller
);

/* Production binding. Both borrowed owners must outlive the async object.
 * Creation registers a conservative ERROR until the first validated worker
 * observation; each later worker outcome updates the Governor registry. The
 * registry is unregistered after the worker is joined and before destruction
 * returns. Existing create() remains an unbound compatibility entry point. */
Lardon3DTuiSsdAsync *lardon3d_tui_ssd_async_create_with_governor(
    Lardon3DSsdController *controller,
    Lardon3DResourceGovernor *governor
);

/* Deterministic provider seam. All five callbacks are required; destroy is
 * optional. Ownership of provider.context transfers only on success. Tests use
 * condition-controlled providers and never invoke real mount/swap operations.
 * Production callers normally use create() above. */
Lardon3DTuiSsdAsync *lardon3d_tui_ssd_async_create_with_provider(
    Lardon3DTuiSsdAsyncProvider provider
);

/* Checked production teardown. `operation` and `*operation` must be non-NULL.
 * It joins the bounded worker, then unregisters the Governor binding before
 * destroying provider/synchronization state. On success it frees the object,
 * stores NULL, and returns true. If an exact scratch lease still prevents
 * unregister, it returns false and retains the stopped object plus binding so
 * the owner can release the lease and retry; no borrowed pointer is orphaned. */
bool lardon3d_tui_ssd_async_destroy_checked(
    Lardon3DTuiSsdAsync **operation
);

/* Compatibility teardown for objects made by the unbound create() or provider
 * constructor. Bound production owners must use destroy_checked() so a failed
 * unregister remains observable and retryable. A
 * D-Bus call is not asynchronously cancelled: production controller timeouts
 * bound the join and preserve its exact side-effect verification contract. */
void lardon3d_tui_ssd_async_destroy(Lardon3DTuiSsdAsync *operation);

/* Starts one exact controller-authorized transition from the last validated
 * snapshot. Requests are rejected while busy, after shutdown, for invalid
 * actions, or when the corresponding capability flag is absent. */
bool lardon3d_tui_ssd_async_request(
    Lardon3DTuiSsdAsync *operation,
    Lardon3DTuiSsdAction action
);

/* Starts one background telemetry refresh only when the one-second cache is
 * due and the sole worker is idle. Returning true means the request was
 * accepted, already cached, or coalesced behind an active control operation;
 * it never performs controller/D-Bus work on the caller/ncurses thread. */
bool lardon3d_tui_ssd_async_refresh(Lardon3DTuiSsdAsync *operation);

/* Poll is non-blocking except for joining a worker already known complete.
 * Output is always initialized; caller owns the bounded copy. Controller
 * telemetry remains unknown until one background refresh completes. A later
 * failed controller refresh publishes its truthful ERROR snapshot. Malformed
 * provider output is replaced by a bounded ERROR copy with
 * controller_snapshot_actionable=false, so it cannot authorize F10. */
bool lardon3d_tui_ssd_async_poll(
    Lardon3DTuiSsdAsync *operation,
    Lardon3DTuiSsdAsyncSnapshot *snapshot
);

/* Waits on the operation condition using CLOCK_MONOTONIC, then joins a
 * completed worker. A timeout returns false without cancelling or losing
 * ownership. This is also the deterministic validation seam. */
bool lardon3d_tui_ssd_async_wait_idle(
    Lardon3DTuiSsdAsync *operation,
    uint64_t timeout_ns
);

/* Selects the exact F10 transition solely from controller-owned capability
 * flags. State/activity/path fields are presentation only and never grant
 * authority. Malformed or ambiguous capability combinations return false. */
bool lardon3d_tui_ssd_action_for_snapshot(
    const Lardon3DSsdSnapshot *snapshot,
    Lardon3DTuiSsdAction *action
);

const char *lardon3d_tui_ssd_action_name(Lardon3DTuiSsdAction action);

#ifdef __cplusplus
}
#endif

#endif
