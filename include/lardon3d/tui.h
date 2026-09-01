#ifndef LARDON3D_TUI_H
#define LARDON3D_TUI_H

#include <stdbool.h>

#include <lardon3d/app_state.h>

typedef struct Lardon3DSsdController Lardon3DSsdController;
typedef struct Lardon3DTuiSsdAsync Lardon3DTuiSsdAsync;

#ifdef __cplusplus
extern "C" {
#endif

bool lardon3d_tui_init(void);
bool lardon3d_tui_run(Lardon3DAppState *state);
/* Additive compatibility entry point. The controller is borrowed for the call
 * and remains outside the frozen AppState ABI; NULL preserves the legacy run.
 * It creates an unbound physical observer, so production code that needs
 * Governor scratch orchestration uses run_with_ssd_operation() instead. The
 * App owner must keep the controller alive until this function returns. */
bool lardon3d_tui_run_with_ssd(
    Lardon3DAppState *state,
    Lardon3DSsdController *ssd_controller
);
/* Production borrowed-operation entry point. The application owns `operation`
 * across this call and the TUI never destroys it. After the UI returns, the
 * owner must first destroy/join the sole Task Queue (so every Task-owned
 * scratch lease is released), then destroy the operation with
 * lardon3d_tui_ssd_async_destroy_checked(), then its controller and Governor.
 * `state` must be non-NULL; a bound operation must use that state's Governor.
 * NULL preserves a TUI without external-storage observation/control. Returns
 * false for invalid input or a runtime/observation/control-owner failure. */
bool lardon3d_tui_run_with_ssd_operation(
    Lardon3DAppState *state,
    Lardon3DTuiSsdAsync *operation
);
void lardon3d_tui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
